/*
 * sd_eloquence — speech-dispatcher output module for Eloquence V6.1
 *
 * Uses the ECI shim library (libeci.so) which transparently communicates
 * with the ARM Eloquence engine via the qemu-arm bridge.
 *
 * Based on the speech-dispatcher skeleton0 module pattern.
 */

#include "eci.h"
#include <spd_module_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>

/* ------------------------------------------------------------------ */
/* Configuration defaults (overridden by eloquence.conf)               */
/* ------------------------------------------------------------------ */

static int cfg_sample_rate   = 1;   /* 0=8k, 1=11k, 2=22k */
static int cfg_default_voice = 0;   /* 0-7 */
static int cfg_pitch         = 65;
static int cfg_speed         = 50;
static int cfg_volume        = 90;
static int cfg_dictionary    = 0;
static int cfg_textmode      = 1;   /* 0=phrase prediction on, 1=off */
static int cfg_debug         = 0;

/* ------------------------------------------------------------------ */
/* Runtime state                                                       */
/* ------------------------------------------------------------------ */

static ECIHand g_eci = NULL;
static volatile int g_stop = 0;

#define AUDIO_BUF_SIZE 8192
static short g_audio_buffer[AUDIO_BUF_SIZE];

static int g_sample_rate_hz = 11025;

/* Current voice mapped from SPD voice type */
static int g_current_voice = 0;

/* Current ECI params (as set by module_set) */
static int g_eci_speed  = 50;
static int g_eci_pitch  = 65;
static int g_eci_volume = 90;

/* ------------------------------------------------------------------ */
/* Voice table                                                         */
/* ------------------------------------------------------------------ */

static const char *voice_names[] = {
    "Reed", "Shelley", "Bobby", "Rocko",
    "Glen", "Sandy", "Grandma", "Grandpa"
};

static SPDVoice *g_voices[9];  /* 8 voices + NULL sentinel */

static int sample_rate_to_hz(int idx)
{
    switch (idx) {
    case 0: return 8000;
    case 2: return 22050;
    default: return 11025;
    }
}

/* Map SPD voice type to ECI voice preset */
static int spd_voice_to_eci(SPDVoiceType vt)
{
    switch (vt) {
    case SPD_MALE1:         return 0; /* Reed */
    case SPD_MALE2:         return 3; /* Rocko */
    case SPD_MALE3:         return 4; /* Glen */
    case SPD_FEMALE1:       return 1; /* Shelley */
    case SPD_FEMALE2:       return 5; /* Sandy */
    case SPD_FEMALE3:       return 6; /* Grandma */
    case SPD_CHILD_MALE:    return 2; /* Bobby */
    case SPD_CHILD_FEMALE:  return 2; /* Bobby (closest) */
    default:                return 0;
    }
}

/* Clamp integer to range */
static int clamp(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

/* ------------------------------------------------------------------ */
/* ECI callback                                                        */
/* ------------------------------------------------------------------ */

static enum ECICallbackReturn eci_callback(ECIHand h, enum ECIMessage msg,
                                           long lparam, void *data)
{
    (void)h;
    (void)data;

    if (msg == eciWaveformBuffer && !g_stop) {
        AudioTrack track = {
            .bits = 16,
            .num_channels = 1,
            .sample_rate = g_sample_rate_hz,
            .num_samples = (int)lparam,
            .samples = g_audio_buffer,
        };
        module_tts_output_server(&track, SPD_AUDIO_LE);
    }
    return g_stop ? eciDataAbort : eciDataProcessed;
}

/* ------------------------------------------------------------------ */
/* Config file parsing                                                 */
/* ------------------------------------------------------------------ */

static void parse_config_line(const char *line)
{
    char key[64], val[64];
    if (sscanf(line, "%63s %63s", key, val) != 2)
        return;
    if (key[0] == '#')
        return;

    if (!strcmp(key, "EloquenceSampleRate"))
        cfg_sample_rate = atoi(val);
    else if (!strcmp(key, "EloquenceDefaultVoice"))
        cfg_default_voice = clamp(atoi(val), 0, 7);
    else if (!strcmp(key, "EloquencePitchBaseline"))
        cfg_pitch = clamp(atoi(val), 0, 100);
    else if (!strcmp(key, "EloquenceSpeed"))
        cfg_speed = clamp(atoi(val), 0, 250);
    else if (!strcmp(key, "EloquenceVolume"))
        cfg_volume = clamp(atoi(val), 0, 100);
    else if (!strcmp(key, "EloquenceDictionary"))
        cfg_dictionary = atoi(val);
    else if (!strcmp(key, "EloquenceTextMode"))
        cfg_textmode = atoi(val);
    else if (!strcmp(key, "Debug"))
        cfg_debug = atoi(val);
}

int module_config(const char *configfile)
{
    if (!configfile)
        return 0;

    FILE *f = fopen(configfile, "r");
    if (!f)
        return 0;  /* Missing config is not fatal */

    char line[256];
    while (fgets(line, sizeof(line), f))
        parse_config_line(line);

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                    */
/* ------------------------------------------------------------------ */

int module_init(char **msg)
{
    g_eci = eciNew();
    if (!g_eci) {
        *msg = strdup("Failed to create ECI instance. "
                       "Has 'sudo eloquence-setup' been run?");
        return -1;
    }

    /* Apply config defaults */
    eciSetParam(g_eci, eciSampleRate, cfg_sample_rate);
    eciSetParam(g_eci, eciDictionary, cfg_dictionary);
    eciSetParam(g_eci, eciTextMode, cfg_textmode);
    g_sample_rate_hz = sample_rate_to_hz(cfg_sample_rate);

    g_current_voice = cfg_default_voice;
    eciCopyVoice(g_eci, g_current_voice, 0);

    g_eci_speed  = cfg_speed;
    g_eci_pitch  = cfg_pitch;
    g_eci_volume = cfg_volume;
    eciSetVoiceParam(g_eci, 0, eciSpeed, g_eci_speed);
    eciSetVoiceParam(g_eci, 0, eciPitchBaseline, g_eci_pitch);
    eciSetVoiceParam(g_eci, 0, eciVolume, g_eci_volume);

    /* Set up callback-based audio output */
    eciSetOutputBuffer(g_eci, AUDIO_BUF_SIZE, g_audio_buffer);
    eciRegisterCallback(g_eci, eci_callback, NULL);

    /* Tell speech-dispatcher we'll send audio via module_tts_output_server */
    module_audio_set_server();

    /* Build voice list */
    for (int i = 0; i < 8; i++) {
        g_voices[i] = malloc(sizeof(SPDVoice));
        g_voices[i]->name = strdup(voice_names[i]);
        g_voices[i]->language = strdup("en");
        g_voices[i]->variant = strdup(voice_names[i]);
    }
    g_voices[8] = NULL;

    *msg = strdup("Eloquence V6.1 initialized.");
    return 0;
}

SPDVoice **module_list_voices(void)
{
    return g_voices;
}

int module_loop(void)
{
    return module_process(STDIN_FILENO, 1);
}

int module_set(const char *var, const char *val)
{
    if (!var || !val)
        return -1;

    if (!strcmp(var, "rate")) {
        /* SPD rate: -100..+100 → ECI speed: 0..250 */
        int spd_rate = atoi(val);
        g_eci_speed = clamp(50 + spd_rate * 2, 0, 250);
        return 0;
    }
    if (!strcmp(var, "pitch")) {
        /* SPD pitch: -100..+100 → ECI pitch baseline: 0..100 */
        int spd_pitch = atoi(val);
        g_eci_pitch = clamp(50 + spd_pitch / 2, 0, 100);
        return 0;
    }
    if (!strcmp(var, "volume")) {
        /* SPD volume: -100..+100 → ECI volume: 0..100 */
        int spd_vol = atoi(val);
        g_eci_volume = clamp((spd_vol + 100) / 2, 0, 100);
        return 0;
    }
    if (!strcmp(var, "voice")) {
        /* SPD voice type string like "MALE1", "FEMALE1", etc. */
        SPDVoiceType vt = SPD_MALE1;
        if (!strcmp(val, "MALE1"))        vt = SPD_MALE1;
        else if (!strcmp(val, "MALE2"))   vt = SPD_MALE2;
        else if (!strcmp(val, "MALE3"))   vt = SPD_MALE3;
        else if (!strcmp(val, "FEMALE1")) vt = SPD_FEMALE1;
        else if (!strcmp(val, "FEMALE2")) vt = SPD_FEMALE2;
        else if (!strcmp(val, "FEMALE3")) vt = SPD_FEMALE3;
        else if (!strcmp(val, "CHILD_MALE"))   vt = SPD_CHILD_MALE;
        else if (!strcmp(val, "CHILD_FEMALE")) vt = SPD_CHILD_FEMALE;
        g_current_voice = spd_voice_to_eci(vt);
        return 0;
    }
    if (!strcmp(var, "synthesis_voice")) {
        /* Direct voice name like "Reed", "Shelley", etc. */
        for (int i = 0; i < 8; i++) {
            if (!strcasecmp(val, voice_names[i])) {
                g_current_voice = i;
                return 0;
            }
        }
        /* Try numeric */
        int v = atoi(val);
        if (v >= 0 && v < 8) {
            g_current_voice = v;
            return 0;
        }
        return -1;
    }
    if (!strcmp(var, "language")) {
        /* We only support English — accept but ignore */
        return 0;
    }
    if (!strcmp(var, "punctuation_mode")) {
        return 0;
    }
    if (!strcmp(var, "spelling_mode")) {
        return 0;
    }
    if (!strcmp(var, "cap_let_recogn")) {
        return 0;
    }

    /* Accept unknown parameters silently — returning -1 would cause
     * speech-dispatcher to abort the speak request entirely. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* SSML stripping                                                      */
/* ------------------------------------------------------------------ */

/* Strip XML/SSML tags from text. Returns malloc'd string. */
static char *strip_ssml(const char *ssml)
{
    size_t len = strlen(ssml);
    char *out = malloc(len + 1);
    if (!out) return NULL;

    size_t j = 0;
    int in_tag = 0;
    for (size_t i = 0; i < len; i++) {
        if (ssml[i] == '<') {
            in_tag = 1;
        } else if (ssml[i] == '>') {
            in_tag = 0;
        } else if (!in_tag) {
            out[j++] = ssml[i];
        }
    }
    out[j] = '\0';

    /* Decode common XML entities */
    char *p;
    while ((p = strstr(out, "&amp;")) != NULL) {
        *p = '&';
        memmove(p + 1, p + 5, strlen(p + 5) + 1);
    }
    while ((p = strstr(out, "&lt;")) != NULL) {
        *p = '<';
        memmove(p + 1, p + 4, strlen(p + 4) + 1);
    }
    while ((p = strstr(out, "&gt;")) != NULL) {
        *p = '>';
        memmove(p + 1, p + 4, strlen(p + 4) + 1);
    }
    while ((p = strstr(out, "&quot;")) != NULL) {
        *p = '"';
        memmove(p + 1, p + 6, strlen(p + 6) + 1);
    }
    while ((p = strstr(out, "&apos;")) != NULL) {
        *p = '\'';
        memmove(p + 1, p + 6, strlen(p + 6) + 1);
    }

    return out;
}

/* ------------------------------------------------------------------ */
/* Synthesis                                                           */
/* ------------------------------------------------------------------ */

void module_speak_sync(const char *data, size_t bytes, SPDMessageType msgtype)
{
    (void)bytes;

    g_stop = 0;
    module_speak_ok();

    if (!g_eci) {
        module_speak_error();
        return;
    }

    /* Clear any leftover state from a previous aborted synthesis */
    eciClearInput(g_eci);

    /* Apply current voice and parameters */
    eciCopyVoice(g_eci, g_current_voice, 0);
    eciSetVoiceParam(g_eci, 0, eciSpeed, g_eci_speed);
    eciSetVoiceParam(g_eci, 0, eciPitchBaseline, g_eci_pitch);
    eciSetVoiceParam(g_eci, 0, eciVolume, g_eci_volume);

    /* Speech-dispatcher sends SSML; ECI doesn't understand it, so strip tags */
    char *text = strip_ssml(data);
    if (!text) {
        module_report_event_end();
        return;
    }

    /* Skip empty text after stripping */
    const char *p = text;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p == '\0') {
        free(text);
        module_report_event_end();
        return;
    }

    eciAddText(g_eci, text);
    free(text);

    if (g_stop) {
        module_report_event_stop();
        return;
    }

    module_report_event_begin();

    eciSynthesize(g_eci);
    eciSynchronize(g_eci);

    if (g_stop)
        module_report_event_stop();
    else
        module_report_event_end();
}

int module_speak(char *data, size_t bytes, SPDMessageType msgtype)
{
    /* We use the synchronous interface; this shouldn't be called,
     * but provide a stub just in case. */
    (void)data;
    (void)bytes;
    (void)msgtype;
    return -1;
}

void module_speak_begin(void) {}
void module_speak_end(void) {}
void module_speak_pause(void) {}
void module_speak_stop(void) {}

/* ------------------------------------------------------------------ */
/* Stop / Pause                                                        */
/* ------------------------------------------------------------------ */

int module_stop(void)
{
    /* Only set the flag — do NOT call eciStop() here.
     * module_stop() is called from the main thread while
     * module_speak_sync() holds the RPC lock in eciSynchronize().
     * Calling eciStop() would deadlock on the same lock.
     * The callback checks g_stop and returns eciDataAbort,
     * which makes eciSynchronize() return promptly. */
    g_stop = 1;
    return 0;
}

size_t module_pause(void)
{
    g_stop = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

int module_close(void)
{
    if (g_eci) {
        eciDelete(g_eci);
        g_eci = NULL;
    }
    for (int i = 0; i < 8; i++) {
        if (g_voices[i]) {
            free(g_voices[i]->name);
            free(g_voices[i]->language);
            free(g_voices[i]->variant);
            free(g_voices[i]);
            g_voices[i] = NULL;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Optional hooks (using defaults from module_utils)                   */
/* ------------------------------------------------------------------ */

int module_audio_set(const char *var, const char *val)
{
    (void)var;
    (void)val;
    return 0;
}

int module_audio_init(char **status_info)
{
    (void)status_info;
    return 0;
}

int module_loglevel_set(const char *var, const char *val)
{
    (void)var;
    if (val)
        cfg_debug = atoi(val);
    return 0;
}

int module_debug(int enable, const char *file)
{
    (void)enable;
    (void)file;
    return 0;
}
