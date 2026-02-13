#include "watch_volume.h"
#include "watch_audio.h"
#include "watch_settings.h"   // you’ll add settings_get/save stubs if not present
#include <math.h>

static int  s_vol_pct = 30;        // default
static bool s_muted   = false;
static int  s_last_unmuted_pct = 30;

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Human-friendly curve:
// - pct=0 => gain=0
// - pct=100 => gain=1
// - mid range feels nicer than linear (gamma-ish)
float volume_pct_to_gain(int pct)
{
    pct = clampi(pct, 0, 100);
    if (pct == 0) return 0.0f;

    float x = (float)pct / 100.0f;
    // tweak exponent to taste: 2.0 = softer lows, louder highs
    float gain = powf(x, 2.0f);

    // If you want to allow “boost” above 100 later, do it here.
    // For now keep it sane:
    if (gain > 1.0f) gain = 1.0f;
    return gain;
}

static void apply_gain(void)
{
    float g = s_muted ? 0.0f : volume_pct_to_gain(s_vol_pct);
    watch_audio_set_gain(g);
}

void volume_init_from_settings(void)
{
    // If you already have settings getters, use them.
    // Otherwise, just keep defaults until you add persistence.
    int saved = settings_get_volume_pct(s_vol_pct);    // you implement
    bool m    = settings_get_volume_muted(false);      // you implement

    s_vol_pct = clampi(saved, 0, 100);
    s_muted   = m;

    if (!s_muted) s_last_unmuted_pct = s_vol_pct;

    apply_gain();
}

int volume_get_user_pct(void) { return s_vol_pct; }

void volume_set_user_pct(int pct)
{
    pct = clampi(pct, 0, 100);
    s_vol_pct = pct;

    // if user moves slider while muted, you can either:
    // A) keep muted but remember new pct, or
    // B) auto-unmute. I like A.
    if (!s_muted) s_last_unmuted_pct = s_vol_pct;

    apply_gain();
}

bool volume_get_muted(void) { return s_muted; }

void volume_set_muted(bool muted)
{
    if (s_muted == muted) return;
    s_muted = muted;

    if (!s_muted) {
        // restore last unmuted volume if you want:
        s_vol_pct = clampi(s_last_unmuted_pct, 0, 100);
    }

    apply_gain();
}