// Local save filesystem. Guest paths are confined to the executable's save/
// directory, with traversal, Windows special syntax and reparse points rejected.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include "filesystem_runtime.h"
#include "ipc_runtime.h"
#define FS_MISSING 0x202u
#define FS_EXISTS 0x402u
#define FS_INVALID 0x2EE202u
typedef struct LocalFs { char root[2048]; } LocalFs;
typedef struct LocalFile { FILE *stream; unsigned mode; } LocalFile;
static SRWLOCK lock=SRWLOCK_INIT;
static LocalFile *files[256];

static uint32_t os_result(void) {
    DWORD error=GetLastError();
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return FS_MISSING;
    if(error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS)return FS_EXISTS;
    if(error==ERROR_DIR_NOT_EMPTY)return 0x1002;
    return FS_INVALID;
}
uint32_t runtime_ensure_save(unsigned type) {
    if(type>255)return FS_INVALID;
    char directory[64];snprintf(directory,sizeof directory,"save/save_%u",type);
    if(!CreateDirectoryA("save",NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return os_result();
    DWORD attr=GetFileAttributesA("save");if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return FS_INVALID;
    if(!CreateDirectoryA(directory,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return os_result();
    attr=GetFileAttributesA(directory);if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return FS_INVALID;
    return 0;
}
static Buffer input_path(Request *request,unsigned index) {
    if(index<request->na&&request->a[index].size)return request->a[index];
    return index<request->nx?request->x[index]:(Buffer){0};
}
static int path_for(LocalFs *fs,Request *request,unsigned index,char output[4096]) {
    Buffer input=input_path(request,index);
    if(!input.address||!input.size)return 0;
    size_t length=input.size<0x301?(size_t)input.size:0x301;
    const char *path=(const char *)(uintptr_t)input.address,*end=memchr(path,0,length);
    if(!end)return 0;
    const char *p=path;
    while(p<end){while(p<end&&*p=='/')++p;const char *start=p;
        while(p<end&&*p!='/'){if(strchr("\\:*?\"<>|",*p)|| (unsigned char)*p<32)return 0;++p;}
        size_t n=(size_t)(p-start);if((n==1&&start[0]=='.')||(n==2&&start[0]=='.'&&start[1]=='.'))return 0;
        if(n&&(start[n-1]==' '||start[n-1]=='.'))return 0;
        size_t stem=0;while(stem<n&&start[stem]!='.')++stem;
        if((stem==3&&(!_strnicmp(start,"CON",3)||!_strnicmp(start,"PRN",3)||!_strnicmp(start,"AUX",3)||!_strnicmp(start,"NUL",3)))||
           (stem==4&&(!_strnicmp(start,"COM",3)||!_strnicmp(start,"LPT",3))&&start[3]>='1'&&start[3]<='9'))return 0;
    }
    while(*path=='/')++path;
    char combined[4096];if(snprintf(combined,sizeof combined,"%s/%s",fs->root,path)>=(int)sizeof combined)return 0;
    DWORD n=GetFullPathNameA(combined,4096,output,NULL);size_t root_length=strlen(fs->root);
    if(!n||n>=4096||_strnicmp(output,fs->root,root_length)||(output[root_length]&&output[root_length]!='\\'))return 0;
    for(char *q=output+root_length; ;++q){
        if(*q=='\\'||!*q){char save=*q;*q=0;DWORD attr=GetFileAttributesA(output);*q=save;
            if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_REPARSE_POINT))return 0;
            if(!save)break;
        }
    }
    return 1;
}
static Buffer in_buffer(Request *request) { return request->na&&request->a[0].size?request->a[0]:request->nx?request->x[0]:(Buffer){0}; }
static Buffer out_buffer(Request *request) { return request->nb&&request->b[0].size?request->b[0]:request->nc?request->c[0]:(Buffer){0}; }
static void close_file(Service *service) {
    LocalFile *file=service->userdata;
    AcquireSRWLockExclusive(&lock);
    for(unsigned i=0;i<256;++i)if(files[i]==file)files[i]=NULL;
    if(file->stream)fclose(file->stream);
    ReleaseSRWLockExclusive(&lock);free(file);service->userdata=NULL;
}
static void local_file(Service *service,Request *request,Response *response) {
    LocalFile *file=service->userdata;
    AcquireSRWLockExclusive(&lock);
    switch(request->command){
    case 0:case 1:{
        if(request->size<24){response->result=FS_INVALID;break;}
        int64_t offset=*(int64_t *)(request->data+8),size=*(int64_t *)(request->data+16);
        Buffer buffer=request->command?in_buffer(request):out_buffer(request);
        if(offset<0||size<0||(uint64_t)size>buffer.size||_fseeki64(file->stream,offset,SEEK_SET)){response->result=FS_INVALID;break;}
        if(request->command==0){size_t n=fread((void *)(uintptr_t)buffer.address,1,(size_t)size,file->stream);response->size=8;*(uint64_t *)response->data=n;if(ferror(file->stream))response->result=FS_INVALID;}
        else{if(!(file->mode&2)||fwrite((void *)(uintptr_t)buffer.address,1,(size_t)size,file->stream)!=(size_t)size)response->result=FS_INVALID;}
        break;
    }
    case 2:
        if(fflush(file->stream)||((file->mode&2)&&!FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(file->stream)))))response->result=FS_INVALID;break;
    case 3:
        if(!(file->mode&2)||request->size<8||*(int64_t *)request->data<0||_chsize_s(_fileno(file->stream),*(int64_t *)request->data))response->result=FS_INVALID;break;
    case 4:
        if(_fseeki64(file->stream,0,SEEK_END))response->result=FS_INVALID;
        else{response->size=8;*(int64_t *)response->data=_ftelli64(file->stream);}break;
    default:ReleaseSRWLockExclusive(&lock);runtime_stop("unimplemented_file_command",request->command,96);
    }
    ReleaseSRWLockExclusive(&lock);
}
static void local_filesystem(Service *service,Request *request,Response *response) {
    LocalFs *fs=service->userdata;char path[4096],second[4096];
    if(request->command!=10&&request->command!=11&&request->command!=12&&!path_for(fs,request,0,path)){response->result=FS_INVALID;return;}
    fprintf(stderr,"[save] command=%u path=%s\n",request->command,request->command>=10?fs->root:path);
    switch(request->command){
    case 0:{
        if(request->size<16||*(int64_t *)(request->data+8)<0){response->result=FS_INVALID;return;}
        HANDLE handle=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(handle==INVALID_HANDLE_VALUE){response->result=os_result();return;}
        LARGE_INTEGER length;length.QuadPart=*(int64_t *)(request->data+8);
        if(!SetFilePointerEx(handle,length,NULL,FILE_BEGIN)||!SetEndOfFile(handle))response->result=FS_INVALID;CloseHandle(handle);return;
    }
    case 1:if(!DeleteFileA(path))response->result=os_result();return;
    case 2:if(!CreateDirectoryA(path,NULL))response->result=os_result();return;
    case 3:if(!RemoveDirectoryA(path))response->result=os_result();return;
    case 5:case 6:
        if(!path_for(fs,request,1,second)){response->result=FS_INVALID;return;}
        if(!MoveFileExA(path,second,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))response->result=os_result();return;
    case 7:{DWORD attr=GetFileAttributesA(path);if(attr==INVALID_FILE_ATTRIBUTES){response->result=FS_MISSING;return;}response->size=4;*(uint32_t *)response->data=(attr&FILE_ATTRIBUTE_DIRECTORY)?0:1;return;}
    case 8:{
        if(request->size<4){response->result=FS_INVALID;return;}
        unsigned mode=*(uint32_t *)request->data;FILE *stream=fopen(path,(mode&2)?"rb+":"rb");
        if(!stream){response->result=FS_MISSING;return;}
        LocalFile *file=calloc(1,sizeof *file);file->stream=stream;file->mode=mode;
        AcquireSRWLockExclusive(&lock);unsigned i;for(i=0;i<256;++i)if(!files[i]){files[i]=file;break;}ReleaseSRWLockExclusive(&lock);
        if(i==256){fclose(stream);free(file);response->result=FS_INVALID;return;}
        Service *opened=runtime_service_new("ILocalSaveFile",local_file,file);opened->on_close=close_file;runtime_service_object(response,opened);return;
    }
    case 10:
        AcquireSRWLockExclusive(&lock);
        for(unsigned i=0;i<256;++i)if(files[i]&&(files[i]->mode&2)){
            if(fflush(files[i]->stream)||!FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(files[i]->stream))))response->result=FS_INVALID;
        }
        ReleaseSRWLockExclusive(&lock);return;
    case 11:case 12:response->size=8;*(uint64_t *)response->data=1024ull*1024*1024;return;
    default:runtime_stop("unimplemented_save_filesystem_command",request->command,96);
    }
}
Service *runtime_save_filesystem(unsigned type) {
    if(runtime_ensure_save(type))return NULL;
    LocalFs *fs=calloc(1,sizeof *fs);char directory[64];snprintf(directory,sizeof directory,"save/save_%u",type);
    if(!GetFullPathNameA(directory,sizeof fs->root,fs->root,NULL)){free(fs);return NULL;}
    return runtime_service_new("ILocalSaveFileSystem",local_filesystem,fs);
}
