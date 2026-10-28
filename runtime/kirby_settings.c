// Original Windows settings UI and optional, build-checked game timing hooks.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdatomic.h>
#include "kirby_settings.h"
#include "probe_modules.h"

KirbySettings kirby_settings = {1280,720,1920,1080,0,false};
static KirbySettings saved_settings;
static _Atomic bool settings_active;
static HWND settings_window, fields[4], fps_box, fullscreen_box, notice;
static HFONT ui_font;
static wchar_t config_path[4096];
static int dpi = 96;
static bool timing_supported;
static int px(int n) { return MulDiv(n,dpi,96); }
static bool valid_size(int w,int h) { return w>=320 && w<=7680 && h>=180 && h<=4320; }
static bool valid_fps(int n) { return n==0 || n==60 || n==120; }

void kirby_settings_load(void) {
    static bool loaded;
    if(loaded)return;
    loaded=true;
    GetModuleFileNameW(NULL,config_path,4096);
    wchar_t *slash=wcsrchr(config_path,L'\\');
    if(!slash || slash-config_path>4000)return;
    wcscpy(slash+1,L"kirby-settings.ini");
    int ww=GetPrivateProfileIntW(L"video",L"window_width",1280,config_path);
    int wh=GetPrivateProfileIntW(L"video",L"window_height",720,config_path);
    int rw=GetPrivateProfileIntW(L"video",L"render_width",1920,config_path);
    int rh=GetPrivateProfileIntW(L"video",L"render_height",1080,config_path);
    int fps=GetPrivateProfileIntW(L"video",L"fps",0,config_path);
    if(valid_size(ww,wh)){kirby_settings.window_width=ww;kirby_settings.window_height=wh;}
    if(valid_size(rw,rh)){kirby_settings.render_width=rw;kirby_settings.render_height=rh;}
    if(valid_fps(fps))kirby_settings.fps=fps;
    kirby_settings.fullscreen=GetPrivateProfileIntW(L"video",L"fullscreen",0,config_path)!=0;
    timing_supported=PROBE_KIRBY_TIMING_SUPPORTED;
    if(!timing_supported)kirby_settings.fps=0;
    // Integration tests use the same validated settings as the saved UI choices.
    const char *value=getenv("KIRBY_TEST_FPS");
    if(value && timing_supported && valid_fps(atoi(value)))kirby_settings.fps=atoi(value);
    saved_settings=kirby_settings;
    fprintf(stderr,"[settings] window=%dx%d render=%dx%d fps=%s%d fullscreen=%d (F1 settings, F11 fullscreen)\n",
        kirby_settings.window_width,kirby_settings.window_height,kirby_settings.render_width,
        kirby_settings.render_height,kirby_settings.fps?"":"original/",kirby_settings.fps,kirby_settings.fullscreen);
}

static bool save_settings(const KirbySettings *s) {
    wchar_t temporary[4096];
    if(swprintf(temporary,4096,L"%ls.tmp",config_path)<0)return false;
    FILE *file=_wfopen(temporary,L"wb");
    if(!file)return false;
    int wrote=fprintf(file,"[video]\r\nwindow_width=%d\r\nwindow_height=%d\r\nrender_width=%d\r\nrender_height=%d\r\nfps=%d\r\nfullscreen=%d\r\n",
        s->window_width,s->window_height,s->render_width,s->render_height,s->fps,s->fullscreen);
    bool ok=wrote>0 && !ferror(file);
    if(fclose(file))ok=false;
    if(ok)ok=MoveFileExW(temporary,config_path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileW(temporary);
    return ok;
}
static HWND control(const wchar_t *cls,const wchar_t *text,DWORD style,int x,int y,int w,int h,int id) {
    HWND item=CreateWindowExW(!wcscmp(cls,L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,
        WS_CHILD|WS_VISIBLE|style,px(x),px(y),px(w),px(h),settings_window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(item,WM_SETFONT,(WPARAM)ui_font,TRUE);return item;
}
static void fill_controls(const KirbySettings *s) {
    const int values[]={s->window_width,s->window_height,s->render_width,s->render_height};
    wchar_t text[24];for(int i=0;i<4;i++){swprintf(text,24,L"%d",values[i]);SetWindowTextW(fields[i],text);}
    SendMessageW(fps_box,CB_SETCURSEL,s->fps==120?2:s->fps==60?1:0,0);
    SendMessageW(fullscreen_box,BM_SETCHECK,s->fullscreen?BST_CHECKED:BST_UNCHECKED,0);
}
static LRESULT CALLBACK settings_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_COMMAND) {
        int id=LOWORD(w);
        if(id==101) {
            KirbySettings s=kirby_settings;int values[4];
            for(int i=0;i<4;i++) {
                wchar_t text[32],*end;GetWindowTextW(fields[i],text,32);
                long value=wcstol(text,&end,10);
                if(!*text || *end || value<0 || value>7680){SetWindowTextW(notice,L"Enter whole pixel dimensions in the supported range.");return 0;}
                values[i]=(int)value;
            }
            if(!valid_size(values[0],values[1])||!valid_size(values[2],values[3])) {
                SetWindowTextW(notice,L"Width: 320-7680 pixels. Height: 180-4320 pixels.");return 0;
            }
            s.window_width=values[0];s.window_height=values[1];s.render_width=values[2];s.render_height=values[3];
            int selected=(int)SendMessageW(fps_box,CB_GETCURSEL,0,0);
            s.fps=timing_supported?(selected==2?120:selected==1?60:0):0;
            s.fullscreen=SendMessageW(fullscreen_box,BM_GETCHECK,0,0)==BST_CHECKED;
            if(!save_settings(&s)){SetWindowTextW(notice,L"Could not save settings beside the game executable.");return 0;}
            saved_settings=s;
            SetWindowTextW(notice,L"Saved. Close and restart the game to apply these settings.");
            return 0;
        }
        if(id==102){KirbySettings defaults={1280,720,1920,1080,0,false};fill_controls(&defaults);SetWindowTextW(notice,L"Defaults selected. Click Save settings to keep them.");return 0;}
        if(id==103 || id==IDCANCEL){DestroyWindow(window);return 0;}
    }
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){atomic_store(&settings_active,false);settings_window=NULL;return 0;}
    return DefWindowProcW(window,message,w,l);
}
void kirby_settings_window(SDL_Window *parent) {
    if(settings_window){ShowWindow(settings_window,SW_SHOW);SetForegroundWindow(settings_window);return;}
    HINSTANCE app=GetModuleHandleW(NULL);
    HWND owner=(HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(parent),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    if(owner)dpi=GetDpiForWindow(owner);
    if(!dpi)dpi=96;
    if(ui_font)DeleteObject(ui_font);
    ui_font=CreateFontW(-px(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    WNDCLASSW cls={.lpfnWndProc=settings_proc,.hInstance=app,.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512)),.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1),.lpszClassName=L"KirbyRecompSettings"};
    RegisterClassW(&cls);
    settings_window=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_APPWINDOW,cls.lpszClassName,L"KirbyRecomp - Settings",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,px(560),px(480),NULL,NULL,app,NULL);
    if(!settings_window){fprintf(stderr,"[settings] could not open the settings window\n");return;}
    control(L"STATIC",L"VIDEO SETTINGS",0,24,18,480,28,0);
    for(int row=0;row<2;row++) {
        control(L"STATIC",row?L"Render width":L"Window width",0,24,65+row*45,160,24,0);
        fields[row*2]=control(L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,200,62+row*45,105,28,200+row*2);
        control(L"STATIC",L"Height",0,320,65+row*45,60,24,0);
        fields[row*2+1]=control(L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,385,62+row*45,105,28,201+row*2);
    }
    control(L"STATIC",L"Frame rate",0,24,158,180,24,0);
    fps_box=control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,220,153,244,160,204);
    const wchar_t *choices[]={L"Original (30 / 60 FPS)",L"60 FPS",L"120 FPS (experimental)"};
    for(int i=0;i<3;i++)SendMessageW(fps_box,CB_ADDSTRING,0,(LPARAM)choices[i]);
    EnableWindow(fps_box,timing_supported);
    fullscreen_box=control(L"BUTTON",L"Start in fullscreen",WS_TABSTOP|BS_AUTOCHECKBOX,24,202,400,26,205);
    control(L"STATIC",L"The game keeps its original 16:9 shape. Larger render resolutions require more GPU power. 120 FPS rendering keeps fixed 60 Hz game logic.",0,24,240,485,65,0);
    notice=control(L"STATIC",timing_supported?L"F1 opens settings. F11 toggles fullscreen. Restart after saving.":L"Timing overrides are unavailable for this game build.",0,24,303,485,50,0);
    control(L"BUTTON",L"Save settings",WS_TABSTOP|BS_DEFPUSHBUTTON,24,375,155,34,101);
    control(L"BUTTON",L"Defaults",WS_TABSTOP,194,375,140,34,102);
    control(L"BUTTON",L"Close",WS_TABSTOP,349,375,140,34,103);
    atomic_store(&settings_active,settings_window!=NULL);
    RECT bounds;GetWindowRect(owner,&bounds);
    int x=bounds.left+((bounds.right-bounds.left)-px(560))/2,y=bounds.top+((bounds.bottom-bounds.top)-px(480))/2;
    SetWindowPos(settings_window,NULL,x<0?0:x,y<0?0:y,0,0,SWP_NOSIZE|SWP_NOZORDER);
    fill_controls(&saved_settings);ShowWindow(settings_window,SW_SHOW);SetForegroundWindow(settings_window);SetFocus(fields[0]);
}
bool kirby_settings_open(void){return atomic_load(&settings_active);}
void kirby_settings_pump(void) {
    MSG msg;while(settings_window && PeekMessageW(&msg,settings_window,0,0,PM_REMOVE)) {
        if(!IsDialogMessageW(settings_window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
}

static volatile LONG frame_units=1;
int kirby_frame_hook(Cpu *cpu,uint64_t site) {
    if(!kirby_settings.fps)return 0;
    // Verified only for the main build selected by tools/game_hooks.py.
    // The host's present pacer replaces the original 60 Hz scheduler's waits.
    if(site==0x70000173d0ull || site==0x7000017460ull)return 1;
    // Rendering may run at 120 Hz; the game consumes whole fixed 60 Hz steps.
    // Sample once per frame so every subsystem sees the same delta.
    if(site==0x70000172b0ull && kirby_settings.fps==120) {
        static uint64_t previous;
        uint64_t now=rt_ticks()/320000;
        uint64_t elapsed=previous?now-previous:1;previous=now;
        InterlockedExchange(&frame_units,(LONG)(elapsed>6?6:elapsed));
        return 0;
    }
    if(site==0x7000017260ull || site==0x7000017530ull) {
        ST32(cpu->x[0],0);ST8(cpu->x[0]+0x32,1);return 1;
    }
    if(site==0x7000017560ull) {
        float delta=kirby_settings.fps==120?(float)InterlockedOr(&frame_units,0):1.0f;uint32_t bits;memcpy(&bits,&delta,4);cpu->v[0]=bits;return 1;
    }
    if(site==0x700001af90ull) {
        uint64_t now=rt_ticks(), origin=LD64(cpu->x[0]+8);
        cpu->x[0]=now>=origin?(uint64_t)((__uint128_t)(now-origin)*kirby_settings.fps/19200000):0;return 1;
    }
    if(site==0x700001ab90ull) {
        cpu->x[0]=kirby_settings.fps==120?(unsigned)InterlockedOr(&frame_units,0):1;return 1;
    }
    return 0;
}
