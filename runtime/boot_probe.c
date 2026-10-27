// Windows host for locally recompiled ARM64 game code, IPC and native threads.
// PORT_GRAPHICS adds the NVN/Vulkan renderer and native SDL input.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "probe_modules.h"
#include "ipc_runtime.h"
#include "kernel_runtime.h"
#ifdef PORT_GRAPHICS
#include "graphics_bridge.h"
#include <process.h>
int ui_run(void);
#endif

#define GUEST_LO 0x7000000000ull
#define GUEST_HI 0x7320000000ull
#define TLS_BASE 0x7090000000ull
#define HEAP_BASE 0x7240000000ull
#define HEAP_MAX 0xD0000000ull
#define STACK_BASE 0x7200000000ull
#define STACK_REGION_SIZE 0x40000000ull
#define STACK_SIZE 0x100000ull
#define MAIN_STACK (STACK_BASE + STACK_REGION_SIZE - STACK_SIZE)
#define SDK_INIT 0x7080000000ull
#define MAIN_INIT 0x7080000010ull

volatile int g_fnprof = 1;
static volatile LONG64 calls, svc_calls;
static __thread uint64_t last_fn;
static uint64_t heap_size;
uint64_t kernel_heap_size(void) { return heap_size; }
static uint32_t main_priority = 44;
static __thread Cpu *current;
static Cpu *main_cpu_snapshot;
static volatile LONG64 main_last_function;
static uint64_t function_budget;
static int trace_svc;
static volatile LONG stopping_thread;

static void stop(const char *reason, uint64_t pc, int code) __attribute__((noreturn));
static void stop(const char *reason, uint64_t pc, int code) {
    LONG id=(LONG)GetCurrentThreadId(),previous=InterlockedCompareExchange(&stopping_thread,id,0);
    if(previous){if(previous==id)TerminateProcess(GetCurrentProcess(),(UINT)code);Sleep(INFINITE);}
    fprintf(stderr, "[stop] %s pc=%" PRIx64 " last_fn=%" PRIx64 " functions=%" PRIu64 " svc_calls=%" PRIu64 "\n",
            reason, pc, last_fn, calls, svc_calls);
    if (current) {
        fprintf(stderr, "[regs] x0=%" PRIx64 " x1=%" PRIx64 " x2=%" PRIx64 " x3=%" PRIx64 " lr=%" PRIx64 " sp=%" PRIx64 "\n",
                current->x[0], current->x[1], current->x[2], current->x[3], current->x[30], current->sp);
        uint64_t fp=current->x[29];
        for(unsigned i=0;i<24 && fp>=GUEST_LO && fp+16<GUEST_HI && !(fp&7);++i){
            MEMORY_BASIC_INFORMATION info;
            if(!VirtualQuery((void *)(uintptr_t)fp,&info,sizeof info)||info.State!=MEM_COMMIT||fp+16>(uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize)break;
            uint64_t *frame=(uint64_t *)(uintptr_t)fp;
            fprintf(stderr,"[guest-frame] fp=%" PRIx64 " lr=%" PRIx64 "\n",fp,frame[1]);
            if(frame[0]<=fp)break;fp=frame[0];
        }
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(),(UINT)code);__builtin_unreachable();
}
void runtime_stop(const char *reason, uint64_t pc, int code) { stop(reason, pc, code); }
void runtime_capture_complete(uint64_t frames){
    fprintf(stderr,"[capture-complete] frames=%" PRIu64 " functions=%" PRIu64 " svc_calls=%" PRIu64 "\n",frames,(uint64_t)calls,(uint64_t)svc_calls);
    fflush(stderr);TerminateProcess(GetCurrentProcess(),0);
}

static LONG CALLBACK fault_handler(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[host-fault] code=%08lx address=%p", (unsigned long)code, e->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, " accessed=%" PRIxPTR, (uintptr_t)e->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    stop("host_exception", last_fn, 71);
}

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    (void)lr; (void)x0;
    last_fn = addr;
    if(current==main_cpu_snapshot)main_last_function=(LONG64)addr;
    if ((uint64_t)InterlockedIncrement64(&calls) > function_budget && function_budget) stop("function_budget", addr, 72);
}
void rt_udf(Cpu *c, uint64_t pc) { current = c; stop("untranslated_instruction_or_unreachable_data", pc, 73); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)imm; current = c; stop("guest_breakpoint", pc, 74); }
int rt_hook(Cpu *c, uint64_t pc) { current = c; stop("unexpected_game_hook", pc, 75); }
uint64_t rt_ticks(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)((__uint128_t)counter.QuadPart * 19200000 / frequency.QuadPart);
}

static void commit(uint64_t addr, uint64_t size) {
    if (addr < GUEST_LO || addr >= GUEST_HI || size > GUEST_HI - addr ||
        VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) == NULL)
        stop("guest_memory_commit_failed", addr, 76);
}

static void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

static void initialize(Cpu *c, const char *name) {
    for (size_t i = 0; i < PROBE_MODULE_COUNT; ++i) {
        const ProbeModule *m = &probe_modules[i];
        if (strcmp(name, m->name)) continue;
        fprintf(stderr, "[init] %s\n", m->name);
        if (m->init) guest_call(c, m->init);
        else for (uint64_t j = 0; j < m->init_array_size / 8; ++j) {
            uint64_t fn = ((uint64_t *)(uintptr_t)m->init_array)[j];
            if (fn) guest_call(c, fn);
        }
        return;
    }
    stop("module_missing", 0, 77);
}

void rt_dispatch(Cpu *c) {
    current = c;
#ifdef PORT_GRAPHICS
    if (runtime_hle_dispatch(c)) return;
#endif
    if (c->pc == SDK_INIT) { initialize(c, "sdk"); return; }
    if (c->pc == MAIN_INIT) { initialize(c, "main"); return; }
    for (size_t i = 0; i < PROBE_MODULE_COUNT; ++i) {
        const ProbeModule *m = &probe_modules[i];
        uint64_t lo = 0, hi = *m->count;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (m->addresses[mid] < c->pc) lo = mid + 1; else hi = mid;
        }
        if (lo < *m->count && m->addresses[lo] == c->pc) {
            GuestFn f = m->functions[lo];
            __attribute__((musttail)) return f(c);
        }
    }
    stop("unresolved_indirect_branch", c->pc, 78);
}

static void query_memory(uint64_t address, uint64_t output) {
    struct Region { uint64_t base, size; uint32_t type, attr, perm, ipc_refs, device_refs, pad; } region = {0};
    uint64_t next = GUEST_HI;
    uint64_t shared_base, shared_size;
    if (runtime_kernel_memory(address, &shared_base, &shared_size)) {
        region.base = shared_base; region.size = shared_size; region.type = 6; region.perm = 3;
        memcpy((void *)(uintptr_t)output, &region, sizeof region); return;
    }
    if (shared_base < next) next = shared_base;
    for (size_t i = 0; i < PROBE_MODULE_COUNT + 3; ++i) {
        uint64_t base, size; uint32_t type;
        if (i < PROBE_MODULE_COUNT) { base = probe_modules[i].base; size = probe_modules[i].size; type = 4; }
        else if (i == PROBE_MODULE_COUNT) { base = TLS_BASE; size = 0x10000; type = 0xC; }
        else if (i == PROBE_MODULE_COUNT + 1) { base = MAIN_STACK; size = STACK_SIZE; type = 0xB; }
        else { base = HEAP_BASE; size = heap_size; type = 5; }
        if (address >= base && address < base + size) {
            region.base = base; region.size = size; region.type = type; region.perm = 3;
            memcpy((void *)(uintptr_t)output, &region, sizeof region); return;
        }
        if (base > address && base < next) next = base;
    }
    region.base = address & ~0xFFFull;
    region.size = next - region.base;
    memcpy((void *)(uintptr_t)output, &region, sizeof region);
}

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    current = c; InterlockedIncrement64(&svc_calls);
    uint64_t *x = c->x;
    if(trace_svc)fprintf(stderr, "[svc] %02x x0=%" PRIx64 " x1=%" PRIx64 " x2=%" PRIx64 " pc=%" PRIx64 "\n", imm, x[0], x[1], x[2], pc);
    if (runtime_kernel_svc(c, imm)) return;
    switch (imm) {
    case 0x01:
        if (x[1] > HEAP_MAX) stop("probe_heap_limit", pc, 79);
        heap_size = x[1];
        if (heap_size) commit(HEAP_BASE, heap_size);
        x[0] = 0; x[1] = HEAP_BASE; return;
    case 0x06: query_memory(x[2], x[0]); x[0] = x[1] = 0; return;
    case 0x07: stop("guest_exit_process", pc, 80);
    case 0x0C:  // GetThreadPriority: only the diagnostic main thread exists.
        if ((uint32_t)x[1] != 0x101 && (uint32_t)x[1] != 0xFFFF8000) { x[0] = 0xE401; return; }
        x[0] = 0; x[1] = main_priority; return;
    case 0x0D:
        if ((uint32_t)x[0] != 0x101 && (uint32_t)x[0] != 0xFFFF8000) { x[0] = 0xE401; return; }
        main_priority = (uint32_t)x[1]; x[0] = 0; return;
    case 0x10: x[0] = 0; return;
    case 0x1D:
        // SignalProcessWideKey has no waiters in this single-thread diagnostic.
        // WaitProcessWideKey and thread creation remain explicit unsupported stops.
        x[0] = 0; return;
    case 0x1F:
        fprintf(stderr, "[service] ConnectToNamedPort name=%.16s\n", (const char *)(uintptr_t)x[1]);
        x[1] = runtime_connect_port((const char *)(uintptr_t)x[1]);
        x[0] = x[1] ? 0 : 0xF201; return;
    case 0x21: x[0] = runtime_send_request(c, (uint32_t)x[0]); return;
    case 0x16: x[0] = x[0] >= 0x1000 ? runtime_kernel_close((uint32_t)x[0]) : runtime_close_handle((uint32_t)x[0]); return;
    case 0x1E: x[0] = rt_ticks(); return;
    case 0x25: x[0] = 0; x[1] = 1; return;  // one diagnostic main thread
    case 0x27:
        if (x[1] > 4096) stop("debug_string_too_large", pc, 81);
        fwrite((const void *)(uintptr_t)x[0], 1, (size_t)x[1], stderr);
        x[0] = 0; return;
    case 0x26:
        if (x[0] & 0x80000000u) { x[0] = 0; return; }
        if (x[2] == 4) fprintf(stderr, "[guest-abort] result=%08x\n", *(uint32_t *)(uintptr_t)x[1]);
        stop("guest_abort", pc, 92);
    case 0x29: {
        uint64_t value;
        switch ((uint32_t)x[1]) {
        case 0: value = 0xF; break;
        case 1: value = UINT64_MAX; break;
        case 2: value = 0x7100000000ull; break;
        case 3: value = 0x100000000ull; break;
        case 4: value = HEAP_BASE; break;
        case 5: value = HEAP_MAX; break;
        case 6: case 21: value = 0xCD500000ull; break;
        case 7: case 22: value = heap_size + 0x4000000; break;
        case 8: case 9: case 16: case 17: case 20: value = 0; break;
        case 11: value = rt_ticks(); break;
        case 12: value = GUEST_LO; break;
        case 13: value = GUEST_HI - GUEST_LO; break;
        case 14: value = STACK_BASE; break;
        case 15: value = STACK_REGION_SIZE; break;
        case 18: value = PROBE_TITLE_ID; break;
        default: stop("unimplemented_get_info", pc, 82);
        }
        x[0] = 0; x[1] = value; return;
    }
    default:
        // An explicit stop is evidence of the next runtime requirement.
        stop("unimplemented_svc", pc, 83);
    }
}

#ifdef PORT_GRAPHICS
static unsigned __stdcall debug_watch(void *argument){
    (void)argument;
    for(;;){Sleep(5000);fprintf(stderr,"[progress] main_fn=%" PRIx64 " functions=%" PRIu64 " svc_calls=%" PRIu64 "\n",(uint64_t)main_last_function,(uint64_t)calls,(uint64_t)svc_calls);}
    return 0;
}
static unsigned __stdcall game_main(void *argument) {
    Cpu *cpu = argument;
    main_cpu_snapshot=cpu;
    runtime_kernel_init(cpu, TLS_BASE);
    guest_call(cpu, PROBE_START);
    stop("sdk_start_returned", last_fn, 87);
}
#endif
int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);
    // Resolve local images beside the executable, including launches from Explorer.
    wchar_t executable_path[32768];
    DWORD length = GetModuleFileNameW(NULL, executable_path, 32768);
    if (!length || length >= 32768) stop("executable_path_unavailable", 0, 88);
    wchar_t *slash = wcsrchr(executable_path, L'\\');
    if (slash) { *slash = L'\0'; if (!SetCurrentDirectoryW(executable_path)) stop("image_directory_unavailable", 0, 89); }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    AddVectoredExceptionHandler(1, fault_handler);
    trace_svc=getenv("KIRBY_TRACE_SVC")!=NULL;
    function_budget=getenv("KIRBY_FUNCTION_BUDGET")?strtoull(getenv("KIRBY_FUNCTION_BUDGET"),NULL,10):0;
    fprintf(stderr, "[port] Native ARM64-recompiled Super Kirby Clash; experimental Windows runtime.\n");
    if (VirtualAlloc((void *)(uintptr_t)GUEST_LO, GUEST_HI - GUEST_LO, MEM_RESERVE, PAGE_NOACCESS) != (void *)(uintptr_t)GUEST_LO)
        stop("guest_address_space_unavailable", GUEST_LO, 84);
    for (size_t i = 0; i < PROBE_MODULE_COUNT; ++i) {
        const ProbeModule *m = &probe_modules[i];
        commit(m->base, m->size);
        FILE *f = fopen(m->image, "rb");
        if (!f) stop("module_image_missing", m->base, 85);
        size_t read = fread((void *)(uintptr_t)m->base, 1, (size_t)m->size, f);
        fclose(f);
        if (read != m->size) stop("module_image_truncated", m->base, 86);
        fprintf(stderr, "[load] %s base=%" PRIx64 " size=%" PRIx64 " functions=%" PRIu64 "\n", m->name, m->base, m->size, *m->count);
    }
    commit(MAIN_STACK, STACK_SIZE);
    commit(TLS_BASE, 0x100000);
    Cpu c = {0}; current = &c;
    c.sp = MAIN_STACK + STACK_SIZE; c.tpidrro = TLS_BASE; c.excl_addr = UINT64_MAX;
    c.x[0] = 0x101; c.x[1] = 0; c.x[2] = SDK_INIT; c.x[3] = MAIN_INIT;
    fprintf(stderr, "[start] nn::init::Start=%" PRIx64 "\n", (uint64_t)PROBE_START);
#ifdef PORT_GRAPHICS
    runtime_graphics_init();
    if(getenv("KIRBY_TRACE"))_beginthreadex(NULL,0,debug_watch,NULL,0,NULL);
    if (!_beginthreadex(NULL,64*1024*1024,game_main,&c,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL)) stop("main_thread_create_failed",0,93);
    return ui_run();
#else
    runtime_kernel_init(&c, TLS_BASE);
    guest_call(&c, PROBE_START);
    stop("sdk_start_returned", last_fn, 87);
#endif
}
