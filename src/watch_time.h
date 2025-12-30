#pragma once
#include <stdbool.h>

void time_set_timezone(void);

void sntp_start(void);

void time_save_last_known(void);
void time_restore_last_known(void);

void set_system_time_hm(int hour, int minute);
