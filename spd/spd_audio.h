/*
 * Compat header: spd_module_main.h includes <spd_audio.h> but
 * speech-dispatcher 0.12+ only ships spd_audio_plugin.h.
 */
#ifndef _SPD_AUDIO_H
#define _SPD_AUDIO_H
#include <spd_audio_plugin.h>
#endif
