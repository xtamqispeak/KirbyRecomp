#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "kirby_settings.h"
static uint64_t ticks;
uint64_t rt_ticks(void){return ticks;}

int main(int argc,char **argv) {
    kirby_settings_load();
    if(argc>1 && !strcmp(argv[1],"custom")) {
        assert(kirby_settings.window_width==1366 && kirby_settings.window_height==768);
        assert(kirby_settings.render_width==2560 && kirby_settings.render_height==1440);
        assert(kirby_settings.fps==120);
    } else {
        assert(kirby_settings.window_width==1280 && kirby_settings.window_height==720);
        assert(kirby_settings.render_width==1920 && kirby_settings.render_height==1080);
        assert(kirby_settings.fps==0);
    }
    kirby_settings.fps=0;
    uint8_t controller[64]={0};Cpu cpu={0};cpu.x[0]=(uintptr_t)controller;
    assert(!kirby_frame_hook(&cpu,0x7000017530ull)); // Original timing is untouched.
    if(kirby_settings.fps==0)assert(controller[0]==0 && controller[0x32]==0);
    assert(!kirby_frame_hook(&cpu,0x70000173d0ull));
    kirby_settings.fps=60;assert(kirby_frame_hook(&cpu,0x70000173d0ull));cpu.x[0]=(uintptr_t)controller;
    assert(kirby_frame_hook(&cpu,0x7000017530ull));assert(LD32((uintptr_t)controller)==0 && controller[0x32]==1);
    assert(kirby_frame_hook(&cpu,0x700001ab90ull));assert(cpu.x[0]==1);
    kirby_settings.fps=120;unsigned sum=0;
    for(unsigned frame=0;frame<120;frame++) {
        ticks=19200000+frame*160000;
        assert(!kirby_frame_hook(&cpu,0x70000172b0ull));
        assert(kirby_frame_hook(&cpu,0x700001ab90ull));unsigned delta=cpu.x[0];assert(delta<=1);sum+=delta;
        assert(kirby_frame_hook(&cpu,0x7000017560ull));float value;uint32_t bits=cpu.v[0];memcpy(&value,&bits,4);assert(value==delta);
        assert(kirby_frame_hook(&cpu,0x700001ab90ull));assert(cpu.x[0]==delta); // stable for multiple consumers
    }
    assert(sum==60);
    ticks+=19200000*2;kirby_frame_hook(&cpu,0x70000172b0ull);kirby_frame_hook(&cpu,0x700001ab90ull);assert(cpu.x[0]==6);
    uint64_t scheduler[2]={0,19200000};ticks=19200000+160000;cpu.x[0]=(uintptr_t)scheduler;
    assert(kirby_frame_hook(&cpu,0x700001af90ull));assert(cpu.x[0]==1);
    puts("PASS settings validation, 120 render frames / 60 fixed updates, stable deltas and hitch bound");return 0;
}
