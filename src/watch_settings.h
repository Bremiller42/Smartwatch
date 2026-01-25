#pragma once
#include <stdbool.h>
#include <stdint.h>

void settings_nvs_init(void);
void settings_load_from_nvs(void);

void settings_save_brightness(int bright);
void settings_save_24h(bool use24);
void settings_save_wifi(bool on);

void settings_save_wifi_creds(const char *ssid, const char *pass);
void settings_load_wifi_creds(void);

void settings_save_screen_timeout_s(uint32_t seconds);
void settings_save_ble(bool on);
void settings_save_hr_current(float bpm, bool valid);
void settings_load_hr_current_into_ui(void);
void settings_commit_dirty_now(void);
