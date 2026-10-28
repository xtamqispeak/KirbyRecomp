// Standalone own-dump setup. The embedded payload contains open-source tools only.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WM_LOG (WM_APP+1)
#define WM_DONE (WM_APP+2)
#define PATH_CAP 4096
static HWND window, root_edit, exefs_edit, romfs_edit, output, status;
static HWND install_button, build_button, play_button, cancel_button;
static HINSTANCE instance;
static HFONT font;
static int scale=96, busy;
static volatile LONG cancelled;
static HANDLE job;
static wchar_t root[PATH_CAP], exefs[PATH_CAP], romfs[PATH_CAP];
static const wchar_t *action;

static int px(int value) { return MulDiv(value,scale,96); }
static void log_text(const wchar_t *text) {
    if (!window) {
        char buffer[16384]; int n=WideCharToMultiByte(CP_UTF8,0,text,-1,buffer,sizeof buffer,NULL,NULL);
        DWORD written; if(n>1)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),buffer,n-1,&written,NULL);
        return;
    }
    size_t length=wcslen(text),n=0;
    wchar_t *copy=malloc((length*2+1)*sizeof *copy);
    if(copy){for(size_t i=0;i<length;i++){if(text[i]==L'\n'&&(!i||text[i-1]!=L'\r'))copy[n++]=L'\r';copy[n++]=text[i];}copy[n]=0;}
    if(copy&&!PostMessageW(window,WM_LOG,0,(LPARAM)copy))free(copy);
}
static int error_text(const wchar_t *text) { log_text(text); log_text(L"\r\n"); return 1; }
static int combine(wchar_t *out,const wchar_t *base,const wchar_t *name) {
    return swprintf(out,PATH_CAP,L"%ls\\%ls",base,name)>0;
}
static int exists(const wchar_t *path) { return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES; }
static int prepare_directory(void) {
    wchar_t absolute[PATH_CAP],marker[PATH_CAP],pattern[PATH_CAP],walk[PATH_CAP];
    DWORD n=GetFullPathNameW(root,PATH_CAP,absolute,NULL);
    if(n<4||n>=PATH_CAP||absolute[1]!=L':'||absolute[2]!=L'\\')return error_text(L"Choose a local installation folder, not a drive root.");
    wcscpy(root,absolute); wcscpy(walk,root);
    for(wchar_t *p=walk+3;;p++) {
        if(*p==L'\\'||!*p) {
            wchar_t saved=*p;*p=0;DWORD attributes=GetFileAttributesW(walk);*p=saved;
            if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return error_text(L"Choose an installation path without junctions or symbolic links.");
            if(!saved)break;
        }
    }
    combine(marker,root,L"kirby-setup.marker");combine(pattern,root,L"*");
    if(exists(root)&&!exists(marker)) {
        WIN32_FIND_DATAW data;HANDLE search=FindFirstFileW(pattern,&data);int nonempty=0;
        if(search!=INVALID_HANDLE_VALUE) {
            do { if(wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L".."))nonempty=1; }while(FindNextFileW(search,&data));
            FindClose(search);
        }
        if(nonempty)return error_text(L"Choose an empty folder or an existing KirbyRecomp setup installation.");
    }
    int result=SHCreateDirectoryExW(NULL,root,NULL);
    if(result!=ERROR_SUCCESS&&result!=ERROR_ALREADY_EXISTS&&result!=ERROR_FILE_EXISTS)return error_text(L"Could not create the installation folder.");
    HANDLE file=CreateFileW(marker,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return error_text(L"The installation folder is not writable.");
    const char text[]="KirbyRecomp setup v0.3.0-alpha\r\n";DWORD wrote;
    int ok=WriteFile(file,text,sizeof(text)-1,&wrote,NULL);CloseHandle(file);
    return ok?0:error_text(L"Could not write the installation marker.");
}

// Quote one argument for CommandLineToArgvW/CRT parsing, including trailing slashes.
static int quote(wchar_t *out,size_t cap,const wchar_t *value) {
    size_t n=0,slashes=0;
    if(cap<3)return 0;out[n++]=L'"';
    for(const wchar_t *p=value;;p++) {
        if(*p==L'\\'){slashes++;continue;}
        size_t repeat=(*p==L'"'||!*p)?slashes*2:slashes;
        if(n+repeat+3>=cap)return 0;
        while(repeat--)out[n++]=L'\\';slashes=0;
        if(!*p){out[n++]=L'"';out[n]=0;return 1;}
        if(*p==L'"')out[n++]=L'\\';out[n++]=*p;
    }
}
static int child(const wchar_t **argv,int count) {
    wchar_t command[32768]={0},part[PATH_CAP*2+4];size_t used=0;
    for(int i=0;i<count;i++) {
        if(!quote(part,sizeof part/sizeof *part,argv[i]))return error_text(L"A path is too long.");
        size_t length=wcslen(part);
        if(used+length+2>=32768)return error_text(L"Command line is too long.");
        if(i)command[used++]=L' ';wcscpy(command+used,part);used+=length;
    }
    if(InterlockedCompareExchange(&cancelled,0,0))return 1223;
    SECURITY_ATTRIBUTES security={sizeof security,NULL,TRUE};HANDLE read_pipe,write_pipe;
    if(!CreatePipe(&read_pipe,&write_pipe,&security,0))return error_text(L"Could not create the build output pipe.");
    SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW startup={.cb=sizeof startup,.dwFlags=STARTF_USESTDHANDLES,
        .hStdOutput=write_pipe,.hStdError=write_pipe};
    startup.hStdInput=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,NULL);
    PROCESS_INFORMATION process={0};
    job=CreateJobObjectW(NULL,NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)) {
        if(job)CloseHandle(job);job=NULL;CloseHandle(read_pipe);CloseHandle(write_pipe);CloseHandle(startup.hStdInput);
        return error_text(L"Could not create the build process group.");
    }
    BOOL started=CreateProcessW(argv[0],command,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,NULL,root,&startup,&process);
    CloseHandle(write_pipe);CloseHandle(startup.hStdInput);
    if(!started) {
        CloseHandle(read_pipe);CloseHandle(job);job=NULL;
        return error_text(L"Could not start the tool. Windows 10/11 with built-in tar.exe is required.");
    }
    if(!AssignProcessToJobObject(job,process.hProcess)) {
        TerminateProcess(process.hProcess,1);CloseHandle(process.hProcess);CloseHandle(process.hThread);
        CloseHandle(read_pipe);CloseHandle(job);job=NULL;return error_text(L"Could not isolate the build process group.");
    }
    ResumeThread(process.hThread);CloseHandle(process.hThread);
    wchar_t log_path[PATH_CAP];combine(log_path,root,L"setup.log");
    HANDLE log=CreateFileW(log_path,FILE_APPEND_DATA,FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    char bytes[4096];DWORD got;
    for(;;) {
        if(InterlockedCompareExchange(&cancelled,0,0))TerminateJobObject(job,1223);
        DWORD available=0;
        if(!PeekNamedPipe(read_pipe,NULL,0,NULL,&available,NULL))break;
        if(available) {
            if(!ReadFile(read_pipe,bytes,sizeof bytes-1,&got,NULL))break;
            if(log!=INVALID_HANDLE_VALUE){DWORD written;WriteFile(log,bytes,got,&written,NULL);}
            wchar_t text[8192];int length=MultiByteToWideChar(CP_UTF8,0,bytes,got,text,8191);
            text[length]=0;log_text(text);
        } else if(WaitForSingleObject(process.hProcess,50)==WAIT_OBJECT_0) {
            // Output can arrive during the wait; drain it before reporting completion.
            if(!PeekNamedPipe(read_pipe,NULL,0,NULL,&available,NULL)||!available)break;
        }
    }
    WaitForSingleObject(process.hProcess,INFINITE);DWORD result=1;GetExitCodeProcess(process.hProcess,&result);
    CloseHandle(process.hProcess);CloseHandle(read_pipe);if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);
    // A launched game must survive the setup backend; build descendants should not.
    if(!wcscmp(action,L"play")&&result==0){limits.BasicLimitInformation.LimitFlags=0;SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits);}
    CloseHandle(job);job=NULL;return (int)result;
}
static int unpack(void) {
    wchar_t archive[PATH_CAP],system[PATH_CAP],tar[PATH_CAP],installed[PATH_CAP],self[PATH_CAP];
    combine(archive,root,L"kirby-setup-payload.zip");
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(101),RT_RCDATA);
    HGLOBAL loaded=resource?LoadResource(instance,resource):NULL;
    DWORD size=resource?SizeofResource(instance,resource):0;
    const void *data=loaded?LockResource(loaded):NULL;
    if(!data||!size)return error_text(L"The setup payload is missing.");
    HANDLE file=CreateFileW(archive,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);DWORD wrote=0;
    if(file==INVALID_HANDLE_VALUE)return error_text(L"Could not write the setup payload.");
    int ok=WriteFile(file,data,size,&wrote,NULL)&&wrote==size;CloseHandle(file);
    if(!ok)return error_text(L"Could not write the complete setup payload.");
    GetSystemDirectoryW(system,PATH_CAP);combine(tar,system,L"tar.exe");
    log_text(L"Unpacking portable Python and source tools...\r\n");
    const wchar_t *args[]={tar,L"-xf",archive,L"-C",root};
    int result=child(args,5);if(result)return result;
    GetModuleFileNameW(NULL,self,PATH_CAP);combine(installed,root,L"KirbyRecompSetup.exe");
    if(_wcsicmp(self,installed)&&!CopyFileW(self,installed,FALSE))return error_text(L"Could not copy the setup launcher into the installation.");
    return 0;
}
static int work(void) {
    int result=0;
    if(wcscmp(action,L"play")&&prepare_directory())return 1;
    wchar_t lock_path[PATH_CAP];combine(lock_path,root,L"kirby-setup.lock");
    HANDLE lock=CreateFileW(lock_path,GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE)return error_text(L"Could not lock the installation. Check the folder and close any other setup using it.");
    if(wcscmp(action,L"play"))result=unpack();
    if(result){CloseHandle(lock);return result;}
    wchar_t python[PATH_CAP],script[PATH_CAP];combine(python,root,L"python\\python.exe");combine(script,root,L"tools\\installer_backend.py");
    const wchar_t *args[]={python,L"-u",script,action,L"--exefs",exefs,L"--romfs",romfs};
    result=child(args,!wcscmp(action,L"build")?8:4);CloseHandle(lock);return result;
}
static unsigned __stdcall worker(void *unused) {
    (void)unused;int result=work();PostMessageW(window,WM_DONE,(WPARAM)result,0);return 0;
}
static HWND control(const wchar_t *type,const wchar_t *label,DWORD style,int x,int y,int w,int h,int id) {
    HWND widget=CreateWindowExW(!wcscmp(type,L"EDIT")?WS_EX_CLIENTEDGE:0,type,label,
        WS_CHILD|WS_VISIBLE|style,px(x),px(y),px(w),px(h),window,(HMENU)(INT_PTR)id,instance,NULL);
    SendMessageW(widget,WM_SETFONT,(WPARAM)font,TRUE);return widget;
}
static void choose(HWND edit) {
    BROWSEINFOW info={.hwndOwner=window,.lpszTitle=L"Choose a folder",.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE};
    PIDLIST_ABSOLUTE item=SHBrowseForFolderW(&info);wchar_t path[MAX_PATH];
    if(item){if(SHGetPathFromIDListW(item,path))SetWindowTextW(edit,path);CoTaskMemFree(item);}
}
static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_COMMAND: {
        int id=LOWORD(wp);
        if(id>=211&&id<=213&&!busy){choose(id==211?root_edit:id==212?exefs_edit:romfs_edit);return 0;}
        if(id==204&&busy){InterlockedExchange(&cancelled,1);SetWindowTextW(status,L"Cancelling build...");return 0;}
        if((id==201||id==202||id==203)&&!busy) {
            GetWindowTextW(root_edit,root,PATH_CAP);GetWindowTextW(exefs_edit,exefs,PATH_CAP);GetWindowTextW(romfs_edit,romfs,PATH_CAP);
            if(!*root||(id==202&&(!*exefs||!*romfs))){MessageBoxW(hwnd,L"Choose an installation folder and, for a game build, your dumped ExeFS and RomFS folders.",L"KirbyRecomp setup",MB_OK|MB_ICONINFORMATION);return 0;}
            action=id==201?L"install":id==202?L"build":L"play";busy=1;InterlockedExchange(&cancelled,0);
            EnableWindow(install_button,FALSE);EnableWindow(build_button,FALSE);EnableWindow(play_button,FALSE);EnableWindow(cancel_button,TRUE);
            SetWindowTextW(status,id==203?L"Launching game...":L"Working. The first build can take several minutes.");
            uintptr_t thread=_beginthreadex(NULL,0,worker,NULL,0,NULL);
            if(thread)CloseHandle((HANDLE)thread);else PostMessageW(hwnd,WM_DONE,1,0);
        }return 0;
    }
    case WM_LOG: {
        wchar_t *text=(wchar_t *)lp;SendMessageW(output,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);SendMessageW(output,EM_REPLACESEL,FALSE,(LPARAM)text);free(text);return 0;
    }
    case WM_DONE:
        busy=0;EnableWindow(install_button,TRUE);EnableWindow(build_button,TRUE);EnableWindow(play_button,TRUE);EnableWindow(cancel_button,FALSE);
        SetWindowTextW(status,wp?(InterlockedCompareExchange(&cancelled,0,0)?L"Cancelled. A build can be resumed.":L"Setup failed. Check the output and setup.log in your installation folder."):L"Done. Tools installed; after building your dump, click Play game.");return 0;
    case WM_CLOSE:
        if(busy){InterlockedExchange(&cancelled,1);SetWindowTextW(status,L"Cancelling. Close this window after the operation stops.");return 0;}
        DestroyWindow(hwnd);return 0;
    case WM_DESTROY: PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE app,HINSTANCE previous,PWSTR command,int show) {
    (void)previous;(void)command;instance=app;
    SetEnvironmentVariableW(L"PYTHONUTF8",L"1");SetEnvironmentVariableW(L"PYTHONIOENCODING",L"utf-8");
    int argc;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc==3&&(!wcscmp(argv[1],L"--self-test")||!wcscmp(argv[1],L"--install-only"))) {
        action=!wcscmp(argv[1],L"--self-test")?L"self-test":L"install";
        wcsncpy(root,argv[2],PATH_CAP-1);int result=work();LocalFree(argv);return result;
    }
    LocalFree(argv);CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);SetProcessDPIAware();
    HDC dc=GetDC(NULL);scale=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(NULL,dc);
    font=CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    wchar_t local[PATH_CAP];DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,PATH_CAP);
    if(!length||length>=PATH_CAP)return 1;combine(root,local,L"KirbyRecomp");
    wchar_t own[PATH_CAP],marker[PATH_CAP];GetModuleFileNameW(NULL,own,PATH_CAP);
    wchar_t *slash=wcsrchr(own,L'\\');
    if(slash){*slash=0;combine(marker,own,L"kirby-setup.marker");if(exists(marker))wcscpy(root,own);}
    WNDCLASSW cls={.lpfnWndProc=procedure,.hInstance=app,.hCursor=LoadCursor(NULL,IDC_ARROW),.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1),.lpszClassName=L"KirbyRecompSetup"};RegisterClassW(&cls);
    window=CreateWindowW(cls.lpszClassName,L"KirbyRecomp Setup v0.3.0-alpha",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,px(810),px(672),NULL,NULL,app,NULL);
    control(L"STATIC",L"KirbyRecomp - build from your own dumped game files",0,20,16,760,25,0);
    control(L"STATIC",L"Windows x64 | AVX2/FMA CPU | Vulkan 1.3 GPU | Internet for first setup",0,20,48,760,23,0);
    control(L"STATIC",L"Installation folder (Python is included; no administrator rights needed)",0,20,85,760,22,0);
    root_edit=control(L"EDIT",root,ES_AUTOHSCROLL|WS_TABSTOP,20,111,650,26,101);control(L"BUTTON",L"Browse...",WS_TABSTOP,682,111,90,26,211);
    control(L"STATIC",L"Your ExeFS folder (main, main.npdm, rtld, sdk)",0,20,150,760,22,0);
    exefs_edit=control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,20,176,650,26,102);control(L"BUTTON",L"Browse...",WS_TABSTOP,682,176,90,26,212);
    control(L"STATIC",L"Your RomFS folder (gfx, map, mint, msg, snd and remaining assets)",0,20,215,760,22,0);
    romfs_edit=control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,20,241,650,26,103);control(L"BUTTON",L"Browse...",WS_TABSTOP,682,241,90,26,213);
    wchar_t preferences[PATH_CAP];combine(preferences,root,L"kirby-setup-folders.txt");
    FILE *saved=_wfopen(preferences,L"rb");
    if(saved){wchar_t text[PATH_CAP*2+4]={0};size_t count=fread(text,sizeof(wchar_t),PATH_CAP*2+2,saved);fclose(saved);
        if(count>1&&text[0]==0xFEFF){wchar_t *first=text+1,*next=wcschr(first,L'\n');if(next){*next++=0;wchar_t *end=wcschr(next,L'\n');if(end)*end=0;SetWindowTextW(exefs_edit,first);SetWindowTextW(romfs_edit,next);}}
    }
    install_button=control(L"BUTTON",L"Install tools",WS_TABSTOP,20,284,135,32,201);
    build_button=control(L"BUTTON",L"Build game",WS_TABSTOP|BS_DEFPUSHBUTTON,168,284,135,32,202);
    play_button=control(L"BUTTON",L"Play game",WS_TABSTOP,316,284,135,32,203);
    cancel_button=control(L"BUTTON",L"Cancel",WS_TABSTOP,464,284,100,32,204);EnableWindow(cancel_button,FALSE);
    status=control(L"STATIC",L"Choose your dumps, then Build game. Install tools also works without a dump.",0,20,330,760,38,0);
    output=control(L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,20,372,752,205,104);SendMessageW(output,EM_SETLIMITTEXT,2000000,0);
    control(L"STATIC",L"Experimental alpha: battles/later stages unverified. No game code or assets included.",0,20,591,760,22,0);
    ShowWindow(window,show);UpdateWindow(window);
    MSG message;while(GetMessageW(&message,NULL,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    DeleteObject(font);CoUninitialize();return 0;
}
