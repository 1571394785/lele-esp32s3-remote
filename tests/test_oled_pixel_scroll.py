"""Verify the actual OLED marquee renderer shifts exactly one column per frame."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];source=(root/'main/ssd1306.c').read_text(encoding='utf-8')
def extract(name):
 m=re.search(r'^(?:static )?[^\n]+\b'+name+r'\([^;]*?\)\n\{',source,re.M);assert m,name
 i=m.end();depth=1
 while depth:depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[m.start():i]
harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
typedef int esp_err_t;
static uint8_t s_framebuf[1024],UNKNOWN_16X16[32],chinese[32],ascii[5];
static bool unicode_to_gb2312(uint32_t c,uint16_t*g){*g=(uint16_t)c;return true;}
static const uint8_t*gb2312_glyph(uint16_t gb){for(unsigned i=0;i<32;++i)chinese[i]=(uint8_t)(gb+17*i);return chinese;}
static const uint8_t*font_for_char(char c){for(unsigned i=0;i<5;++i)ascii[i]=(uint8_t)(((unsigned)c+7*i)&127);return ascii;}
'''
tests=r'''
int main(void){
 const char*text="AB中CDEFGH";const unsigned total=80,cycle=104;
 assert(ssd1306_text16_width(text)==80);
 memset(s_framebuf,0,sizeof(s_framebuf));assert(!ssd1306_draw_text16_marquee(0,0,128,text,99));
 uint8_t upper[80],lower[80];memcpy(upper,s_framebuf,80);memcpy(lower,s_framebuf+128,80);
 // Test each sub-glyph offset, spaces, wrapping, and clipping on both pages.
 for(unsigned step=0;step<cycle*2;++step){
  memset(s_framebuf,0xa5,sizeof(s_framebuf));
  assert(!ssd1306_draw_text16_marquee(2,16,40,text,step));
  for(unsigned x=0;x<128;++x){
   if(x<16||x>=56){assert(s_framebuf[256+x]==0xa5&&s_framebuf[384+x]==0xa5);continue;}
   unsigned origin=(step+x-16)%cycle;
   assert(s_framebuf[256+x]==(origin<total?upper[origin]:0));
   assert(s_framebuf[384+x]==(origin<total?lower[origin]:0));
  }
  for(unsigned x=0;x<256;++x)assert(s_framebuf[x]==0xa5);
  for(unsigned x=512;x<1024;++x)assert(s_framebuf[x]==0xa5);
 }
 // Short strings are stationary regardless of the frame counter.
 memset(s_framebuf,0,sizeof(s_framebuf));ssd1306_draw_text16_marquee(0,0,128,text,0);
 uint8_t first[256];memcpy(first,s_framebuf,256);ssd1306_draw_text16_marquee(0,0,128,text,1000);assert(!memcmp(first,s_framebuf,256));
 assert(ssd1306_draw_text16_marquee(7,0,20,text,0)==ESP_ERR_INVALID_ARG);
 assert(ssd1306_draw_text16_marquee(0,120,20,text,0)==ESP_ERR_INVALID_ARG);
 assert(ssd1306_draw_text16_marquee(0,-1,20,text,0)==ESP_ERR_INVALID_ARG);
 puts("PASS: 208 exact pixel offsets, partial Chinese/ASCII glyphs, viewport guards, gap/wrap, stationary short text and invalid bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='oled_pixel_test_') as d:
 t=Path(d);(t/'test.c').write_text(harness+'\n'.join(extract(n) for n in ['decode_utf8','ssd1306_text16_width','draw_text16_clipped','ssd1306_draw_text16_marquee'])+tests,encoding='utf-8')
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(t/'test.c'),'-o',str(t/'test.exe')],check=True)
 subprocess.run([str(t/'test.exe')],check=True)
