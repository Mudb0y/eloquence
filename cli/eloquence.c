/*
 * Eloquence CLI: text-to-speech using the ECI shim library.
 * Outputs WAV files from text input.
 */
#include "eci.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <unistd.h>

/* WAV header for 16-bit mono PCM */
typedef struct {
    char     riff[4];        /* "RIFF" */
    uint32_t file_size;      /* file size - 8 */
    char     wave[4];        /* "WAVE" */
    char     fmt[4];         /* "fmt " */
    uint32_t fmt_size;       /* 16 */
    uint16_t audio_fmt;      /* 1 = PCM */
    uint16_t channels;       /* 1 */
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample; /* 16 */
    char     data[4];        /* "data" */
    uint32_t data_size;
} wav_header_t;

/* PCM accumulation buffer */
static short *g_pcm_buf = NULL;
static int    g_pcm_len = 0;      /* samples written */
static int    g_pcm_cap = 0;      /* samples allocated */

#define CALLBACK_BUF_SIZE 8192     /* samples per callback buffer */
static short g_cb_buffer[CALLBACK_BUF_SIZE];

static enum ECICallbackReturn my_callback(ECIHand hEngine,
                                          enum ECIMessage msg,
                                          long lParam,
                                          void *pData)
{
    (void)hEngine;
    (void)pData;

    if (msg == eciWaveformBuffer) {
        int samples = (int)lParam;
        if (samples <= 0) return eciDataProcessed;

        /* Grow buffer if needed */
        while (g_pcm_len + samples > g_pcm_cap) {
            int newcap = g_pcm_cap ? g_pcm_cap * 2 : 65536;
            short *nb = (short *)realloc(g_pcm_buf, (size_t)newcap * sizeof(short));
            if (!nb) return eciDataAbort;
            g_pcm_buf = nb;
            g_pcm_cap = newcap;
        }
        memcpy(g_pcm_buf + g_pcm_len, g_cb_buffer, (size_t)samples * sizeof(short));
        g_pcm_len += samples;
    }
    return eciDataProcessed;
}

static int write_wav(const char *path, const short *pcm, int num_samples, int sample_rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }

    uint32_t data_size = (uint32_t)(num_samples * 2);
    wav_header_t hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.file_size = 36 + data_size;
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_fmt = 1;
    hdr.channels = 1;
    hdr.sample_rate = (uint32_t)sample_rate;
    hdr.byte_rate = (uint32_t)(sample_rate * 2);
    hdr.block_align = 2;
    hdr.bits_per_sample = 16;
    memcpy(hdr.data, "data", 4);
    hdr.data_size = data_size;

    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(pcm, sizeof(short), (size_t)num_samples, f);
    fclose(f);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -t, --text TEXT        Text to synthesize\n"
        "  -f, --file FILE        Read text from file\n"
        "  -o, --output FILE      Output WAV file (default: output.wav)\n"
        "  -v, --voice NUM        Voice preset 0-7 (default: 0)\n"
        "  -s, --speed NUM        Speed 0-250 (default: 50)\n"
        "  -p, --pitch NUM        Pitch baseline 0-100 (default: 65)\n"
        "      --rate NUM         Sample rate: 0=8kHz, 1=11kHz, 2=22kHz (default: 1)\n"
        "      --list-voices      Show available voices\n"
        "      --version          Show ECI version\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Reads from stdin if no -t or -f given.\n",
        prog);
}

static const char *voice_names[] = {
    "Reed", "Shelley", "Bobby", "Rocko",
    "Glen", "Sandy", "Grandma", "Grandpa"
};

int main(int argc, char *argv[])
{
    const char *text = NULL;
    const char *input_file = NULL;
    const char *output_file = "output.wav";
    int voice = -1;
    int speed = -1;
    int pitch = -1;
    int rate = -1;
    int list_voices = 0;
    int show_version = 0;

    static struct option long_opts[] = {
        {"text",        required_argument, 0, 't'},
        {"file",        required_argument, 0, 'f'},
        {"output",      required_argument, 0, 'o'},
        {"voice",       required_argument, 0, 'v'},
        {"speed",       required_argument, 0, 's'},
        {"pitch",       required_argument, 0, 'p'},
        {"rate",        required_argument, 0, 'R'},
        {"list-voices", no_argument,       0, 'L'},
        {"version",     no_argument,       0, 'V'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:f:o:v:s:p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't': text = optarg; break;
        case 'f': input_file = optarg; break;
        case 'o': output_file = optarg; break;
        case 'v': voice = atoi(optarg); break;
        case 's': speed = atoi(optarg); break;
        case 'p': pitch = atoi(optarg); break;
        case 'R': rate = atoi(optarg); break;
        case 'L': list_voices = 1; break;
        case 'V': show_version = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    /* Handle remaining args as text */
    if (!text && !input_file && optind < argc) {
        /* Join remaining args */
        static char joined[65536];
        joined[0] = '\0';
        for (int i = optind; i < argc; i++) {
            if (i > optind) strcat(joined, " ");
            strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 2);
        }
        text = joined;
    }

    if (show_version) {
        char ver[256] = {0};
        eciVersion(ver);
        printf("ECI version: %s\n", ver);
        return 0;
    }

    if (list_voices) {
        ECIHand h = eciNew();
        if (!h) { fprintf(stderr, "Failed to create ECI instance\n"); return 1; }
        printf("Available voices:\n");
        for (int i = 0; i < ECI_PRESET_VOICES; i++) {
            char name[ECI_VOICE_NAME_LENGTH + 1] = {0};
            eciGetVoiceName(h, i, name);
            int spd = eciGetVoiceParam(h, i, eciSpeed);
            int pit = eciGetVoiceParam(h, i, eciPitchBaseline);
            printf("  %d: %-12s (speed=%d, pitch=%d)\n", i,
                   name[0] ? name : voice_names[i], spd, pit);
        }
        eciDelete(h);
        return 0;
    }

    /* Read text from file or stdin if needed */
    char *file_text = NULL;
    if (!text) {
        FILE *fin = stdin;
        if (input_file) {
            fin = fopen(input_file, "r");
            if (!fin) { perror(input_file); return 1; }
        } else if (isatty(0) && !input_file) {
            fprintf(stderr, "Reading from stdin (Ctrl-D to end)...\n");
        }
        size_t cap = 0, len = 0;
        char buf[4096];
        while (fgets(buf, sizeof(buf), fin)) {
            size_t blen = strlen(buf);
            if (len + blen + 1 > cap) {
                cap = cap ? cap * 2 : 8192;
                if (cap < len + blen + 1) cap = len + blen + 1;
                file_text = (char *)realloc(file_text, cap);
            }
            memcpy(file_text + len, buf, blen);
            len += blen;
        }
        if (fin != stdin) fclose(fin);
        if (!file_text || len == 0) {
            fprintf(stderr, "No text provided\n");
            free(file_text);
            return 1;
        }
        file_text[len] = '\0';
        text = file_text;
    }

    /* Create ECI instance */
    ECIHand h = eciNew();
    if (!h) {
        fprintf(stderr, "Failed to create ECI instance\n");
        free(file_text);
        return 1;
    }

    /* Configure voice parameters */
    if (voice >= 0 && voice < ECI_PRESET_VOICES) {
        eciCopyVoice(h, voice, 0);
    }
    if (speed >= 0) eciSetVoiceParam(h, 0, eciSpeed, speed);
    if (pitch >= 0) eciSetVoiceParam(h, 0, eciPitchBaseline, pitch);
    if (rate >= 0) eciSetParam(h, eciSampleRate, rate);

    /* Get actual sample rate for WAV header */
    int sr_idx = eciGetParam(h, eciSampleRate);
    int sample_rate;
    switch (sr_idx) {
    case 0: sample_rate = 8000; break;
    case 2: sample_rate = 22050; break;
    default: sample_rate = 11025; break;
    }

    /* Set up callback mode for PCM capture */
    eciSetOutputBuffer(h, CALLBACK_BUF_SIZE, g_cb_buffer);
    eciRegisterCallback(h, my_callback, NULL);

    /* Add text and synthesize */
    if (!eciAddText(h, text)) {
        fprintf(stderr, "eciAddText failed\n");
        eciDelete(h);
        free(file_text);
        return 1;
    }

    if (!eciSynthesize(h)) {
        fprintf(stderr, "eciSynthesize failed\n");
        eciDelete(h);
        free(file_text);
        return 1;
    }

    if (!eciSynchronize(h)) {
        fprintf(stderr, "eciSynchronize failed\n");
        eciDelete(h);
        free(file_text);
        return 1;
    }

    /* Write WAV */
    if (g_pcm_len > 0) {
        if (write_wav(output_file, g_pcm_buf, g_pcm_len, sample_rate) == 0)
            fprintf(stderr, "Wrote %s: %d samples, %d Hz, %.1f sec\n",
                    output_file, g_pcm_len, sample_rate,
                    (double)g_pcm_len / sample_rate);
    } else {
        fprintf(stderr, "No audio data generated\n");
    }

    eciDelete(h);
    free(g_pcm_buf);
    free(file_text);
    return 0;
}
