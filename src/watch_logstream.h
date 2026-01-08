// FILE: src/watch_logstream.h
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void watch_logstream_init(void);

// Returns number of bytes drained into dst (0 if none)
size_t watch_logstream_read(char *dst, size_t dst_sz);

// Optional stats
uint32_t watch_logstream_dropped(void);
uint32_t watch_logstream_written(void);

#ifdef __cplusplus
}
#endif
