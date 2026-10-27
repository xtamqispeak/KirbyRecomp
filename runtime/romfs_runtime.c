#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include "romfs_build.h"
#include "ipc_runtime.h"
#include "romfs_runtime.h"
#include "probe_modules.h"
static RomfsLayout layout;
static INIT_ONCE initialized = INIT_ONCE_STATIC_INIT;
static SRWLOCK lock = SRWLOCK_INIT;
static struct { int file; FILE *stream; uint64_t tick; } cache[32];
static uint64_t tick, reads;
static BOOL CALLBACK initialize(PINIT_ONCE once, PVOID arg, PVOID *context) {
    (void)once; (void)arg; (void)context;
    char why[256];
    if (!romfs_layout(PROBE_ROMFS_ROOT, &layout, why, sizeof why)) {
        fprintf(stderr, "[romfs-error] %s\n", why); runtime_stop("romfs_layout_failed", 0, 94);
    }
    fprintf(stderr, "[romfs] mapped %d files, virtual image %" PRIu64 " bytes from %s\n", layout.nfiles, layout.size, PROBE_ROMFS_ROOT);
    return TRUE;
}
uint64_t runtime_romfs_size(void) {
    InitOnceExecuteOnce(&initialized, initialize, NULL, NULL); return layout.size;
}
static void overlap_copy(uint64_t offset, uint64_t size, uint8_t *dest, uint64_t start, uint64_t length, const uint8_t *source) {
    if (offset >= start + length || offset + size <= start) return;
    uint64_t a = offset > start ? offset : start, b = offset + size < start + length ? offset + size : start + length;
    memcpy(dest + a - offset, source + a - start, (size_t)(b - a));
}
int runtime_romfs_read(uint64_t offset, uint64_t size, void *destination) {
    runtime_romfs_size();
    if (offset > layout.size || size > layout.size - offset) return 0;
    memset(destination, 0, (size_t)size);
    overlap_copy(offset, size, destination, 0, ROMFS_DATA_START, layout.header);
    overlap_copy(offset, size, destination, layout.tables_off, layout.tables_size, layout.tables);
    AcquireSRWLockExclusive(&lock);
    int lo = 0, hi = layout.nfiles;
    while (lo < hi) { int mid = lo + (hi - lo) / 2; if (layout.files[mid].start + layout.files[mid].size <= offset) lo = mid + 1; else hi = mid; }
    int ok = 1;
    for (int i = lo; i < layout.nfiles && layout.files[i].start < offset + size; ++i) {
        RomfsLayoutFile *file = &layout.files[i];
        uint64_t a = offset > file->start ? offset : file->start;
        uint64_t b = offset + size < file->start + file->size ? offset + size : file->start + file->size;
        if (b <= a) continue;
        unsigned slot = 0;
        for (unsigned k = 0; k < 32; ++k) {
            if (cache[k].stream && cache[k].file == i) { slot = k; break; }
            if (cache[k].tick < cache[slot].tick) slot = k;
        }
        if (!cache[slot].stream || cache[slot].file != i) {
            if (cache[slot].stream) fclose(cache[slot].stream);
            cache[slot].stream = fopen(file->path, "rb"); cache[slot].file = i;
            if (reads < 30) fprintf(stderr, "[asset] %s\n", file->path);
        }
        cache[slot].tick = ++tick;
        if (!cache[slot].stream || _fseeki64(cache[slot].stream, (__int64)(a - file->start), SEEK_SET) ||
            fread((uint8_t *)destination + a - offset, 1, (size_t)(b - a), cache[slot].stream) != b - a) { ok = 0; break; }
    }
    ++reads;
    ReleaseSRWLockExclusive(&lock);
    return ok;
}
