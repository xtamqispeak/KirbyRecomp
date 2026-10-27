#pragma once
#include "cpu.h"
void runtime_kernel_init(Cpu *main_cpu, uint64_t tls_base);
int runtime_kernel_svc(Cpu *cpu, uint32_t svc);
uint32_t runtime_event_create(int signaled);
void runtime_event_set(uint32_t handle, int signaled);
uint32_t runtime_shared_create(uint64_t size);
void runtime_shared_callback(uint32_t handle, void (*on_map)(uint64_t));
uint32_t runtime_kernel_close(uint32_t handle);
uint32_t runtime_kernel_duplicate(uint32_t handle);
int runtime_kernel_memory(uint64_t address, uint64_t *base, uint64_t *size);
