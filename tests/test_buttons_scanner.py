"""Run the actual timer-driven button scanner with a blocked foreground loop."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
headers={
'esp_err.h':'#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM -1\n',
'esp_check.h':'#pragma once\n#define ESP_RETURN_ON_ERROR(e,...) do{int r=(e);if(r)return r;}while(0)\n',
'esp_log.h':'#pragma once\n#define ESP_LOGI(tag,...) ((void)(tag))\n',
'freertos/FreeRTOS.h':'#pragma once\n#include <stddef.h>\n#define pdTRUE 1\n',
'freertos/queue.h':'''#pragma once
typedef void*QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned,unsigned);
int xQueueSend(QueueHandle_t,const void*,unsigned);
int xQueueReceive(QueueHandle_t,void*,unsigned);
void vQueueDelete(QueueHandle_t);
''',
'esp_timer.h':'''#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef void*esp_timer_handle_t;
typedef struct{void(*callback)(void*);const char*name;bool skip_unhandled_events;}esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
int esp_timer_create(const esp_timer_create_args_t*,esp_timer_handle_t*);
int esp_timer_start_periodic(esp_timer_handle_t,uint64_t);
int esp_timer_delete(esp_timer_handle_t);
''',
'driver/gpio.h':'''#pragma once
#include <stdint.h>
typedef int gpio_num_t;
enum{GPIO_NUM_4=4,GPIO_NUM_5,GPIO_NUM_6,GPIO_NUM_7,GPIO_NUM_39=39,GPIO_NUM_40,GPIO_NUM_41,GPIO_NUM_42};
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct{uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;}gpio_config_t;
int gpio_config(const gpio_config_t*);
int gpio_get_level(int);
'''}
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "buttons.c"
static int levels[50];static int64_t now;static bool fail_queue,fail_timer;
static uint16_t masks[32];static unsigned begin,count,allocations,deletes;
int gpio_config(const gpio_config_t*c){assert(c->pull_up_en==1);return 0;}
int gpio_get_level(int pin){return levels[pin];}
QueueHandle_t xQueueCreate(unsigned len,unsigned size){assert(len==32&&size==2);if(fail_queue)return NULL;++allocations;return masks;}
int xQueueSend(QueueHandle_t q,const void*v,unsigned wait){(void)q;(void)wait;if(count==32)return 0;masks[(begin+count++)%32]=*(const uint16_t*)v;return 1;}
int xQueueReceive(QueueHandle_t q,void*v,unsigned wait){(void)q;(void)wait;if(!count)return 0;*(uint16_t*)v=masks[begin];begin=(begin+1)%32;--count;return 1;}
void vQueueDelete(QueueHandle_t q){(void)q;++deletes;count=0;}
int64_t esp_timer_get_time(void){return now;}
int esp_timer_create(const esp_timer_create_args_t*a,esp_timer_handle_t*h){assert(a->callback==scan_buttons&&a->skip_unhandled_events);if(fail_timer)return -2;*h=(void*)1;return 0;}
int esp_timer_start_periodic(esp_timer_handle_t h,uint64_t us){(void)h;assert(us==5000);return 0;}
int esp_timer_delete(esp_timer_handle_t h){(void)h;return 0;}
static void advance(unsigned ms){for(unsigned i=0;i<ms;i+=5){now+=5000;scan_buttons(NULL);}}
int main(void){
 for(unsigned i=0;i<50;++i)levels[i]=1;
 fail_queue=true;assert(buttons_init()==ESP_ERR_NO_MEM);fail_queue=false;
 fail_timer=true;assert(buttons_init()==-2&&deletes==1&&!s_mask_queue);fail_timer=false;
 assert(!buttons_init());unsigned original_allocations=allocations;assert(!buttons_init()&&allocations==original_allocations);
 advance(20);assert(!buttons_poll_mask());
 // Mechanical chatter shorter than 10 ms is discarded.
 for(unsigned i=0;i<8;++i){levels[5]=i%2;advance(5);}levels[5]=1;advance(20);assert(!count&&!buttons_poll_mask());
 // 30 ms tap happens entirely inside a long foreground/OLED/BLE stall.
 levels[5]=0;advance(30);levels[5]=1;advance(200);
 assert(count==2);assert(buttons_poll_mask()==BUTTON_MASK_UP);assert(!buttons_poll_mask());
 // Multiple quick taps and release edges remain ordered, including F1.
 for(unsigned i=0;i<3;++i){levels[41]=0;advance(30);levels[41]=1;advance(30);}
 assert(count==6);for(unsigned i=0;i<3;++i){assert(buttons_poll_mask()==BUTTON_MASK_FUNC1);assert(!buttons_poll_mask());}
 // Held keys do not repeat queue entries or lose the stable held state.
 levels[6]=0;advance(1000);assert(count==1);assert(buttons_poll_mask()==BUTTON_MASK_DOWN);assert(buttons_poll_mask()==BUTTON_MASK_DOWN);
 levels[6]=1;advance(20);assert(!buttons_poll_mask());
 // Millisecond wrap is safe for the time-based debounce.
 now=((int64_t)UINT32_MAX-5)*1000;levels[7]=0;advance(30);assert(buttons_poll_mask()==BUTTON_MASK_LEFT);
 levels[7]=1;advance(20);assert(!buttons_poll_mask());
 // A full queue retries its latest release as soon as a slot is available.
 s_queued_mask=BUTTON_MASK_UP;s_polled_mask=BUTTON_MASK_UP;
 uint16_t held=BUTTON_MASK_UP;while(count<32)assert(xQueueSend(s_mask_queue,&held,0));
 advance(20);assert(s_queued_mask==BUTTON_MASK_UP);
 buttons_poll_mask();advance(5);assert(s_queued_mask==0);
 while(count){buttons_poll_mask();}
 assert(!buttons_poll_mask());
 puts("PASS: timer/queue setup, bounce rejection, 30ms taps during 230ms stalls, ordered F1 edges, held state, timer wrap, full-queue release recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='buttons_timer_test_') as d:
 t=Path(d)
 for n,v in headers.items():p=t/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(v)
 (t/'test.c').write_text(harness)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(t),'-I',str(root/'main'),str(t/'test.c'),'-o',str(t/'test.exe')],check=True)
 subprocess.run([str(t/'test.exe')],check=True)
