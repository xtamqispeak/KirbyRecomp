// HIPC/CMIF services for the native port. Wire format follows LASRecomp's MIT
// IPC implementation (vendor/lasrecomp/LICENSE), with bounded message parsing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ipc_runtime.h"
#include "kernel_runtime.h"
#include "romfs_runtime.h"
#include "service_api.h"
#include "filesystem_runtime.h"
#ifdef PORT_GRAPHICS
#include "input_runtime.h"
#endif

#define MAX_HANDLES 1024
#define BAD_HANDLE 0xE401u
#define BAD_MESSAGE 0xF601u
typedef struct Domain { Service *objects[256]; volatile LONG references; } Domain;
typedef struct Session { Service *service; Domain *domain; int open; } Session;
static Session sessions[MAX_HANDLES];
static SRWLOCK session_lock = SRWLOCK_INIT;
static uint32_t applet_event;
static int focus_message_pending = 1;
static const uint8_t clock_uuid[16] = {'K','i','r','b','y','P','o','r','t',1,2,3,4,5,6,7};
static uint32_t time_shared;
static uint32_t hid_shared;
static const uint8_t local_uid[16]={'K','I','R','B','Y','R','E','C','O','M','P',0,0,0,0,1};
static int launch_parameter_taken;
static void clock_context(uint8_t *data) {
    int64_t steady = (int64_t)(rt_ticks() / 19200000);
    int64_t offset = (int64_t)time(NULL) - steady;
    memcpy(data, &offset, 8); memcpy(data + 8, &steady, 8); memcpy(data + 16, clock_uuid, 16);
}
static void time_mapped(uint64_t address) {
    uint8_t *data = (uint8_t *)(uintptr_t)address;
    memcpy(data + 0x10, clock_uuid, 16);
    clock_context(data + 0x40); clock_context(data + 0x88);
}
static void calendar_fields(int64_t seconds, uint8_t *calendar, uint8_t *additional) {
    time_t timestamp=(time_t)seconds;struct tm value;
    struct tm *p=gmtime(&timestamp);if(!p)return;value=*p;
    *(uint16_t *)calendar=value.tm_year+1900;calendar[2]=value.tm_mon+1;calendar[3]=value.tm_mday;
    calendar[4]=value.tm_hour;calendar[5]=value.tm_min;calendar[6]=value.tm_sec;
    *(uint32_t *)additional=value.tm_wday;*(uint32_t *)(additional+4)=value.tm_yday;memcpy(additional+8,"UTC",4);
}

static Service *service_new(const char *name) {
    Service *s = calloc(1, sizeof *s);
    if (!s) runtime_stop("service_allocation_failed", 0, 90);
    snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}
Service *runtime_service_new(const char *name, ServiceHandler handler, void *state) {
    Service *service = service_new(name); service->handler = handler; service->userdata = state; return service;
}
void runtime_service_object(Response *response, Service *service) {
    if (response->count >= 8) runtime_stop("too_many_service_objects", 0, 90);
    response->objects[response->count++] = service;
}
extern void port_audren_u(Service *, Request *, Response *);
extern void port_audout_u(Service *, Request *, Response *);
static void service_retain(Service *service){InterlockedIncrement(&service->references);}
static void service_release(Service *service){
    if(service&&InterlockedDecrement(&service->references)==0){
        if(service->on_close)service->on_close(service);
        free(service);
    }
}
static void domain_release(Domain *domain){
    if(InterlockedDecrement(&domain->references)==0){
        for(unsigned i=1;i<256;++i)service_release(domain->objects[i]);
        free(domain);
    }
}
static uint32_t session_new(Service *service, Domain *domain) {
    AcquireSRWLockExclusive(&session_lock);
    for (unsigned i = 1; i < MAX_HANDLES; ++i) if (!sessions[i].open) {
        if(domain)InterlockedIncrement(&domain->references);else service_retain(service);
        sessions[i] = (Session){service, domain, 1};
        ReleaseSRWLockExclusive(&session_lock);
        return 0x200 + i;
    }
    ReleaseSRWLockExclusive(&session_lock);
    runtime_stop("session_table_full", 0, 90);
}
static Session *session_get(uint32_t handle) {
    return handle > 0x200 && handle < 0x200 + MAX_HANDLES && sessions[handle - 0x200].open ? &sessions[handle - 0x200] : NULL;
}
uint32_t runtime_connect_port(const char *name) {
    if (strcmp(name, "sm:")) return 0;
    return session_new(service_new(name), NULL);
}
uint32_t runtime_close_handle(uint32_t handle) {
    Session *s = session_get(handle);
    if (!s) return BAD_HANDLE;
    AcquireSRWLockExclusive(&session_lock);
    Service *service=s->service;Domain *domain=s->domain;s->open=0;
    ReleaseSRWLockExclusive(&session_lock);
    if(domain)domain_release(domain);else service_release(service);
    return 0;
}
static void service_call(Service *service, Request *req, Response *resp) {
    static int trace=-1;if(trace<0)trace=getenv("KIRBY_TRACE_IPC")!=NULL;
    if(trace)fprintf(stderr, "[ipc] %s cmd=%u input=%zu x=%u a=%u b=%u c=%u\n", service->name, req->command, req->size, req->nx, req->na, req->nb, req->nc);
    if (service->handler) { service->handler(service, req, resp); return; }
    if (!strcmp(service->name, "audren:u")) { port_audren_u(service, req, resp); return; }
    if (!strcmp(service->name, "audout:u")) { port_audout_u(service, req, resp); return; }
    if (!strcmp(service->name, "sm:")) {
        if (req->command == 0) return;  // RegisterClient
        if (req->command == 1 && req->size >= 8) {
            char name[9] = {0}; memcpy(name, req->data, 8);
            fprintf(stderr, "[service] GetService name=%s\n", name);
            resp->objects[resp->count++] = service_new(name);
            return;
        }
    }
    if (!strcmp(service->name, "lm") && req->command == 0) {
        resp->objects[resp->count++] = service_new("ILogger"); return;
    }
    if (!strcmp(service->name, "prepo:u")) {
        // Play reports are accepted by this local runtime; no remote transport.
        if (req->command >= 10100 && req->command <= 10103) {
            ++service->state;
            fprintf(stderr, "[play-report] local report %llu\n", (unsigned long long)service->state);
            return;
        }
    }
    if (!strcmp(service->name, "ILogger")) {
        if (req->command == 1) { service->state = req->size >= 4 ? *(uint32_t *)req->data : 0; return; }
        if (req->command == 0) {
            Buffer buffer = req->na && req->a[0].size ? req->a[0] : req->nx ? req->x[0] : (Buffer){0};
            if (buffer.size > 1024 * 1024) { resp->result = BAD_MESSAGE; return; }
            const uint8_t *p = (const uint8_t *)(uintptr_t)buffer.address;
            size_t offset = 0x18;
            while (offset + 2 <= buffer.size) {
                uint8_t key = p[offset++], length = p[offset++];
                if (length > buffer.size - offset) break;
                if (key == 2) { fprintf(stderr, "[guest-log] "); fwrite(p + offset, 1, length, stderr); fputc('\n', stderr); }
                offset += length;
            }
            return;
        }
    }
    if (!strcmp(service->name, "set:sys") && (req->command == 3 || req->command == 4)) {
        Buffer buffer = req->nb && req->b[0].size ? req->b[0] : req->nc ? req->c[0] : (Buffer){0};
        if (buffer.size < 0x100) { resp->result = BAD_MESSAGE; return; }
        uint8_t firmware[0x100] = {8, 1, 0, 0, 8, 1, 0};
        memcpy(firmware + 0x68, "8.1.0", 6); memcpy(firmware + 0x80, "8.1.0-1.0", 10);
        memcpy((void *)(uintptr_t)buffer.address, firmware, sizeof firmware); return;
    }
    if (!strcmp(service->name, "appletOE") && req->command == 0) {
        if (!applet_event) applet_event = runtime_event_create(1);
        resp->objects[resp->count++] = service_new("IApplicationProxy"); return;
    }
    if (!strcmp(service->name, "IApplicationProxy")) {
        static const struct { unsigned command; const char *name; } interfaces[] = {
            {0,"ICommonStateGetter"}, {1,"ISelfController"}, {2,"IWindowController"},
            {3,"IAudioController"}, {4,"IDisplayController"}, {11,"ILibraryAppletCreator"},
            {20,"IApplicationFunctions"}, {1000,"IDebugFunctions"}
        };
        for (unsigned i = 0; i < sizeof interfaces / sizeof *interfaces; ++i)
            if (req->command == interfaces[i].command) { resp->objects[resp->count++] = service_new(interfaces[i].name); return; }
    }
    if (!strcmp(service->name, "ICommonStateGetter")) {
        switch (req->command) {
        case 0: resp->copy[resp->ncopy++] = applet_event; return;
        case 1:
            if (!focus_message_pending) { resp->result = (3 << 9) | 128; return; }
            focus_message_pending = 0; runtime_event_set(applet_event, 0);
            resp->size = 4; *(uint32_t *)resp->data = 15; return;
        case 5: case 6: case 9: resp->size = 4; *(uint32_t *)resp->data = 1; return;
        case 8: resp->size = 4; return;
        case 60: resp->size = 8; *(uint32_t *)resp->data = 1920; *(uint32_t *)(resp->data + 4) = 1080; return;
        }
    }
    if (!strcmp(service->name, "ISelfController")) {
        switch (req->command) {
        case 10: case 11: case 12: case 13: case 14: case 16: case 62: case 100:
            service->state = req->size >= 4 ? *(uint32_t *)req->data : 0; return;
        case 9: case 91: resp->copy[resp->ncopy++] = runtime_event_create(0); return;
        case 90: resp->size = 8; return; // no suspension has occurred
        case 40: resp->size = 8; *(uint64_t *)resp->data = 1; return;
        case 69: resp->size = 1; return;
        }
    }
    if (!strcmp(service->name, "IWindowController")) {
        if (req->command == 1) { resp->size = 8; *(uint64_t *)resp->data = 1; return; }
        if (req->command == 10) { service->state = 1; return; } // acquire applet foreground rights
    }
    if (!strcmp(service->name, "IApplicationFunctions")) {
        if(req->command==20){resp->result=runtime_ensure_save(1);resp->size=8;return;}
        if (req->command == 1) {
            if(req->size>=4 && *(uint32_t *)req->data==2 && !launch_parameter_taken){
                uint8_t *parameter=calloc(1,0x88);*(uint32_t *)parameter=0xC79497CA;*(uint32_t *)(parameter+4)=1;memcpy(parameter+8,local_uid,16);
                Service *storage=service_new("ILaunchStorage");storage->userdata=parameter;storage->state=0x88;
                resp->objects[resp->count++]=storage;launch_parameter_taken=1;return;
            }
            resp->result = (2 << 9) | 128; return;
        }
        if (req->command == 21) { resp->size = 8; memcpy(resp->data, "en-US", 5); return; }
        if (req->command == 22) { service->state = req->size >= 4 ? *(uint32_t *)req->data : 0; return; }
        if (req->command == 23) { resp->size=16;memcpy(resp->data,"1.0.0",6);return; }
        if (req->command == 40) { resp->size = 1; return; }
        // This local profile has no recorded application play history.
        if (req->command == 110 || req->command == 111) { resp->size = 4; return; }
    }
    if (!strcmp(service->name, "apm")) {
        if (req->command == 0) { resp->objects[resp->count++] = service_new("IPerformanceSession"); return; }
        if (req->command == 1) { resp->size = 4; *(uint32_t *)resp->data = 1; return; }
    }
    if (!strcmp(service->name, "IPerformanceSession")) {
        if (req->command == 0) { service->state = req->size >= 8 ? *(uint32_t *)(req->data + 4) : 0; return; }
        if (req->command == 1) { resp->size = 4; *(uint32_t *)resp->data = 0x92220008; return; }
    }
    if ((!strcmp(service->name, "pctl") || !strcmp(service->name, "pctl:s") || !strcmp(service->name, "pctl:a") || !strcmp(service->name, "pctl:r")) && (req->command == 0 || req->command == 1)) {
        resp->objects[resp->count++] = service_new("IParentalControlService"); return;
    }
    if (!strcmp(service->name, "IParentalControlService")) {
        if (req->command == 0 || req->command == 1) { service->state = 1; return; }
        if (req->command == 1031 || req->command == 1035 || req->command == 1018 || req->command == 1032) { resp->size = 4; return; }
    }
    if (!strcmp(service->name, "aoc:u")) {
        if (req->command == 2 || req->command == 3) { resp->size = 4; return; } // base-game data only
        if (req->command == 5) { resp->size = 8; *(uint64_t *)resp->data = 0x01003FB00C5A9000ull; return; }
        if (req->command == 8) { resp->copy[resp->ncopy++] = runtime_event_create(0); return; }
        if (req->command == 100 || req->command == 101) {
            Service *manager=service_new("IPurchaseEventManager");manager->state=runtime_event_create(0);resp->objects[resp->count++]=manager;return;
        }
    }
    if(!strcmp(service->name,"IPurchaseEventManager")){
        if(req->command==0||req->command==1)return;
        if(req->command==2){resp->copy[resp->ncopy++]=(uint32_t)service->state;return;}
    }
    if(!strcmp(service->name,"bcat:u")){
        if(req->command==0){resp->objects[resp->count++]=service_new("IBcatService");return;}
        if(req->command==1||req->command==2){resp->objects[resp->count++]=service_new("IDeliveryCacheStorageService");return;}
    }
    if(!strcmp(service->name,"IDeliveryCacheStorageService")){
        if(req->command==10){resp->size=4;return;} // empty local delivery cache
        if(req->command==0||req->command==1){resp->objects[resp->count++]=service_new(req->command?"IEmptyDeliveryDirectory":"IEmptyDeliveryFile");return;}
    }
    if((!strcmp(service->name,"IEmptyDeliveryFile")||!strcmp(service->name,"IEmptyDeliveryDirectory"))&&req->command==0){resp->result=(2<<9)|122;return;}
    if(!strcmp(service->name,"friend:u")){
        if(req->command==0){resp->objects[resp->count++]=service_new("IFriendService");return;}
        if(req->command==1){Service *notifications=service_new("IFriendNotifications");notifications->state=runtime_event_create(0);resp->objects[resp->count++]=notifications;return;}
    }
    if(!strcmp(service->name,"nifm:u")&&(req->command==4||req->command==5)){
        resp->objects[resp->count++]=service_new("INetworkGeneralService");return;
    }
    if(!strcmp(service->name,"INetworkGeneralService")){
        if(req->command==4){
            Service *request=service_new("INetworkRequest");
            uint32_t *events=calloc(2,sizeof *events);
            events[0]=runtime_event_create(0);events[1]=runtime_event_create(0);
            request->userdata=events;request->state=1;
            resp->objects[resp->count++]=request;return;
        }
        if(req->command==18){resp->size=3;return;} // disconnected
        if(req->command==17||req->command==21||req->command==22){resp->size=1;return;}
    }
    if(!strcmp(service->name,"INetworkRequest")){
        uint32_t *events=service->userdata;
        switch(req->command){
        case 0:resp->size=4;*(uint32_t *)resp->data=1;return;
        case 1:resp->result=(311<<9)|110;return; // no submitted/accepted connection
        case 2:resp->copy[resp->ncopy++]=events[0];resp->copy[resp->ncopy++]=events[1];return;
        case 3:case 4:service->state=1;runtime_event_set(events[0],1);return;
        case 5:case 6:case 8:case 9:case 10:case 11:case 12:case 13:
        case 14:case 15:case 16:case 17:case 18:case 23:
            return; // local request options; native port remains offline
        }
    }
    if (!strcmp(service->name, "time:u") || !strcmp(service->name, "time:s") || !strcmp(service->name, "time:a")) {
        switch (req->command) {
        case 0: case 1: case 4: case 5: resp->objects[resp->count++] = service_new("ISystemClock"); return;
        case 2: resp->objects[resp->count++] = service_new("ISteadyClock"); return;
        case 3: resp->objects[resp->count++] = service_new("ITimeZoneService"); return;
        case 20:
            if (!time_shared) { time_shared = runtime_shared_create(0x1000); runtime_shared_callback(time_shared, time_mapped); }
            resp->copy[resp->ncopy++] = time_shared; return;
        case 100: case 101: resp->size = 1; return;
        case 500: case 501: {
            Buffer first=req->nx>0?req->x[0]:(Buffer){0},second=req->nx>1?req->x[1]:(Buffer){0};
            if(first.size<0xD0||second.size<0xD0){resp->result=BAD_MESSAGE;return;}
            const uint8_t *a=(const uint8_t *)(uintptr_t)first.address,*b=(const uint8_t *)(uintptr_t)second.address;
            int64_t start,end;
            if(req->command==500){
                if(memcmp(a+0x10,b+0x10,16)){resp->result=(102<<9)|116;return;}
                start=*(int64_t *)a;end=*(int64_t *)b;
            }else if(!memcmp(a+0x98,b+0x98,16)){
                start=*(int64_t *)(a+0x90);end=*(int64_t *)(b+0x90);
            }else if(*(int64_t *)(a+0x48)&&*(int64_t *)(b+0x48)){
                start=*(int64_t *)(a+0x48);end=*(int64_t *)(b+0x48);
            }else{resp->result=(102<<9)|116;return;}
            __int128 span=((__int128)end-start)*1000000000;
            if(span>INT64_MAX||span<INT64_MIN){resp->result=BAD_MESSAGE;return;}
            resp->size=8;*(int64_t *)resp->data=(int64_t)span;return;
        }
        case 400: case 401: {
            Buffer buffer=req->nb&&req->b[0].size?req->b[0]:req->nc?req->c[0]:(Buffer){0};
            if(buffer.size<0xD0 || (req->command==401 && req->size<0x48)){resp->result=BAD_MESSAGE;return;}
            uint8_t snapshot[0xD0]={0};
            if(req->command==401){memcpy(snapshot,req->data+8,32);memcpy(snapshot+0x20,req->data+0x28,32);}
            else{clock_context(snapshot);clock_context(snapshot+0x20);}
            int64_t steady=(int64_t)(rt_ticks()/19200000),user=steady+*(int64_t *)snapshot,network=steady+*(int64_t *)(snapshot+0x20);
            memcpy(snapshot+0x40,&user,8);memcpy(snapshot+0x48,&network,8);
            calendar_fields(user,snapshot+0x50,snapshot+0x60);calendar_fields(network,snapshot+0x58,snapshot+0x78);
            memcpy(snapshot+0x90,&steady,8);memcpy(snapshot+0x98,clock_uuid,16);memcpy(snapshot+0xA8,"UTC",4);
            snapshot[0xCD]=req->data[0];memcpy((void *)(uintptr_t)buffer.address,snapshot,sizeof snapshot);return;
        }
        }
    }
    if (!strcmp(service->name, "ISystemClock")) {
        if (req->command == 0) { resp->size = 8; *(int64_t *)resp->data = (int64_t)time(NULL); return; }
        if (req->command == 2) { resp->size = 32; clock_context(resp->data); return; }
    }
    if (!strcmp(service->name, "ISteadyClock") && req->command == 0) {
        resp->size = 24; *(int64_t *)resp->data = rt_ticks() / 19200000; memcpy(resp->data + 8, clock_uuid, 16); return;
    }
    if (!strcmp(service->name, "ITimeZoneService")) {
        if (req->command == 0) { resp->size = 0x24; memcpy(resp->data, "UTC", 4); return; }
        if(req->command==202&&req->size>=8){
            struct tm calendar={0};calendar.tm_year=*(uint16_t *)req->data-1900;calendar.tm_mon=req->data[2]-1;calendar.tm_mday=req->data[3];
            calendar.tm_hour=req->data[4];calendar.tm_min=req->data[5];calendar.tm_sec=req->data[6];
            int64_t timestamp=(int64_t)_mkgmtime(&calendar);
            Buffer buffer=req->nb&&req->b[0].size?req->b[0]:req->nc?req->c[0]:(Buffer){0};
            if(buffer.size<8){resp->result=BAD_MESSAGE;return;}
            memcpy((void *)(uintptr_t)buffer.address,&timestamp,8);resp->size=4;*(uint32_t *)resp->data=timestamp>=0?1:0;return;
        }
        if ((req->command == 100 || req->command == 101) && req->size >= 8) {
            time_t timestamp = (time_t)*(int64_t *)req->data;
            struct tm *calendar = gmtime(&timestamp);
            if (!calendar) { resp->result = BAD_MESSAGE; return; }
            resp->size = 32; *(uint16_t *)resp->data = calendar->tm_year + 1900;
            resp->data[2] = calendar->tm_mon + 1; resp->data[3] = calendar->tm_mday;
            resp->data[4] = calendar->tm_hour; resp->data[5] = calendar->tm_min; resp->data[6] = calendar->tm_sec;
            *(uint32_t *)(resp->data + 8) = calendar->tm_wday; *(uint32_t *)(resp->data + 12) = calendar->tm_yday;
            memcpy(resp->data + 16, "UTC", 4); return;
        }
    }
    if (!strcmp(service->name, "set")) {
        static const char *languages[] = {"ja","en-US","fr","de","it","es","zh-CN","ko","nl","pt","ru","zh-TW","en-GB","fr-CA","es-419","zh-Hans","zh-Hant","pt-BR"};
        unsigned max = sizeof languages / sizeof *languages;
        switch (req->command) {
        case 0: resp->size = 8; memcpy(resp->data, "en-US", 5); return;
        case 1: max = 15; // fall through to language-list transfer
        case 5: {
            Buffer buffer = req->nb && req->b[0].size ? req->b[0] : req->nc ? req->c[0] : (Buffer){0};
            unsigned n = buffer.size / 8 < max ? (unsigned)(buffer.size / 8) : max;
            for (unsigned i = 0; i < n; ++i) {
                uint8_t *dest = (uint8_t *)(uintptr_t)(buffer.address + i * 8);
                memset(dest, 0, 8); memcpy(dest, languages[i], strlen(languages[i]));
            }
            resp->size = 4; *(uint32_t *)resp->data = n; return;
        }
        case 2:
            if (req->size < 4 || *(uint32_t *)req->data >= max) { resp->result = BAD_MESSAGE; return; }
            resp->size = 8; memcpy(resp->data, languages[*(uint32_t *)req->data], strlen(languages[*(uint32_t *)req->data])); return;
        case 3: resp->size = 4; *(uint32_t *)resp->data = 15; return;
        case 6: resp->size = 4; *(uint32_t *)resp->data = max; return;
        case 4: resp->size = 4; *(uint32_t *)resp->data = 1; return;
        case 8: resp->size = 1; return;
        }
    }
    if (!strcmp(service->name, "fsp-srv")) {
        switch (req->command) {
        case 1: return; // associate the local process with this session
        case 22:resp->result=runtime_ensure_save(1);return;
        case 51:case 53:{
            unsigned type=req->size>0x20?req->data[0x20]:1;
            Service *fs=runtime_save_filesystem(type);if(!fs){resp->result=0x202;return;}
            resp->objects[resp->count++]=fs;return;
        }
        case 200: resp->objects[resp->count++] = service_new("IRomfsStorage"); return;
        case 202: resp->result = (1001 << 9) | 2; return; // system data archive is not supplied
        case 1005: resp->size = 4; return; // access logging disabled
        case 1003: case 1004: case 1006: service->state = req->size >= 4 ? *(uint32_t *)req->data : 0; return;
        }
    }
    if (!strcmp(service->name, "IRomfsStorage")) {
        if (req->command == 4) { resp->size = 8; *(uint64_t *)resp->data = runtime_romfs_size(); return; }
        if (req->command == 0 && req->size >= 16) {
            uint64_t offset = *(uint64_t *)req->data, size = *(uint64_t *)(req->data + 8);
            Buffer buffer = req->nb && req->b[0].size ? req->b[0] : req->nc ? req->c[0] : (Buffer){0};
            if (size > buffer.size || !runtime_romfs_read(offset, size, (void *)(uintptr_t)buffer.address)) resp->result = (1 << 9) | 2;
            return;
        }
    }
    if (!strcmp(service->name, "hid")) {
        switch (req->command) {
        case 0: resp->objects[resp->count++] = service_new("IAppletResource"); return;
        case 11: case 21: case 31: case 66: case 67: case 69: case 70: case 79:
        case 100: case 102: case 103: case 104: case 109: case 120: case 122: case 123: case 124:
            service->state = req->size >= 4 ? *(uint32_t *)req->data : 0; return;
        case 40: case 106: resp->copy[resp->ncopy++] = runtime_event_create(1); return;
        case 101: resp->size = 4; *(uint32_t *)resp->data = 1; return; // FullKey controller
        case 82: resp->size=1;resp->data[0]=1;return; // native controller has no motion input
        case 83: resp->size=1;return; // no controller firmware update
        case 121: case 129: resp->size = 8; return;
        case 200: resp->size = 16; *(uint32_t *)resp->data = 1; return;
        case 201:
            if(req->size<32){resp->result=BAD_MESSAGE;return;}
#ifdef PORT_GRAPHICS
            runtime_input_rumble(*(uint32_t *)req->data,(const float *)(req->data+4));
#endif
            return;
        case 202:
            if(req->size<16){resp->result=BAD_MESSAGE;return;}resp->size=16;
#ifdef PORT_GRAPHICS
            runtime_input_rumble_get(*(uint32_t *)req->data,(float *)resp->data);
#endif
            return;
        case 203: resp->objects[resp->count++] = service_new("IActiveVibrationDeviceList"); return;
        }
    }
    if (!strcmp(service->name, "IAppletResource") && req->command == 0) {
        if (!hid_shared) hid_shared = runtime_shared_create(0x40000);
        resp->copy[resp->ncopy++] = hid_shared; return;
    }
    if (!strcmp(service->name, "IActiveVibrationDeviceList") && req->command == 0) return;
    if (!strcmp(service->name,"ILaunchStorage") && req->command==0){
        Service *accessor=service_new("ILaunchStorageAccessor");accessor->userdata=service->userdata;accessor->state=service->state;resp->objects[resp->count++]=accessor;return;
    }
    if (!strcmp(service->name,"ILaunchStorageAccessor")){
        if(req->command==0){resp->size=8;*(uint64_t *)resp->data=service->state;return;}
        if(req->command==11&&req->size>=8){
            uint64_t offset=*(uint64_t *)req->data;Buffer buffer=req->nb&&req->b[0].size?req->b[0]:req->nc?req->c[0]:(Buffer){0};
            if(offset>service->state||buffer.size>service->state-offset){resp->result=BAD_MESSAGE;return;}
            memcpy((void *)(uintptr_t)buffer.address,(uint8_t *)service->userdata+offset,buffer.size);return;
        }
    }
    if(!strcmp(service->name,"acc:u0")){
        switch(req->command){
        case 0:resp->size=4;*(uint32_t *)resp->data=1;return;
        case 1:resp->size=1;resp->data[0]=req->size>=16&&!memcmp(req->data,local_uid,16);return;
        case 2:case 3:case 141:{
            Buffer buffer=req->nb&&req->b[0].size?req->b[0]:req->nc?req->c[0]:(Buffer){0};
            if(buffer.size>4096){resp->result=BAD_MESSAGE;return;}
            memset((void *)(uintptr_t)buffer.address,0,buffer.size);if(buffer.size>=16)memcpy((void *)(uintptr_t)buffer.address,local_uid,16);return;
        }
        case 4:case 51:resp->size=16;memcpy(resp->data,local_uid,16);return;
        case 5:resp->objects[resp->count++]=service_new("ILocalUserProfile");return;
        case 50:case 150:resp->size=1;return;
        case 100:case 140:service->state=1;return;
        case 101:resp->objects[resp->count++]=service_new("ILocalAccountManager");return;
        }
    }
    if(!strcmp(service->name,"ILocalAccountManager")){
        if(req->command==0||req->command==1){resp->result=0xF601;return;} // Nintendo online accounts are unavailable on the host
    }
    if(!strcmp(service->name,"nfp:user")&&req->command==0){resp->objects[resp->count++]=service_new("INfpUser");return;}
    if(!strcmp(service->name,"INfpUser")){
        switch(req->command){
        case 0:service->state=1;return;
        case 1:service->state=0;return;
        case 2:resp->size=4;return; // no NFC reader is attached
        case 19:resp->size=4;*(uint32_t *)resp->data=(uint32_t)service->state;return;
        case 17:case 18:case 23:resp->copy[resp->ncopy++]=runtime_event_create(0);return;
        }
    }
    if(!strcmp(service->name,"ILocalUserProfile")){
        if(req->command==0||req->command==1){
            resp->size=0x38;memcpy(resp->data,local_uid,16);memcpy(resp->data+24,"Player",7);
            Buffer buffer=req->nb&&req->b[0].size?req->b[0]:req->nc?req->c[0]:(Buffer){0};
            if(buffer.size&&buffer.size<4096)memset((void *)(uintptr_t)buffer.address,0,buffer.size);return;
        }
        if(req->command==10||req->command==11){resp->size=4;return;}
    }
    fprintf(stderr, "[missing-service] %s command=%u\n", service->name, req->command);
    runtime_stop("unimplemented_service_command", 0, 91);
}

// Message word indices are bounded before descriptor/payload access.
static int take(unsigned *index, unsigned count) {
    if (*index > 64 || count > 64 - *index) return 0;
    *index += count; return 1;
}
static uint64_t ab_address(uint32_t *w) {
    return w[1] | ((uint64_t)(w[2] >> 28) << 32) | ((uint64_t)((w[2] >> 2) & 7) << 36);
}
uint32_t runtime_send_request(Cpu *cpu, uint32_t handle) {
    Session *session = session_get(handle);
    if (!session) return BAD_HANDLE;
    uint32_t *msg = (uint32_t *)(uintptr_t)cpu->tpidrro;
    Request req = {0}; Response resp = {0};
    req.type = msg[0] & 0xFFFF;
    // HIPC Close releases the server-side connection; the client still owns
    // the kernel handle and closes it with svcCloseHandle afterwards.
    if (req.type == 2) return 0;
    req.nx = (msg[0] >> 16) & 15; req.na = (msg[0] >> 20) & 15; req.nb = (msg[0] >> 24) & 15;
    unsigned receive_mode = (msg[1] >> 10) & 15, raw_words = msg[1] & 1023, index = 2;
    if (req.nx > 8 || req.na > 8 || req.nb > 8) return BAD_MESSAGE;
    if (msg[1] & 0x80000000) {
        if (!take(&index, 1)) return BAD_MESSAGE;
        uint32_t hd = msg[index - 1];
        unsigned copy = (hd >> 1) & 15, move = (hd >> 5) & 15;
        if (copy > 8 || move > 8 || !take(&index, (hd & 1 ? 2 : 0) + copy + move)) return BAD_MESSAGE;
    }
    for (unsigned i = 0; i < req.nx; ++i) {
        unsigned old = index; if (!take(&index, 2)) return BAD_MESSAGE;
        uint32_t *p = msg + old;
        req.x[i] = (Buffer){p[1] | ((uint64_t)((p[0] >> 12) & 15) << 32) | ((uint64_t)((p[0] >> 6) & 7) << 36), p[0] >> 16};
    }
    for (unsigned group = 0; group < 2; ++group) {
        unsigned n = group ? req.nb : req.na; Buffer *buffers = group ? req.b : req.a;
        for (unsigned i = 0; i < n; ++i) {
            unsigned old = index; if (!take(&index, 3)) return BAD_MESSAGE;
            uint32_t *p = msg + old;
            buffers[i] = (Buffer){ab_address(p), p[0] | ((uint64_t)((p[2] >> 24) & 15) << 32)};
        }
    }
    unsigned raw_start = index;
    if (!take(&index, raw_words)) return BAD_MESSAGE;
    unsigned raw_end = index;
    if (receive_mode >= 2) {
        req.nc = receive_mode == 2 ? 1 : receive_mode - 2;
        if (req.nc > 8 || !take(&index, req.nc * 2)) return BAD_MESSAGE;
        for (unsigned i = 0; i < req.nc; ++i) {
            uint32_t *p = msg + raw_end + i * 2;
            req.c[i] = (Buffer){p[0] | ((uint64_t)(p[1] & 0xFFFF) << 32), p[1] >> 16};
        }
    }
    unsigned payload = (raw_start + 3) & ~3u, domain = 0, target = 0;
    if (session->domain && (req.type == 4 || req.type == 6)) {
        if (payload + 4 > raw_end) return BAD_MESSAGE;
        domain = 1; target = msg[payload + 1];
        if (target >= 256 || !session->domain->objects[target]) return BAD_HANDLE;
        unsigned domain_cmd = msg[payload] & 255;
        if (domain_cmd == 2) {
            AcquireSRWLockExclusive(&session_lock);
            Service *closed=session->domain->objects[target];session->domain->objects[target]=NULL;
            ReleaseSRWLockExclusive(&session_lock);service_release(closed);goto respond;
        }
        if (domain_cmd != 1) return BAD_MESSAGE;
        payload += 4;
    }
    if (payload + 4 > raw_end || memcmp(msg + payload, "SFCI", 4)) return BAD_MESSAGE;
    req.command = msg[payload + 2]; req.data = (uint8_t *)(msg + payload + 4); req.size = (raw_end - payload - 4) * 4;
    if (req.type == 4 || req.type == 6) {
        service_call(domain ? session->domain->objects[target] : session->service, &req, &resp);
    } else if (req.type == 5 || req.type == 7) {
        fprintf(stderr, "[ipc-control] %s cmd=%u\n", session->service->name, req.command);
        switch (req.command) {
        case 0:
            if (session->domain) return BAD_MESSAGE;
            session->domain = calloc(1, sizeof(Domain));
            if (!session->domain) runtime_stop("domain_allocation_failed", 0, 90);
            session->domain->objects[1] = session->service;
            session->domain->references=1; // transfer the session's root service reference
            resp.size = 4; *(uint32_t *)resp.data = 1; break;
        case 2: case 4:
            resp.size = 0;
            // Returned below as a moved handle sharing the domain.
            resp.objects[resp.count++] = session->service; break;
        case 3: resp.size = 4; *(uint16_t *)resp.data = 0x8000; break;
        default: return BAD_MESSAGE;
        }
    } else return BAD_MESSAGE;
respond:;
    uint32_t handles[8], object_ids[8];
    for (unsigned i = 0; i < resp.count; ++i) {
        if (domain) {
            AcquireSRWLockExclusive(&session_lock);
            unsigned slot = 1;
            while (slot < 256 && session->domain->objects[slot]) ++slot;
            if (slot == 256) {ReleaseSRWLockExclusive(&session_lock);return BAD_MESSAGE;}
            service_retain(resp.objects[i]);
            session->domain->objects[slot] = resp.objects[i]; object_ids[i] = slot;
            ReleaseSRWLockExclusive(&session_lock);
        } else handles[i] = session_new(resp.objects[i], (req.type == 5 || req.type == 7) ? session->domain : NULL);
    }
    memset(msg, 0, 256);
    unsigned move_count = domain ? 0 : resp.count;
    unsigned has_handles = resp.ncopy || move_count;
    index = 2;
    if (has_handles) {
        msg[1] = 0x80000000; msg[index++] = (move_count << 5) | (resp.ncopy << 1);
        for (unsigned i = 0; i < resp.ncopy; ++i) {
            uint32_t copied=runtime_kernel_duplicate(resp.copy[i]);
            if(!copied)return BAD_HANDLE;msg[index++] = copied;
        }
        for (unsigned i = 0; i < move_count; ++i) msg[index++] = handles[i];
    }
    unsigned raw = index;
    index = (index + 3) & ~3u;
    if (domain) { msg[index] = resp.count; index += 4; }
    if (index + 4 + (resp.size + 3) / 4 + (domain ? resp.count : 0) > 64) return BAD_MESSAGE;
    memcpy(msg + index, "SFCO", 4); msg[index + 2] = resp.result;
    memcpy(msg + index + 4, resp.data, resp.size);
    index += 4 + (resp.size + 3) / 4;
    if (domain) for (unsigned i = 0; i < resp.count; ++i) msg[index++] = object_ids[i];
    // HIPC reserves four words for alignment, including trailing padding.
    msg[1] |= index - ((raw + 3) & ~3u) + 4;
    return 0;
}
