#include "audio_compat.h"
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
AudioSettings audio_settings = {1.0f, true};
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable) {
    KEvent *event = calloc(1, sizeof *event);
    event->handle = runtime_event_create(0);
    *writable = *readable = event->handle;
    return event;
}
void kernel_signal(KEvent *event) { runtime_event_set(event->handle, 1); }
