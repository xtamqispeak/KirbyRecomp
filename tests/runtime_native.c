// Integration checks exercise public kernel calls and HIPC wire messages.
// Including the IPC unit also lets the save tests create ordinary service objects.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <assert.h>
#include <stdio.h>
#include "../runtime/ipc_runtime.c"

void runtime_stop(const char *reason,uint64_t pc,int code){fprintf(stderr,"FAIL %s %llx\n",reason,(unsigned long long)pc);ExitProcess(code);}
void rt_dispatch(Cpu *cpu){(void)cpu;assert(!"unexpected guest dispatch");}
uint64_t rt_ticks(void){return GetTickCount64()*19200;}
uint64_t runtime_romfs_size(void){return 0;}
int runtime_romfs_read(uint64_t offset,uint64_t size,void *out){(void)offset;(void)size;(void)out;return 0;}
void port_audren_u(Service *s,Request *r,Response *o){(void)s;(void)r;(void)o;assert(0);}
void port_audout_u(Service *s,Request *r,Response *o){(void)s;(void)r;(void)o;assert(0);}

static volatile LONG mutex_word;
static LONG total;
static unsigned __stdcall increment(void *argument){
    uint32_t tag=(uint32_t)(uintptr_t)argument;Cpu cpu={0};
    for(unsigned i=0;i<20000;++i){
        if(InterlockedCompareExchange(&mutex_word,(LONG)tag,0)!=0){
            cpu.x[1]=(uint64_t)(uintptr_t)&mutex_word;cpu.x[2]=tag;
            assert(runtime_kernel_svc(&cpu,0x1a)&&cpu.x[0]==0);
        }
        ++total;
        if(InterlockedCompareExchange(&mutex_word,0,(LONG)tag)!=(LONG)tag){
            cpu.x[0]=(uint64_t)(uintptr_t)&mutex_word;assert(runtime_kernel_svc(&cpu,0x1b)&&cpu.x[0]==0);
        }
    }
    return 0;
}
static void test_mutex(void){
    HANDLE threads[4];for(unsigned i=0;i<4;++i)threads[i]=(HANDLE)_beginthreadex(NULL,0,increment,(void *)(uintptr_t)(i+1),0,NULL);
    assert(WaitForMultipleObjects(4,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i)CloseHandle(threads[i]);assert(total==80000&&mutex_word==0);
    puts("PASS concurrent guest mutex: 80000 increments");
}
static void test_events(void){
    Cpu cpu={0};uint32_t original=runtime_event_create(0),copy=runtime_kernel_duplicate(original);
    assert(copy&&copy!=original);runtime_event_set(original,1);assert(runtime_kernel_close(original)==0);
    cpu.x[1]=(uint64_t)(uintptr_t)&copy;cpu.x[2]=1;cpu.x[3]=0;
    assert(runtime_kernel_svc(&cpu,0x18)&&cpu.x[0]==0);
    runtime_event_set(copy,0);cpu.x[1]=(uint64_t)(uintptr_t)&copy;cpu.x[2]=1;cpu.x[3]=0;
    assert(runtime_kernel_svc(&cpu,0x18)&&cpu.x[0]==0xea01);assert(runtime_kernel_close(copy)==0);
    puts("PASS event duplicates retain shared signal state after close");
}
static uint32_t *wire(Cpu *cpu,unsigned type,unsigned command,unsigned target){
    uint32_t *msg=(uint32_t *)(uintptr_t)cpu->tpidrro;memset(msg,0,256);msg[0]=type;
    unsigned at=4;if(target){msg[4]=1;msg[5]=target;at=8;}
    memcpy(msg+at,"SFCI",4);msg[at+2]=command;msg[1]=at+4-2;return msg+at+4;
}
static uint32_t *reply(Cpu *cpu){
    uint32_t *msg=(uint32_t *)(uintptr_t)cpu->tpidrro;
    for(unsigned i=2;i<58;++i)if(!memcmp(msg+i,"SFCO",4)){assert(msg[i+2]==0);return msg+i+4;}
    assert(!"no CMIF response");return NULL;
}
static void test_ipc(Cpu *cpu){
    uint32_t root=runtime_connect_port("sm:");assert(root);
    uint32_t *data=wire(cpu,4,1,0);memcpy(data,"set",4);((uint32_t *)(uintptr_t)cpu->tpidrro)[1]+=2;
    assert(!runtime_send_request(cpu,root));reply(cpu);
    uint32_t settings=((uint32_t *)(uintptr_t)cpu->tpidrro)[3];assert(settings!=root);
    wire(cpu,5,0,0);assert(!runtime_send_request(cpu,settings));assert(reply(cpu)[0]==1);
    wire(cpu,5,2,0);assert(!runtime_send_request(cpu,settings));reply(cpu);
    uint32_t clone=((uint32_t *)(uintptr_t)cpu->tpidrro)[3];assert(clone!=settings);
    assert(!runtime_close_handle(settings));
    wire(cpu,4,0,1);assert(!runtime_send_request(cpu,clone));assert(!memcmp(reply(cpu),"en-US",5));
    assert(!runtime_close_handle(clone));assert(runtime_send_request(cpu,clone)==0xe401);
    uint32_t *msg=(uint32_t *)(uintptr_t)cpu->tpidrro;memset(msg,0,256);msg[0]=4|(15<<16);
    assert(runtime_send_request(cpu,root)==0xf601);assert(!runtime_close_handle(root));
    puts("PASS CMIF service creation, domains, clone ownership, malformed descriptors");
}
static Response invoke(Service *s,unsigned cmd,const void *data,size_t size,const char *path){
    Request r={.command=cmd,.data=(uint8_t *)data,.size=size};Response o={0};
    if(path){r.nx=1;r.x[0]=(Buffer){(uint64_t)(uintptr_t)path,strlen(path)+1};}
    s->handler(s,&r,&o);return o;
}
static void test_saves(void){
    Service *fs=runtime_save_filesystem(77);assert(fs);service_retain(fs);fs->on_close=NULL;
    uint64_t create[2]={0,32};Response result=invoke(fs,0,create,sizeof create,"/roundtrip.dat");assert(result.result==0);
    unsigned mode=3;result=invoke(fs,8,&mode,4,"/roundtrip.dat");assert(!result.result&&result.count==1);
    Service *file=result.objects[0];service_retain(file);
    char text[]="native save roundtrip",out[sizeof text]={0};uint64_t args[3]={0,3,sizeof text};
    Request r={.command=1,.data=(uint8_t *)args,.size=24,.na=1,.a={{(uint64_t)(uintptr_t)text,sizeof text}}};Response o={0};
    file->handler(file,&r,&o);assert(!o.result);result=invoke(fs,10,NULL,0,NULL);assert(!result.result);
    service_release(file); // flush/close must release the OS file and table entry
    mode=1;result=invoke(fs,8,&mode,4,"/roundtrip.dat");assert(!result.result);file=result.objects[0];service_retain(file);
    r.command=0;r.na=0;r.nb=1;r.b[0]=(Buffer){(uint64_t)(uintptr_t)out,sizeof out};o=(Response){0};
    file->handler(file,&r,&o);assert(!o.result&&!memcmp(text,out,sizeof text));service_release(file);
    const char *bad[]={"/../escape.dat","/a/../../escape.dat","/C:/escape","/file:stream","/trailing. ","/a\\..\\escape","/NUL.dat","/COM1","/folder/aux.txt"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;++i)assert(invoke(fs,0,create,sizeof create,bad[i]).result!=0);
    assert(!invoke(fs,1,NULL,0,"/roundtrip.dat").result);
    free(fs->userdata);service_release(fs);
    puts("PASS durable save close/reopen, deletion, and path confinement");
}
int main(void){
    void *memory=VirtualAlloc((void *)0x7100000000ull,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(memory);
    Cpu cpu={0};cpu.tpidrro=(uint64_t)(uintptr_t)memory;runtime_kernel_init(&cpu,cpu.tpidrro);
    test_mutex();test_events();test_ipc(&cpu);test_saves();puts("PASS native runtime integration");return 0;
}
