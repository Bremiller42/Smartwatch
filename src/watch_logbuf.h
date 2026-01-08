#pragma once
#include <stddef.h>
#include <stdint.h>
void watch_logbuf_init(void);

// Push raw formatted text into the ring buffer
void watch_logbuf_write(const char *s, size_t len);

// Copy newest logs into `out` (null-terminated). Returns bytes copied.
size_t watch_logbuf_snapshot(char *out, size_t out_sz);
uint32_t watch_logbuf_seq(void);
