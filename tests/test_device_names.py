"""Exercise real name storage, HTTP handler, escaping and OLED device-row highlighting."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
web=(root/'main/web_control.c').read_text(encoding='utf-8')
display=(root/'main/app_display.c').read_text(encoding='utf-8')
def extract(source,name):
 m=re.search(r'^static [^\n]+\b'+name+r'\([^;]*?\)\n\{',source,re.M);assert m,name
 i=m.end();depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[m.start():i]
headers={
'esp_err.h':'#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG -2\n#define ESP_ERR_NVS_NOT_FOUND -3\n',
'nvs.h':'''#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
int nvs_open(const char*,int,int*);
int nvs_get_blob(int,const char*,void*,size_t*);
int nvs_set_blob(int,const char*,const void*,size_t);
int nvs_commit(int);
void nvs_close(int);
''',
'freertos/FreeRTOS.h':'''#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
'''}
harness=r'''
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device_names.c"
#include "mode_manager.h"
#include "custom_mode.h"
#define ESP_ERR_NO_MEM -4
#define ESP_RETURN_ON_ERROR(expr,...) do{int r=(expr);if(r)return r;}while(0)
#define ESP_LOGI(...) ((void)0)
#define OLED_WIDTH 128
#define TAG "test"
#define HTTPD_400_BAD_REQUEST 400
static unsigned char saved[512],pending[512];static size_t saved_len,pending_len;
static bool fail_set,fail_commit;
int nvs_open(const char*n,int m,int*h){assert(!strcmp(n,"devnames"));*h=1;return m==0&&!saved_len?ESP_ERR_NVS_NOT_FOUND:0;}
int nvs_get_blob(int h,const char*k,void*d,size_t*n){(void)h;assert(!strcmp(k,"names"));if(!saved_len)return -3;assert(*n>=saved_len);memcpy(d,saved,saved_len);*n=saved_len;return 0;}
int nvs_set_blob(int h,const char*k,const void*d,size_t n){(void)h;(void)k;if(fail_set)return -1;memcpy(pending,d,n);pending_len=n;return 0;}
int nvs_commit(int h){(void)h;if(fail_commit)return -1;memcpy(saved,pending,pending_len);saved_len=pending_len;return 0;}
void nvs_close(int h){(void)h;}
typedef struct {const char*body;int status;} httpd_req_t;
static int receive_post_body(httpd_req_t*r,char**out){*out=malloc(strlen(r->body)+1);strcpy(*out,r->body);return 0;}
static int httpd_resp_send_err(httpd_req_t*r,int status,const char*m){(void)m;r->status=status;return 0;}
static int httpd_resp_send_500(httpd_req_t*r){r->status=500;return 0;}
static int send_save_redirect(httpd_req_t*r,const char*t){assert(!strcmp(t,"wifi"));r->status=303;return 0;}
static char page_html[40000];
static int httpd_resp_set_type(httpd_req_t*r,const char*t){(void)r;(void)t;return 0;}
static int httpd_resp_sendstr_chunk(httpd_req_t*r,const char*t){(void)r;if(t){assert(strlen(page_html)+strlen(t)<sizeof(page_html));strcat(page_html,t);}return 0;}
const char*custom_mode_get_wifi_ssid(void){return "Home";}
const char*custom_mode_get_wifi_password(void){return "";}
bool custom_mode_get_button_config(button_key_t k,custom_button_config_t*c){(void)k;memset(c,0,sizeof(*c));return true;}
static uint32_t s_device_scroll_step;
static int ssd1306_draw_text16_marquee(int page,int col,int width,const char*t,uint32_t step){(void)page;(void)col;(void)width;(void)t;(void)step;return 0;}
static int invert_page,arrow_page;
static int ssd1306_draw_text16(int page,int col,const char*t){(void)page;(void)col;(void)t;return 0;}
static int ssd1306_fill_rect(int x,int y,int w,int h,bool on){assert(x==2&&w>=2&&w<=5&&h==1&&on);arrow_page=(y/16)*2;return 0;}
static int ssd1306_draw_scrollbar(int a,int b,int c,int d,int e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
static int ssd1306_invert_area(int a,int b,int c,int d){(void)b;(void)c;(void)d;invert_page=a;return 0;}
'''
tests=r'''
static void expect(unsigned slot,const char*value){char out[49];device_names_get(slot,out,sizeof(out));assert(!strcmp(out,value));}
int main(void){
 assert(!device_names_init());expect(0,"设备1");expect(2,"设备3");
 char names[3][49]={" iPhone ","办公电脑","客厅电脑"};
 assert(!device_names_save(names));expect(0,"iPhone");
 memset(s_names,0,sizeof(s_names));assert(!device_names_init());expect(1,"办公电脑");
 // Invalid batch or a failed write must retain all three old names.
 strcpy(names[1],"bad\nname");assert(device_names_save(names)==ESP_ERR_INVALID_ARG);expect(0,"iPhone");
 strcpy(names[1],"新电脑");fail_commit=true;assert(device_names_save(names)!=0);expect(1,"办公电脑");
 fail_commit=false;assert(!device_names_init());expect(1,"办公电脑");
 fail_set=true;assert(device_names_save(names)!=0);fail_set=false;expect(1,"办公电脑");
 strcpy(names[1],"12345678901234567");assert(device_names_save(names)==ESP_ERR_INVALID_ARG);
 strcpy(names[1],"\xc0\xaf");assert(device_names_save(names)==ESP_ERR_INVALID_ARG);
 strcpy(names[1],"中文中文中文中文中文中文中文中文");assert(!device_names_save(names));
 // URL encoding, all-name save, HTML attribute escaping, and POST validation.
 httpd_req_t req={"device1=iPhone&device2=%E5%8A%9E%E5%85%AC%E7%94%B5%E8%84%91&device3=Alice%27s+PC",0};
 assert(!save_devices_post_handler(&req)&&req.status==303);expect(1,"办公电脑");expect(2,"Alice's PC");
 char escaped[100];html_attr_escape(escaped,sizeof(escaped),"'\"<>&");assert(!strcmp(escaped,"&#39;&quot;&lt;&gt;&amp;"));
 req=(httpd_req_t){"device1=bad&device2=ok",0};save_devices_post_handler(&req);assert(req.status==400);expect(0,"iPhone");
 req=(httpd_req_t){"device1=12345678901234567&device2=ok&device3=ok",0};save_devices_post_handler(&req);assert(req.status==400);expect(0,"iPhone");
 req=(httpd_req_t){"device1=%E5%8A%9E%E5%85%AC&device2=ok&device3=ok",0};fail_commit=true;save_devices_post_handler(&req);fail_commit=false;assert(req.status==500);expect(0,"iPhone");
 memset(s_names,0,sizeof(s_names));assert(!device_names_init());expect(2,"Alice's PC");
 assert(!root_get_handler(&req));
 assert(strstr(page_html,"action='/save_devices'")&&strstr(page_html,"name='device1'")&&strstr(page_html,"name='device3'"));
 assert(strstr(page_html,"value='Alice&#39;s PC'"));
 unsigned forms=0;bool in_form=false;
 for(char*p=page_html;*p;++p){if(!strncmp(p,"<form ",6)){assert(!in_form);in_form=true;++forms;}if(!strncmp(p,"</form>",7)){assert(in_form);in_form=false;}}
 assert(forms==3&&!in_form);
 mode_view_t view={.show_devices=true,.active_device=0,.selected=2,.item_count=3,.items={"iPhone","电脑","平板"}};
 assert(!draw_menu_list(&view,0));assert(invert_page==2&&arrow_page==6);
 view.active_device=1;assert(!draw_menu_list(&view,0));assert(invert_page==4&&arrow_page==6);
 view.blink_selected=true;invert_page=-1;arrow_page=-1;
 assert(!draw_menu_list(&view,500));assert(invert_page==-1&&arrow_page==6);
 assert(!draw_menu_list(&view,1000));assert(invert_page==4);
 view.blink_selected=false;invert_page=-1;
 assert(!draw_menu_list(&view,1500));assert(invert_page==4);
 assert(!device_names_reset());assert(!device_names_init());expect(0,"设备1");expect(1,"设备2");
 puts("PASS: names survive cold boot, UTF-8/limits, atomic batch save, NVS failures, form decoding/errors, HTML escaping, marquee and active-row inversion");
}
'''
with tempfile.TemporaryDirectory(prefix='device_names_test_') as d:
 t=Path(d)
 for name,value in headers.items():p=t/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(value)
 constants=web[web.index('static const button_key_t WEB_KEYS[]'):web.index('static int hex_value')]
 funcs=constants+'\n'+'\n'.join(extract(web,n) for n in ['hex_value','url_decode_range','find_param','html_attr_escape','save_devices_post_handler','root_get_handler'])+'\n'+extract(display,'draw_menu_list')
 (t/'test.c').write_text(harness+funcs+tests,encoding='utf-8')
 subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(t),'-I',str(root/'main'),str(t/'test.c'),'-o',str(t/'test.exe')],check=True)
 subprocess.run([str(t/'test.exe')],check=True)
