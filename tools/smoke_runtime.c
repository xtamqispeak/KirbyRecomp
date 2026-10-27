// Synthetic-program verification only. No Switch or game runtime is implemented here.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

extern const uint64_t main_fn_addrs[], main_fn_count;
extern const GuestFn main_fn_ptrs[];
volatile int g_fnprof = 0;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
void rt_dispatch(Cpu *c) {
    for (uint64_t i = 0; i < main_fn_count; ++i) {
        if (main_fn_addrs[i] == c->pc) { main_fn_ptrs[i](c); return; }
    }
    fprintf(stderr, "Unresolved synthetic branch: %" PRIx64 "\n", c->pc);
    abort();
}
void rt_udf(Cpu *c, uint64_t pc) { (void)c; fprintf(stderr, "Trap at %" PRIx64 "\n", pc); abort(); }
void rt_brk(Cpu *c, uint32_t n, uint64_t pc) { (void)n; rt_udf(c, pc); }
void rt_svc(Cpu *c, uint32_t n, uint64_t pc) { (void)n; rt_udf(c, pc); }
int rt_hook(Cpu *c, uint64_t pc) { rt_udf(c, pc); }
uint64_t rt_ticks(void) { abort(); }

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    Cpu c = {0};
    uint64_t cell = strtoull(argv[3], NULL, 0);
    c.x[0] = strtoull(argv[1], NULL, 0);
    c.x[1] = strtoull(argv[2], NULL, 0);
    c.x[3] = (uint64_t)(uintptr_t)&cell;
    const float a = 1.25f, b = 2.5f;
    memcpy(&c.v[0], &a, sizeof a);
    memcpy(&c.v[1], &b, sizeof b);
    main_fn_ptrs[0](&c);
    uint32_t fp;
    memcpy(&fp, &c.v[0], sizeof fp);
    printf("%" PRIu64 " %" PRIu64 " %" PRIu32 " %" PRIu64 "\n", c.x[0], cell, fp, c.nzcv);
    return 0;
}
