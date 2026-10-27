#pragma once
#include <stdint.h>
uint64_t runtime_romfs_size(void);
int runtime_romfs_read(uint64_t offset, uint64_t size, void *destination);
