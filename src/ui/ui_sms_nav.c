// FILE: src/ui/ui_sms_nav.c
#include "ui_priv.h"
#include <string.h>

static char s_active_thread_id[192];
static char s_active_thread_name[96];

void ui_sms_set_active_thread(const char *thread_id, const char *name)
{
    if (!thread_id) thread_id = "";
    if (!name) name = "";
    strncpy(s_active_thread_id, thread_id, sizeof(s_active_thread_id)-1);
    s_active_thread_id[sizeof(s_active_thread_id)-1] = 0;

    strncpy(s_active_thread_name, name, sizeof(s_active_thread_name)-1);
    s_active_thread_name[sizeof(s_active_thread_name)-1] = 0;
}

const char *ui_sms_get_active_thread_id(void)   { return s_active_thread_id; }
const char *ui_sms_get_active_thread_name(void) { return s_active_thread_name; }
