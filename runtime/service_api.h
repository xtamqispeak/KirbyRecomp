#pragma once
#include <stddef.h>
#include "cpu.h"
typedef struct Buffer { union { uint64_t address, addr; }; uint64_t size; } Buffer;
typedef struct Request {
    union { uint32_t command, cmd; }; uint32_t type;
    uint8_t *data;
    union { size_t size, data_size; };
    Buffer x[8], a[8], b[8], c[8];
    unsigned nx, na, nb, nc;
    uint64_t pid;
    uint32_t copy_handles[8], move_handles[8];
    unsigned ncopy, nmove;
} Request;
typedef struct Service Service;
typedef struct Response {
    uint32_t result;
    uint8_t data[512];
    unsigned size;
    union { unsigned count, nobjects; };
    unsigned ncopy;
    union { uint32_t copy[8], copy_handles[8]; };
    Service *objects[8];
} Response;
typedef void (*ServiceHandler)(Service *, Request *, Response *);
struct Service {
    char name[32]; uint64_t state;
    volatile long references;
    void *userdata;
    ServiceHandler handler;
    void (*on_close)(Service *);
};
Service *runtime_service_new(const char *name, ServiceHandler handler, void *state);
void runtime_service_object(Response *response, Service *service);
