#pragma once
#include <stdint.h>
void runtime_input_rumble(uint32_t handle,const float value[4]);
void runtime_input_rumble_get(uint32_t handle,float value[4]);
void runtime_input_clear(void);
