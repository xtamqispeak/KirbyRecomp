// Host bridge for the MIT LASRecomp NVN/Vulkan renderer. Zelda-specific texture
// edits, overlays and randomizer hooks are disabled; GPU execution is unchanged.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <ctype.h>
#include "hle.h"
#include "settings.h"
#include "nvn/nvn.h"
#include "nvn/pace.h"
#include "nvn/texpatch.h"
#include "nvn/recolour.h"
#include "ui/menu.h"
#include "probe_modules.h"
#include "graphics_bridge.h"
#include "kernel_runtime.h"
#include "kirby_settings.h"
#include "input_runtime.h"

void runtime_stop(const char *, uint64_t, int) __attribute__((noreturn));
Settings g_settings = {.aspect=ASPECT_ORIGINAL, .res_mode=1, .upscaler=UPSCALE_BILINEAR, .volume=1, .reverb=true};
bool g_gpu_integrated;
int g_default_upscaler, g_default_res_mode = 1;
volatile int g_rt_unwind;
__thread uint64_t g_blocked_ns;
const char *g_romfs_dir = PROBE_ROMFS_ROOT;
Module g_modules[4]; int g_nmodules;
static char fatal_text[2048];
void rt_fatal_msg(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vsnprintf(fatal_text,sizeof fatal_text,fmt,ap); va_end(ap); fprintf(stderr,"[graphics-fatal] %s",fatal_text); }
void rt_fatal_trace(void) { runtime_stop("graphics_failure",0,95); }
void rt_unwind_point(void) {}
int rt_reboot_requested(void) { return 0; }
void rt_request_reboot(int kind) { (void)kind; runtime_stop("restart_requested",0,95); }
const char *larecomp_version(void) { return "KirbyRecomp-dev"; }
void settings_load(void) { kirby_settings_load(); g_settings.res_mode=RES_CUSTOM; g_settings.render_width=kirby_settings.render_width; g_settings.render_height=kirby_settings.render_height; g_settings.fullscreen=kirby_settings.fullscreen; }
void settings_save(void) {}
void settings_resolve_defaults(bool integrated) { g_gpu_integrated = integrated; }
const char *settings_language(void) { return "en-US"; }
uint64_t plat_now_ns(void) { LARGE_INTEGER a,b;QueryPerformanceCounter(&a);QueryPerformanceFrequency(&b);return (uint64_t)((__uint128_t)a.QuadPart*1000000000/b.QuadPart); }
void plat_sleep_until_ns(uint64_t target) { uint64_t now;while((now=plat_now_ns())<target){ uint64_t ms=(target-now)/1000000; if(ms>1) Sleep((DWORD)(ms-1)); else SwitchToThread(); } }
void plat_thread_guest(int priority) { (void)priority; }
void plat_thread_perf_core(int role) { (void)role; }
void plat_thread_realtime(uint64_t period) { (void)period; }
int plat_thread_realtime_check(uint64_t period) { (void)period;return 0; }
int plat_cpu_count(void) { SYSTEM_INFO info;GetSystemInfo(&info);return (int)info.dwNumberOfProcessors; }
uint64_t plat_thread_cpu_ns(void) { FILETIME a,b,k,u;if(!GetThreadTimes(GetCurrentThread(),&a,&b,&k,&u))return 0;ULARGE_INTEGER x={.LowPart=k.dwLowDateTime,.HighPart=k.dwHighDateTime},y={.LowPart=u.dwLowDateTime,.HighPart=u.dwHighDateTime};return(x.QuadPart+y.QuadPart)*100; }
void plat_commit(uint64_t a,uint64_t n) { if(!VirtualAlloc((void *)(uintptr_t)a,n,MEM_COMMIT,PAGE_READWRITE))runtime_stop("graphics_commit_failed",a,95); }
int plat_written_pages(uint64_t a,uint64_t n,uint64_t *p,int max,uint64_t *page) { (void)a;(void)n;(void)p;(void)max;*page=4096;return -1; }
int plat_protect(uint64_t a,uint64_t n,int permission) { DWORD old;return VirtualProtect((void *)(uintptr_t)a,n,permission==0?PAGE_NOACCESS:permission==1?PAGE_READONLY:PAGE_READWRITE,&old)?0:-1; }
void plat_terminate(void) { fflush(stderr);TerminateProcess(GetCurrentProcess(),0); }
void kirby_window_size(int *w,int *h) {*w=kirby_settings.window_width;*h=kirby_settings.window_height;}
void rt_window_icon(SDL_Window *window) { SDL_SetWindowTitle(window,"Super Kirby Clash Recomp - F1 Settings"); }
void kernel_hang_hooks(bool (*check)(uint64_t,char*,size_t),void (*state)(const char*)) { (void)check;(void)state; }
uint64_t kernel_current_thread_id(void) { return GetCurrentThreadId(); }
uint64_t kernel_heap_size(void); // supplied by boot runtime

typedef struct { const char *name; HostFn fn; } Entry;
__attribute__((section(".hle$a"),used)) static const Entry first[1];
__attribute__((section(".hle$z"),used)) static const Entry last[1];
static Entry slots[4096]; static unsigned nslots;
static struct { uint64_t address; HostFn function; } overrides[512];
static unsigned noverrides;
uint64_t loader_find_symbol(const char *name) {
    unsigned lo=0,hi=PROBE_EXPORT_COUNT;
    while(lo<hi){unsigned m=lo+(hi-lo)/2;int compare=strcmp(probe_exports[m].name,name);if(compare<0)lo=m+1;else hi=m;}
    return lo<PROBE_EXPORT_COUNT&&!strcmp(probe_exports[lo].name,name)?probe_exports[lo].address:0;
}
uint64_t guest_sym(const char *name) { uint64_t address=loader_find_symbol(name);if(!address){fprintf(stderr,"[missing-symbol] %s\n",name);runtime_stop("guest_symbol_missing",0,95);}return address; }
static volatile LONG64 host_heap_used;
void *hle_alloc(uint64_t size) {
    size=(size+63)&~63ull;
    uint64_t address=0x7092000000ull+(uint64_t)InterlockedAdd64(&host_heap_used,(LONG64)size)-size;
    if(address+size>0x70a0000000ull)runtime_stop("host_guest_heap_limit",address,95);
    plat_commit(address,size);memset((void *)(uintptr_t)address,0,(size_t)size);return(void *)(uintptr_t)address;
}
HostFn hle_lookup_impl(const char *name) {
    const Entry *a=first,*z=last; __asm__("" : "+r"(a), "+r"(z));
    for(const Entry *e=a;e<z;++e)if(e->name&&!strcmp(e->name,name))return e->fn;
    return NULL;
}
uint64_t hle_register(const char *name,HostFn fn) {
    for(unsigned i=0;i<nslots;++i)if(!strcmp(slots[i].name,name))return 0x7080001000ull+16*i;
    if(!fn) { fprintf(stderr,"[nvn-missing] %s\n",name);return 0; }
    if(nslots==4096)runtime_stop("hle_table_full",0,95);
    slots[nslots]=(Entry){strdup(name),fn};return 0x7080001000ull+16*nslots++;
}
static uint64_t bootstrap_pc;
static void hle_nvnDeviceGetProcAddress(Cpu *c);
void hle_nvnBootstrapLoader(Cpu *c) {
    const char *name=(const char *)(uintptr_t)c->x[0];
    c->x[0]=hle_register(name,!strcmp(name,"nvnDeviceGetProcAddress")?hle_nvnDeviceGetProcAddress:hle_lookup_impl(name));
}
static void hle_nvnDeviceGetProcAddress(Cpu *c) {
    const char *name=(const char *)(uintptr_t)c->x[1];
    c->x[0]=hle_register(name,!strcmp(name,"nvnDeviceGetProcAddress")?hle_nvnDeviceGetProcAddress:hle_lookup_impl(name));
}
int runtime_hle_dispatch(Cpu *c) {
    if(c->pc==PROBE_NV_INIT){fprintf(stderr,"[nvn] nv::InitializeGraphics uses the native Vulkan device\n");c->x[0]=0;return 1;}
    for(unsigned i=0;i<noverrides;++i)if(c->pc==overrides[i].address){overrides[i].function(c);return 1;}
    if(c->pc==bootstrap_pc){hle_nvnBootstrapLoader(c);return 1;}
    if(c->pc>=0x7080001000ull&&c->pc<0x7080001000ull+16*nslots){slots[(c->pc-0x7080001000ull)/16].fn(c);return 1;}
    return 0;
}
void runtime_graphics_init(void) {
    settings_load();
    bootstrap_pc=PROBE_NVN_BOOTSTRAP;
    for(unsigned i=0;i<PROBE_MODULE_COUNT;++i){g_modules[i].name=probe_modules[i].name;g_modules[i].base=probe_modules[i].base;g_modules[i].size=probe_modules[i].size;}
    g_nmodules=PROBE_MODULE_COUNT;
    const Entry *a=first,*z=last;__asm__("" : "+r"(a), "+r"(z));
    for(const Entry *e=a;e<z;++e)if(e->name){uint64_t address=loader_find_symbol(e->name);if(address&&noverrides<512){overrides[noverrides].address=address;overrides[noverrides++].function=e->fn;}}
    fprintf(stderr,"[hle] %u SDK exports bound to native implementations\n",noverrides);
}
void guest_call(Cpu *c,uint64_t address) { uint64_t lr=c->x[30];c->x[30]=0;c->pc=address;rt_dispatch(c);c->x[30]=lr; }

void pace_frame(void) { while(kirby_settings_open()) Sleep(10); static __thread uint64_t last;uint64_t next=last+1000000000ull/(kirby_settings.fps?kirby_settings.fps:60),now=plat_now_ns();if(next>now)plat_sleep_until_ns(next);last=next>now?next:now; }
uint64_t pace_end_counter(const uint64_t *a,const uint64_t *b,uint64_t value){(void)a;(void)b;return value;}
bool pace_is_slow(void){return false;}
uint64_t pace_frames_over(void){return 0;}
void pace_hitch(const char *reason){(void)reason;}
void pace_settings_changed(const char *reason){(void)reason;}
int texpatch_identify(const NvnTexture *t){(void)t;return -1;}
uint32_t texpatch_generation(void){return 0;}
void texpatch_apply(int id,uint32_t lv,uint8_t *b,uint32_t w,uint32_t h,uint32_t row){(void)id;(void)lv;(void)b;(void)w;(void)h;(void)row;}
bool texpatch_is_item_cursor(int id){(void)id;return false;}
int texpatch_bottle_kind(int id){(void)id;return 0;}
int texpatch_heart_kind(int id){(void)id;return 0;}
void texpatch_heart_tint(int id,float t[8]){(void)id;(void)t;}
NvnTexture *texpatch_hint_icon(void){return NULL;}
void recolour_add(int cls[2],int c){(void)cls;(void)c;}
int recolour_class_of(int id){(void)id;return 0;}
void recolour_tint_draw(const int cls[2],float t[16]){(void)cls;(void)t;}
int game_in_play(void){return 0;}
const char *game_level_name(void){return "Kirby";}
bool widescreen_ui_pane_partial(const float *u,uint64_t size){(void)u;(void)size;return false;}
bool hint_sheet_icon_draw(const float *u,uint32_t size){(void)u;(void)size;return false;}
void escape_bottle_tint(int kind,const float *u,uint32_t size,float t[8]){(void)kind;(void)u;(void)size;(void)t;}
void input_led_poll(void){}
void input_led_stop(void){}
bool input_user_blocked(const SDL_Event *e){(void)e;return false;}
void input_cursor_event(const SDL_Event *e){(void)e;}
void input_apply_background(void){}
void input_load_user_mappings(void){}
bool menu_handle_event(const SDL_Event *e){
    extern SDL_Window *ui_window(void);
    if(e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat){
        if(e->key.scancode==SDL_SCANCODE_F1){runtime_input_clear();kirby_settings_window(ui_window());return true;}
        if(e->key.scancode==SDL_SCANCODE_F11){SDL_Window *w=ui_window();SDL_SetWindowFullscreen(w,!(SDL_GetWindowFlags(w)&SDL_WINDOW_FULLSCREEN));return true;}
    }
    return kirby_settings_open();
}
bool menu_is_open(void){return kirby_settings_open();}
void menu_apply_window_requests(void *w){(void)w;kirby_settings_pump();}
void menu_render(void *cmd,uint32_t w,uint32_t h){(void)cmd;(void)w;(void)h;}
void menu_test_open(void){}
static uint32_t vsync_handle;
static uint64_t native_window;
static unsigned __stdcall vsync_entry(void *arg) {
    (void)arg;uint64_t next=plat_now_ns();for(;;){next+=1000000000ull/(kirby_settings.fps?kirby_settings.fps:60);plat_sleep_until_ns(next);runtime_event_set(vsync_handle,1);}return 0;
}
HLE_FUNC("_ZN2nn2vi10InitializeEv",vi_init){(void)c;}
HLE_FUNC("_ZN2nn2vi8FinalizeEv",vi_fini){(void)c;}
HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_display){*(uint64_t *)(uintptr_t)c->x[0]=(uint64_t)(uintptr_t)hle_alloc(256);c->x[0]=0;}
HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE",vi_close){(void)c;}
HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer){*(uint64_t *)(uintptr_t)c->x[0]=(uint64_t)(uintptr_t)hle_alloc(256);c->x[0]=0;}
HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE",vi_destroy){(void)c;}
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii",vi_crop){c->x[0]=0;}
HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window){if(!native_window)native_window=(uint64_t)(uintptr_t)hle_alloc(256);*(uint64_t *)(uintptr_t)c->x[0]=native_window;c->x[0]=0;}
HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync){
    if(!vsync_handle){vsync_handle=runtime_event_create(0);_beginthreadex(NULL,0,vsync_entry,NULL,0,NULL);}
    c->x[1]=vsync_handle;c->x[2]=0;c->x[3]=1;
    guest_call(c,guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));c->x[0]=0;
}
// The Switch system dictionary is not in a game's RomFS. Supply an independent
// ASCII word filter for the native build; no console dictionary is bundled.
static int filter_char(const void *text,size_t i,bool wide){return wide?((const uint16_t *)text)[i]:((const uint8_t *)text)[i];}
static int filter_ascii(void *text,bool wide,bool mask){
    static const char *words[]={"fuck","shit","cunt"};
    size_t length=0;while(length<4096&&filter_char(text,length,wide))++length;
    if(length==4096)return -1;
    int matches=0;
    for(size_t i=0;i<length;++i)for(unsigned w=0;w<sizeof words/sizeof *words;++w){
        size_t n=strlen(words[w]);if(i+n>length)continue;
        bool equal=true;for(size_t j=0;j<n;++j){int c=filter_char(text,i+j,wide);if(c>127||tolower(c)!=words[w][j]){equal=false;break;}}
        if(!equal)continue;
        int before=i?filter_char(text,i-1,wide):0,after=i+n<length?filter_char(text,i+n,wide):0;
        if((before<128&&isalnum(before))||(after<128&&isalnum(after)))continue;
        ++matches;
        if(mask)for(size_t j=0;j<n;++j){if(wide)((uint16_t *)text)[i+j]='*';else((uint8_t *)text)[i+j]='*';}
    }
    return matches;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm",ngc_init){fprintf(stderr,"[ngc] native ASCII filter (system dictionary not supplied)\n");c->x[0]=0;}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb",ngc_init_ex){c->x[0]=0;}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter8FinalizeEv",ngc_fini){(void)c;}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv",ngc_version){c->x[0]=1;}
static void ngc_check(Cpu *c,bool wide){
    if(c->x[4]>32){c->x[0]=0xD401;return;}
    uint32_t *out=(uint32_t *)(uintptr_t)c->x[1];uint64_t *strings=(uint64_t *)(uintptr_t)c->x[3];
    for(size_t i=0;i<c->x[4];++i){int n=filter_ascii((void *)(uintptr_t)strings[i],wide,false);if(n<0){c->x[0]=0xD401;return;}out[i]=n?(uint32_t)(c->x[2]?c->x[2]:1):0;}
    c->x[0]=0;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm",ngc_check8){ngc_check(c,false);}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm",ngc_check16){ngc_check(c,true);}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm",ngc_check_ds){ngc_check(c,true);}
static void ngc_mask(Cpu *c,bool wide){int n=filter_ascii((void *)(uintptr_t)c->x[2],wide,true);if(n>=0)*(int *)(uintptr_t)c->x[1]=n;c->x[0]=n<0?0xD401:0;}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj",ngc_mask8){ngc_mask(c,false);}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj",ngc_mask16){ngc_mask(c,true);}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj",ngc_mask_ds){ngc_mask(c,true);}
volatile int g_debug_frame_request;
void widescreen_frame_end(uint64_t f){(void)f;}
bool widescreen_allowed(void){return false;}
void widescreen_set_factor(float factor){(void)factor;}
void widescreen_present_crop(uint64_t frame,double shape,float *crop,double *x,double *y,double *w,double *h){(void)frame;(void)shape;(void)x;(void)y;(void)w;(void)h;crop[0]=crop[1]=0;crop[2]=crop[3]=1;}
void fnprof_frame(uint64_t frame){(void)frame;}
void warp_frame(uint64_t frame){(void)frame;}
void reboot_test_frame(uint64_t frame){(void)frame;}
void save_editor_frame(uint64_t frame){(void)frame;}
bool mirror_scene_size(uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;return false;}
bool mirror_draw(bool composite){(void)composite;return false;}
void scene_size_log(bool c,uint32_t a,uint32_t b,uint32_t d,uint32_t e){(void)c;(void)a;(void)b;(void)d;(void)e;}
bool pot_level_active(void){return false;}
bool pot_glow_active(void){return false;}
bool pets_hide_item_cursor(void){return false;}
bool pot_tint(const float *m,float tint[8],uint64_t mesh){(void)m;(void)tint;(void)mesh;return false;}
bool pot_instance_gold(const float *m){(void)m;return false;}
void pot_gold_tint(float tint[8]){(void)tint;}
void widescreen_backdrop_seen(uint64_t hash){(void)hash;}
void widescreen_scene_ubo(const float *u,uint64_t size){(void)u;(void)size;}
float widescreen_effect_stretch(uint64_t hash){(void)hash;return 1;}
size_t rt_lz4_decompress(const uint8_t *s,size_t n,uint8_t *d,size_t cap){(void)s;(void)n;(void)d;(void)cap;return 0;} // AOT preparation disabled
