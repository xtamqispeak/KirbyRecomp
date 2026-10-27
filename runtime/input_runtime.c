// Native SDL input and the public Npad SDK state ABI. Button numbers and the
// 0x28-byte state layout follow libnx hid.h and LASRecomp's MIT input bridge.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "input_runtime.h"
#include <math.h>

typedef struct { uint64_t sampling, buttons; int32_t lx,ly,rx,ry; uint32_t attributes,pad; } PadState;
_Static_assert(sizeof(PadState)==0x28,"Npad state ABI");
static SRWLOCK input_lock=SRWLOCK_INIT;
static uint64_t keys,buttons,script_buttons,frame_number,sampling;
static int32_t axes[4];
static SDL_Gamepad *gamepad;
static float vibration[2][4];
static int rumble_changed;
static volatile LONG capture_request;
int runtime_input_take_capture(void){return InterlockedExchange(&capture_request,0)!=0;}
void runtime_input_rumble(uint32_t handle,const float value[4]){
    unsigned side=(handle>>16)&0xff;if(side>1)return;
    AcquireSRWLockExclusive(&input_lock);
    for(unsigned i=0;i<4;++i)vibration[side][i]=isfinite(value[i])?value[i]:0;
    rumble_changed=1;ReleaseSRWLockExclusive(&input_lock);
}
void runtime_input_rumble_get(uint32_t handle,float value[4]){
    unsigned side=(handle>>16)&0xff;memset(value,0,16);if(side>1)return;
    AcquireSRWLockShared(&input_lock);memcpy(value,vibration[side],16);ReleaseSRWLockShared(&input_lock);
}
void input_rumble_poll(void){
    AcquireSRWLockExclusive(&input_lock);
    if(rumble_changed&&gamepad){
        float low=fmaxf(vibration[0][0],vibration[1][0]),high=fmaxf(vibration[0][2],vibration[1][2]);
        SDL_RumbleGamepad(gamepad,(Uint16)(fminf(fmaxf(low,0),1)*65535),(Uint16)(fminf(fmaxf(high,0),1)*65535),1000);
    }
    rumble_changed=0;ReleaseSRWLockExclusive(&input_lock);
}
void input_rumble_stop(void){
    AcquireSRWLockExclusive(&input_lock);if(gamepad)SDL_RumbleGamepad(gamepad,0,0,0);ReleaseSRWLockExclusive(&input_lock);
}
static const char *names[]={"A","B","X","Y","LSTICK","RSTICK","L","R","ZL","ZR","PLUS","MINUS","LEFT","UP","RIGHT","DOWN"};
static int key_bit(SDL_Scancode key){
    switch(key){
    case SDL_SCANCODE_Z:case SDL_SCANCODE_RETURN:return 0;
    case SDL_SCANCODE_X:case SDL_SCANCODE_BACKSPACE:return 1;
    case SDL_SCANCODE_C:return 2;case SDL_SCANCODE_V:return 3;
    case SDL_SCANCODE_Q:return 6;case SDL_SCANCODE_E:return 7;
    case SDL_SCANCODE_LSHIFT:return 8;case SDL_SCANCODE_RSHIFT:return 9;
    case SDL_SCANCODE_ESCAPE:return 10;case SDL_SCANCODE_TAB:return 11;
    case SDL_SCANCODE_LEFT:case SDL_SCANCODE_A:return 12;
    case SDL_SCANCODE_UP:case SDL_SCANCODE_W:return 13;
    case SDL_SCANCODE_RIGHT:case SDL_SCANCODE_D:return 14;
    case SDL_SCANCODE_DOWN:case SDL_SCANCODE_S:return 15;
    default:return -1;
    }
}
static int pad_bit(unsigned button){
    switch(button){
    case SDL_GAMEPAD_BUTTON_EAST:return 0;case SDL_GAMEPAD_BUTTON_SOUTH:return 1;
    case SDL_GAMEPAD_BUTTON_NORTH:return 2;case SDL_GAMEPAD_BUTTON_WEST:return 3;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:return 4;case SDL_GAMEPAD_BUTTON_RIGHT_STICK:return 5;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:return 6;case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:return 7;
    case SDL_GAMEPAD_BUTTON_START:return 10;case SDL_GAMEPAD_BUTTON_BACK:return 11;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:return 12;case SDL_GAMEPAD_BUTTON_DPAD_UP:return 13;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:return 14;case SDL_GAMEPAD_BUTTON_DPAD_DOWN:return 15;
    default:return -1;
    }
}
void input_handle_event(const SDL_Event *event){
    AcquireSRWLockExclusive(&input_lock);
    if(event->type==SDL_EVENT_KEY_DOWN||event->type==SDL_EVENT_KEY_UP){
        if(event->key.down&&event->key.scancode==SDL_SCANCODE_F12&&!event->key.repeat)InterlockedExchange(&capture_request,1);
        int bit=key_bit(event->key.scancode);
        if(bit>=0){if(event->key.down)keys|=1ull<<bit;else keys&=~(1ull<<bit);}
    }else if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST){keys=0;
    }else if(event->type==SDL_EVENT_GAMEPAD_ADDED&&!gamepad){
        gamepad=SDL_OpenGamepad(event->gdevice.which);
        if(gamepad)fprintf(stderr,"[input] controller %s\n",SDL_GetGamepadName(gamepad));
    }else if(event->type==SDL_EVENT_GAMEPAD_REMOVED&&gamepad&&SDL_GetGamepadID(gamepad)==event->gdevice.which){
        SDL_CloseGamepad(gamepad);gamepad=NULL;buttons=0;memset(axes,0,sizeof axes);
    }else if(gamepad&&(event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN||event->type==SDL_EVENT_GAMEPAD_BUTTON_UP)&&event->gbutton.which==SDL_GetGamepadID(gamepad)){
        int bit=pad_bit(event->gbutton.button);
        if(bit>=0){if(event->gbutton.down)buttons|=1ull<<bit;else buttons&=~(1ull<<bit);}
    }else if(gamepad&&event->type==SDL_EVENT_GAMEPAD_AXIS_MOTION&&event->gaxis.which==SDL_GetGamepadID(gamepad)){
        unsigned axis=event->gaxis.axis;int value=event->gaxis.value;
        if(axis<4)axes[axis]=abs(value)<6500?0:(axis==1||axis==3?-value:value);
        else if(axis==SDL_GAMEPAD_AXIS_LEFT_TRIGGER||axis==SDL_GAMEPAD_AXIS_RIGHT_TRIGGER){
            uint64_t mask=1ull<<(axis==SDL_GAMEPAD_AXIS_LEFT_TRIGGER?8:9);
            if(value>16000)buttons|=mask;else buttons&=~mask;
        }
    }
    ReleaseSRWLockExclusive(&input_lock);
}
// Deterministic integration-test presses: "start-end:BUTTON,start-end:BUTTON".
// No presses are injected unless explicitly enabled in the process environment.
void input_frame(uint64_t frame){
    uint64_t value=0;const char *script=getenv("KIRBY_INPUT_SCRIPT");
    while(script&&*script){
        unsigned long long start,end;char name[20];int consumed=0;
        if(sscanf(script,"%llu-%llu:%19[A-Z] %n",&start,&end,name,&consumed)!=3||!consumed)break;
        if(frame>=start&&frame<=end)for(unsigned i=0;i<16;++i)if(!strcmp(name,names[i]))value|=1ull<<i;
        script+=consumed;if(*script==',')++script;else if(*script)break;
    }
    const char *control=getenv("KIRBY_TEST_CONTROL");
    if(control){
        static char content[256],previous[256];
        if(!(frame&1)){
            FILE *file=fopen(control,"rb");
            if(file){size_t size=fread(content,1,sizeof content-1,file);content[size]=0;fclose(file);}
            if(strcmp(content,previous)){
                if(strstr(content,"SHOT="))InterlockedExchange(&capture_request,1);
                if(strstr(content,"DEBUG=")){extern volatile int g_debug_frame_request;g_debug_frame_request=1;}
                memcpy(previous,content,sizeof previous);
            }
        }
        char copy[256];memcpy(copy,content,sizeof copy);
        for(char *word=strtok(copy,", \r\n");word;word=strtok(NULL,", \r\n")){
            if(!strcmp(word,"AUTO_A")&&(frame%60)<9)value|=1;
            for(unsigned i=0;i<16;++i)if(!strcmp(word,names[i]))value|=1ull<<i;
        }
    }
    AcquireSRWLockExclusive(&input_lock);frame_number=frame;script_buttons=value;ReleaseSRWLockExclusive(&input_lock);
}
static void read_state(PadState *state,uint32_t id,int fullkey){
    memset(state,0,sizeof *state);
    AcquireSRWLockExclusive(&input_lock);
    state->sampling=++sampling;
    if(id==0&&fullkey){
        uint64_t held=keys|buttons|script_buttons;
        state->buttons=held;state->attributes=1;
        state->lx=axes[0];state->ly=axes[1];state->rx=axes[2];state->ry=axes[3];
        if(held&(15ull<<12)){
            state->lx=(((held>>14)&1)-((held>>12)&1))*32767;
            state->ly=(((held>>13)&1)-((held>>15)&1))*32767;
        }
        if(state->lx< -16000)state->buttons|=1ull<<16;
        if(state->ly>16000)state->buttons|=1ull<<17;
        if(state->lx>16000)state->buttons|=1ull<<18;
        if(state->ly< -16000)state->buttons|=1ull<<19;
        if(state->rx< -16000)state->buttons|=1ull<<20;
        if(state->ry>16000)state->buttons|=1ull<<21;
        if(state->rx>16000)state->buttons|=1ull<<22;
        if(state->ry< -16000)state->buttons|=1ull<<23;
        static uint64_t last;
        if(held!=last){fprintf(stderr,"[input] frame=%llu buttons=%llx\n",(unsigned long long)frame_number,(unsigned long long)held);last=held;}
    }
    ReleaseSRWLockExclusive(&input_lock);
}
static void single(Cpu *c,int fullkey){read_state((PadState *)(uintptr_t)c->x[0],*(uint32_t *)(uintptr_t)c->x[1],fullkey);}
static void multiple(Cpu *c,int fullkey){
    int count=(int)c->x[1];uint32_t id=*(uint32_t *)(uintptr_t)c->x[2];
    if(count>0)read_state((PadState *)(uintptr_t)c->x[0],id,fullkey);
    c->x[0]=count>0?1:0;
}
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state){single(c,1);}
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",dual_state){single(c,0);}
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",left_state){single(c,0);}
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state){single(c,0);}
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",right_state){single(c,0);}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states){multiple(c,1);}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",dual_states){multiple(c,0);}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",left_states){multiple(c,0);}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states){multiple(c,0);}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",right_states){multiple(c,0);}
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj",style){c->x[0]=*(uint32_t *)(uintptr_t)c->x[0]==0?1:0;}
