#pragma once
#include <stdbool.h>
#include <stdint.h>

void  volume_init_from_settings(void);          // call once at boot after settings load
int   volume_get_user_pct(void);                // 0..100 (or 10..100 if you prefer)
void  volume_set_user_pct(int pct);             // applies to audio immediately

bool  volume_get_muted(void);
void  volume_set_muted(bool muted);             // applies immediately

float volume_pct_to_gain(int pct);              // exposed for UI/debug if you want