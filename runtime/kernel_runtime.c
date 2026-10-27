#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <process.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel_runtime.h"
#include "ipc_runtime.h"

enum { OBJ_THREAD = 1, OBJ_EVENT, OBJ_SHARED, OBJ_TRANSFER, OBJ_MAPPING };
typedef struct Object {
    int type; HANDLE native;
    Cpu cpu;
    uint64_t entry, argument, size, address, id;
    int priority, core;
    uint32_t handle;
    void (*on_map)(uint64_t);
} Object;
typedef struct Key { uint64_t address, generation; CONDITION_VARIABLE cv; unsigned waiters, wake_tokens; } Key;
static Object *objects[2048];
static SRWLOCK object_lock = SRWLOCK_INIT, wait_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE mutex_cv = CONDITION_VARIABLE_INIT;
static Key keys[256];
static uint64_t tls_start, tls_slot = 1, next_id = 2;
static __thread Object *current_thread;

static Object *get(uint32_t handle) {
    if (handle == 0xFFFF8000 || handle == 0x101) return current_thread;
    unsigned i = handle - 0x1000;
    return i < 2048 ? objects[i] : NULL;
}
int runtime_kernel_memory(uint64_t address, uint64_t *base, uint64_t *size) {
    uint64_t next = UINT64_MAX;
    for (unsigned i = 1; i < 2048; ++i) {
        Object *object = objects[i];
        if (!object || (object->type != OBJ_SHARED && object->type != OBJ_MAPPING) || !object->address) continue;
        if (address >= object->address && address - object->address < object->size) {
            *base = object->address; *size = object->size; return 1;
        }
        if (object->address > address && object->address < next) next = object->address;
    }
    *base = next; *size = 0; return 0;
}
static uint32_t add(Object *object) {
    AcquireSRWLockExclusive(&object_lock);
    for (unsigned i = 1; i < 2048; ++i) if (!objects[i]) {
        objects[i] = object; object->handle = 0x1000 + i;
        ReleaseSRWLockExclusive(&object_lock); return object->handle;
    }
    ReleaseSRWLockExclusive(&object_lock);
    runtime_stop("kernel_handle_limit", 0, 93);
}
void runtime_kernel_init(Cpu *cpu, uint64_t tls_base) {
    tls_start = tls_base;
    Object *main = calloc(1, sizeof *main);
    main->type = OBJ_THREAD; main->cpu = *cpu; main->id = 1; main->priority = 44; main->handle = 0x101;
    current_thread = main;
}
uint32_t runtime_event_create(int signaled) {
    Object *object = calloc(1, sizeof *object); object->type = OBJ_EVENT;
    object->native = CreateEventW(NULL, TRUE, signaled, NULL);
    if (!object->native) runtime_stop("event_create_failed", 0, 93);
    return add(object);
}
void runtime_event_set(uint32_t handle, int signaled) {
    Object *object = get(handle);
    if (!object || object->type != OBJ_EVENT) runtime_stop("invalid_event", handle, 93);
    if (signaled) SetEvent(object->native); else ResetEvent(object->native);
}
uint32_t runtime_shared_create(uint64_t size) {
    Object *object = calloc(1, sizeof *object); object->type = OBJ_SHARED; object->size = size;
    return add(object);
}
void runtime_shared_callback(uint32_t handle, void (*on_map)(uint64_t)) {
    Object *object = get(handle);
    if (!object || object->type != OBJ_SHARED) runtime_stop("invalid_shared_object", handle, 93);
    object->on_map = on_map;
}
uint32_t runtime_kernel_close(uint32_t handle) {
    Object *object = get(handle);
    if (!object || object == current_thread) return 0xE401;
    AcquireSRWLockExclusive(&object_lock);
    objects[handle - 0x1000] = NULL;
    ReleaseSRWLockExclusive(&object_lock);
    if (object->native) CloseHandle(object->native);
    // Thread state may still be used until its native entry exits.
    if (object->type != OBJ_THREAD) free(object);
    return 0;
}
uint32_t runtime_kernel_duplicate(uint32_t handle) {
    Object *source=get(handle);if(!source)return 0;
    Object *copy=calloc(1,sizeof *copy);*copy=*source;
    if(source->native&&!DuplicateHandle(GetCurrentProcess(),source->native,GetCurrentProcess(),&copy->native,0,FALSE,DUPLICATE_SAME_ACCESS)){free(copy);return 0;}
    return add(copy);
}
static DWORD timeout_ms(int64_t ns) {
    if (ns < 0) return INFINITE;
    if (ns / 1000000 >= INFINITE - 1) return INFINITE - 1;
    return (DWORD)((ns + 999999) / 1000000);
}
static unsigned __stdcall thread_entry(void *argument) {
    Object *thread = argument; current_thread = thread;
    thread->cpu.x[0] = thread->argument; thread->cpu.x[1] = thread->handle;
    thread->cpu.pc = thread->entry; thread->cpu.x[30] = 0;
    fprintf(stderr, "[thread-start] handle=%x entry=%" PRIx64 "\n", thread->handle, thread->entry);
    rt_dispatch(&thread->cpu);
    fprintf(stderr, "[thread-exit] handle=%x\n", thread->handle);
    return 0;
}
static Key *key_get(uint64_t address) {
    for (unsigned i = 0; i < 256; ++i) {
        if (keys[i].address == address) return &keys[i];
        if (!keys[i].address) { keys[i].address = address; InitializeConditionVariable(&keys[i].cv); return &keys[i]; }
    }
    runtime_stop("condition_key_limit", address, 93);
}
static void lock_guest(uint64_t address, uint32_t tag) {
    volatile LONG *word = (volatile LONG *)(uintptr_t)address;
    for (;;) {
        LONG old = InterlockedCompareExchange(word, (LONG)tag, 0);
        if (old == 0 || (old & ~0x40000000u) == tag) return;
        // The guest's fast unlock can run outside wait_lock. Set the waiter bit
        // only while the observed owner is still present, otherwise retry: an
        // unconditional OR can turn an unlocked mutex into a permanent orphan.
        if ((old & ~0x40000000u) == 0) {
            if (InterlockedCompareExchange(word,(LONG)tag,old)==old) return;
            continue;
        }
        if (InterlockedCompareExchange(word,old|0x40000000,old)!=old) continue;
        SleepConditionVariableSRW(&mutex_cv, &wait_lock, INFINITE, 0);
    }
}
int runtime_kernel_svc(Cpu *cpu, uint32_t svc) {
    uint64_t *x = cpu->x; Object *object;
    switch (svc) {
    case 0x02: case 0x03: x[0] = 0; return 1; // host CPU memory is coherent; guest code is native C
    case 0x04: {
        if (x[0] < 0x7000000000ull || x[0] >= 0x7320000000ull || x[2] > 0x7320000000ull - x[0] ||
            !VirtualAlloc((void *)(uintptr_t)x[0], x[2], MEM_COMMIT, PAGE_READWRITE)) runtime_stop("memory_map_failed", x[0], 93);
        memcpy((void *)(uintptr_t)x[0], (const void *)(uintptr_t)x[1], (size_t)x[2]);
        object = calloc(1, sizeof *object); object->type = OBJ_MAPPING; object->address = x[0]; object->size = x[2]; add(object);
        x[0] = 0; return 1;
    }
    case 0x05:
        memcpy((void *)(uintptr_t)x[1], (const void *)(uintptr_t)x[0], (size_t)x[2]);
        for (unsigned i = 1; i < 2048; ++i) if (objects[i] && objects[i]->type == OBJ_MAPPING && objects[i]->address == x[0]) {
            free(objects[i]); objects[i] = NULL; break;
        }
        VirtualFree((void *)(uintptr_t)x[0], (SIZE_T)x[2], MEM_DECOMMIT); x[0] = 0; return 1;
    case 0x08: {
        Object *thread = calloc(1, sizeof *thread); thread->type = OBJ_THREAD;
        thread->entry = x[1]; thread->argument = x[2]; thread->cpu.sp = x[3] & ~15ull;
        thread->priority = (int)x[4]; thread->core = (int)x[5]; thread->cpu.excl_addr = UINT64_MAX;
        AcquireSRWLockExclusive(&object_lock);
        thread->id = next_id++; thread->cpu.tpidrro = tls_start + tls_slot++ * 0x200;
        ReleaseSRWLockExclusive(&object_lock);
        if (tls_slot >= 2048) runtime_stop("tls_slot_limit", 0, 93);
        uint32_t handle = add(thread);
        thread->native = (HANDLE)_beginthreadex(NULL, 64 * 1024 * 1024, thread_entry, thread,
                CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!thread->native) runtime_stop("thread_create_failed", thread->entry, 93);
        fprintf(stderr, "[thread-create] handle=%x entry=%" PRIx64 " stack=%" PRIx64 "\n", handle, thread->entry, thread->cpu.sp);
        x[0] = 0; x[1] = handle; return 1;
    }
    case 0x09:
        object = get((uint32_t)x[0]);
        x[0] = !object || object->type != OBJ_THREAD || ResumeThread(object->native) == (DWORD)-1 ? 0xE401 : 0; return 1;
    case 0x0A: _endthreadex(0); return 1;
    case 0x0B: if ((int64_t)x[0] <= 0) SwitchToThread(); else Sleep(timeout_ms((int64_t)x[0])); x[0] = 0; return 1;
    case 0x0C:
        object = get((uint32_t)x[1]); if (!object || object->type != OBJ_THREAD) x[0] = 0xE401;
        else { x[0] = 0; x[1] = object->priority; } return 1;
    case 0x0D:
        object = get((uint32_t)x[0]); if (!object || object->type != OBJ_THREAD) x[0] = 0xE401;
        else { object->priority = (int)x[1]; x[0] = 0; } return 1;
    case 0x0E:
        object = get((uint32_t)x[2]); if (!object || object->type != OBJ_THREAD) x[0] = 0xE401;
        else { x[0] = 0; x[1] = object->core < 0 ? 0 : object->core; x[2] = 15; } return 1;
    case 0x0F:
        object = get((uint32_t)x[0]); if (!object || object->type != OBJ_THREAD) x[0] = 0xE401;
        else { object->core = (int)x[1]; x[0] = 0; } return 1;
    case 0x10: x[0] = current_thread && current_thread->core >= 0 ? current_thread->core : 0; return 1;
    case 0x11: case 0x12: case 0x17:
        object = get((uint32_t)x[0]); if (!object || object->type != OBJ_EVENT) x[0] = 0xE401;
        else { runtime_event_set((uint32_t)x[0], svc == 0x11); x[0] = 0; } return 1;
    case 0x13:
        object = get((uint32_t)x[0]);
        if (!object || object->type != OBJ_SHARED || x[2] > object->size) { x[0] = 0xE401; return 1; }
        if (x[1] < 0x7000000000ull || x[1] >= 0x7320000000ull || x[2] > 0x7320000000ull - x[1] ||
            !VirtualAlloc((void *)(uintptr_t)x[1], x[2], MEM_COMMIT, PAGE_READWRITE)) runtime_stop("shared_memory_map_failed", x[1], 93);
        object->address = x[1]; if (object->on_map) object->on_map(x[1]); x[0] = 0; return 1;
    case 0x15:
        object = calloc(1, sizeof *object); object->type = OBJ_TRANSFER; object->address = x[1]; object->size = x[2];
        x[0] = 0; x[1] = add(object); return 1;
    case 0x18: {
        if (x[2] < 1 || x[2] > 64) { x[0] = 0xF001; return 1; }
        HANDLE handles[64]; uint32_t *guest = (uint32_t *)(uintptr_t)x[1];
        for (unsigned i = 0; i < x[2]; ++i) { object = get(guest[i]); if (!object || !object->native) { x[0] = 0xE401; return 1; } handles[i] = object->native; }
        DWORD result = WaitForMultipleObjects((DWORD)x[2], handles, FALSE, timeout_ms((int64_t)x[3]));
        if (result < WAIT_OBJECT_0 + x[2]) { x[0] = 0; x[1] = result - WAIT_OBJECT_0; }
        else x[0] = result == WAIT_TIMEOUT ? 0xEA01 : 0xE401;
        return 1;
    }
    case 0x1A:
        AcquireSRWLockExclusive(&wait_lock); lock_guest(x[1], (uint32_t)x[2]); ReleaseSRWLockExclusive(&wait_lock); x[0] = 0; return 1;
    case 0x1B:
        AcquireSRWLockExclusive(&wait_lock); InterlockedExchange((volatile LONG *)(uintptr_t)x[0], 0);
        WakeAllConditionVariable(&mutex_cv); ReleaseSRWLockExclusive(&wait_lock); x[0] = 0; return 1;
    case 0x1C: {
        AcquireSRWLockExclusive(&wait_lock); Key *key = key_get(x[1]); uint64_t generation = key->generation;
        InterlockedExchange((volatile LONG *)(uintptr_t)x[0], 0); WakeAllConditionVariable(&mutex_cv);
        BOOL success = TRUE;
        while (key->generation == generation && success) success = SleepConditionVariableSRW(&key->cv, &wait_lock, timeout_ms((int64_t)x[3]), 0);
        lock_guest(x[0], (uint32_t)x[2]); ReleaseSRWLockExclusive(&wait_lock); x[0] = success ? 0 : 0xEA01; return 1;
    }
    case 0x1D:
        AcquireSRWLockExclusive(&wait_lock); { Key *key = key_get(x[0]); ++key->generation;
        if ((int32_t)x[1] == 1) WakeConditionVariable(&key->cv); else WakeAllConditionVariable(&key->cv); }
        ReleaseSRWLockExclusive(&wait_lock); x[0] = 0; return 1;
    case 0x25:
        object = get((uint32_t)x[1]); if (!object || object->type != OBJ_THREAD) x[0] = 0xE401;
        else { x[0] = 0; x[1] = object->id; } return 1;
    case 0x34: {
        AcquireSRWLockExclusive(&wait_lock);
        volatile LONG *word=(volatile LONG *)(uintptr_t)x[0];int32_t value=(int32_t)x[2],observed=InterlockedCompareExchange(word,0,0);
        uint32_t type=(uint32_t)x[1];int wait=0;
        if(type==0)wait=observed<value;
        else if(type==1){for(;;){wait=observed<value;if(!wait)break;LONG actual=InterlockedCompareExchange(word,(LONG)((uint32_t)observed-1),(LONG)observed);if(actual==observed)break;observed=actual;}}
        else if(type==2)wait=observed==value;
        else{ReleaseSRWLockExclusive(&wait_lock);x[0]=0xF001;return 1;}
        if(!wait){ReleaseSRWLockExclusive(&wait_lock);x[0]=0xFA01;return 1;}
        if(!x[3]){ReleaseSRWLockExclusive(&wait_lock);x[0]=0xEA01;return 1;}
        Key *key=key_get(x[0]);++key->waiters;
        DWORD duration=timeout_ms((int64_t)x[3]);uint64_t end=GetTickCount64()+duration;BOOL success=TRUE;
        while(!key->wake_tokens&&success){DWORD remaining=duration==INFINITE?INFINITE:GetTickCount64()>=end?0:(DWORD)(end-GetTickCount64());success=SleepConditionVariableSRW(&key->cv,&wait_lock,remaining,0);}
        if(key->wake_tokens){--key->wake_tokens;success=TRUE;}--key->waiters;
        ReleaseSRWLockExclusive(&wait_lock);x[0]=success?0:0xEA01;return 1;
    }
    case 0x35: {
        AcquireSRWLockExclusive(&wait_lock);Key *key=key_get(x[0]);
        uint32_t type=(uint32_t)x[1];int32_t expected=(int32_t)x[2],count=(int32_t)x[3],desired=expected;
        if(type==1)desired=(int32_t)((uint32_t)expected+1);
        else if(type==2){if(count<=0)desired=key->waiters?(int32_t)((uint32_t)expected-2):(int32_t)((uint32_t)expected+1);else desired=!key->waiters?(int32_t)((uint32_t)expected+1):key->waiters<=(unsigned)count?(int32_t)((uint32_t)expected-1):expected;}
        else if(type!=0){ReleaseSRWLockExclusive(&wait_lock);x[0]=0xF001;return 1;}
        if(type&&InterlockedCompareExchange((volatile LONG *)(uintptr_t)x[0],desired,expected)!=expected){ReleaseSRWLockExclusive(&wait_lock);x[0]=0xFA01;return 1;}
        unsigned grant=count<=0?key->waiters:(unsigned)count;
        key->wake_tokens=key->wake_tokens+grant<key->waiters?key->wake_tokens+grant:key->waiters;
        WakeAllConditionVariable(&key->cv);ReleaseSRWLockExclusive(&wait_lock);x[0]=0;return 1;
    }
    case 0x45: {
        uint32_t writable = runtime_event_create(0), readable = runtime_event_create(0);
        Object *w = get(writable), *r = get(readable); CloseHandle(r->native);
        if (!DuplicateHandle(GetCurrentProcess(), w->native, GetCurrentProcess(), &r->native, 0, FALSE, DUPLICATE_SAME_ACCESS)) runtime_stop("event_duplicate_failed", 0, 93);
        x[0] = 0; x[1] = writable; x[2] = readable; return 1;
    }
    default: return 0;
    }
}
