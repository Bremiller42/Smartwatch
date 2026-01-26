// FILE: watch_logstream.h
#pragma once
#include <stddef.h>
#include <stdint.h>

void   watch_logstream_init(void);
size_t watch_logstream_read(char *dst, size_t dst_sz);

uint32_t watch_logstream_dropped(void);
uint32_t watch_logstream_written(void);

/* NEW: increments when new bytes are queued */
uint32_t watch_logstream_seq(void);
