#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "cpu.h"

typedef struct {
    int window_width, window_height, render_width, render_height;
    int fps; // 0: original, 60 or 120
    bool fullscreen;
} KirbySettings;
extern KirbySettings kirby_settings;
void kirby_settings_load(void);
void kirby_settings_window(SDL_Window *parent);
void kirby_settings_pump(void);
bool kirby_settings_open(void);
int kirby_frame_hook(Cpu *cpu, uint64_t site);
