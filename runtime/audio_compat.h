// Adapter for the vendored LASRecomp/Ryujinx audio mixer.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "service_api.h"
#include "kernel_runtime.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef Request IpcReq;
typedef Response IpcResp;
typedef Buffer IpcBuf;
typedef struct KEvent { uint32_t handle; } KEvent;
extern pthread_mutex_t g_klock;
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
void kernel_signal(KEvent *event);
typedef struct AudioSettings { float volume; bool reverb; } AudioSettings;
extern AudioSettings audio_settings;
#define g_settings audio_settings
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define SERVICE(name, fn) void port_##fn(Service *s, IpcReq *req, IpcResp *resp)
#define RT_ON_RESET(fn) static void fn(void)
#define IN(type, offset) (*(const type *)(req->data + (offset)))
#define OUT(type, offset) (*(type *)(resp->data + (offset)))
static inline void resp_size(Response *resp, unsigned size) { if (size > resp->size) resp->size = size; }
static inline Buffer *in_buf(Request *req, unsigned i) {
    if (i < req->na && req->a[i].size) return &req->a[i];
    return i < req->nx ? &req->x[i] : i < req->na ? &req->a[i] : NULL;
}
static inline Buffer *out_buf(Request *req, unsigned i) {
    if (i < req->nb && req->b[i].size) return &req->b[i];
    return i < req->nc ? &req->c[i] : i < req->nb ? &req->b[i] : NULL;
}
