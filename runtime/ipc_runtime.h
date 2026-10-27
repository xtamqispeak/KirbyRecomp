#pragma once
#include "cpu.h"
uint32_t runtime_connect_port(const char *name);
uint32_t runtime_send_request(Cpu *cpu, uint32_t handle);
uint32_t runtime_close_handle(uint32_t handle);
void runtime_stop(const char *reason, uint64_t pc, int code) __attribute__((noreturn));
