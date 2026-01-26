// FILE: watch_logbuf.h
#pragma once
#include <stddef.h>
#include <stdint.h>

void     watch_logbuf_init(void);
void     watch_logbuf_write(const char *s, size_t len);
size_t   watch_logbuf_snapshot(char *out, size_t out_sz);

/* NEW: monotonically increasing write sequence */
uint32_t watch_logbuf_seq(void);
