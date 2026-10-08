/*
 * audio_sdl.c -- SDL3 audio device pulling the emulator's samples. SDL routes
 * through PipeWire/Pulse on Linux and ports unchanged to the Mac/Windows
 * frontends. Only the audio subsystem is initialized here -- never SDL video,
 * which would fight the GTK/Qt display stack.
 *
 * The device runs at A7800SESSION_AUDIO_RATE, interleaved stereo float:
 * MAME mixes TIA and POKEY to this rate and core/mame/MameHost resamples
 * and converts to float as it pulls, so the callback does no conversion of
 * its own. Modelled on the ColecoVision port's audio_sdl.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "session_internal.h"

typedef struct {
    SDL_AudioStream *stream;
} audio_state;

static void audio_cb(void *ud, SDL_AudioStream *stream, int additional_amount,
                     int total_amount)
{
    struct a7800session *s = ud;
    float buf[1024 * 2];
    (void)total_amount;

    while (additional_amount > 0) {
        int want_bytes = additional_amount > (int)sizeof(buf)
                             ? (int)sizeof(buf) : additional_amount;
        int want_frames = want_bytes / (int)(2 * sizeof(float));
        if (want_frames <= 0) break;
        /* render_audio zero-fills any shortfall itself: an underrun plays as
         * a gap, never as a buzz from stale buffer contents. */
        a7800session_render_audio(s, buf, want_frames);
        SDL_PutAudioStreamData(stream, buf, want_frames * (int)(2 * sizeof(float)));
        additional_amount -= want_frames * (int)(2 * sizeof(float));
    }
}

int audio_start(struct a7800session *s)
{
    audio_state *a;
    SDL_AudioSpec spec;

    if (s->audio)
        return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        session_set_error(s, "SDL audio init failed: %s", SDL_GetError());
        return -1;
    }

    a = calloc(1, sizeof *a);
    if (!a)
        return -1;

    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = A7800SESSION_AUDIO_RATE;
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_cb, s);
    if (!a->stream) {
        session_set_error(s, "SDL audio device open failed: %s", SDL_GetError());
        free(a);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }
    SDL_ResumeAudioStreamDevice(a->stream);
    s->audio = a;
    return 0;
}

void audio_stop(struct a7800session *s)
{
    audio_state *a = s->audio;
    if (!a)
        return;
    s->audio = NULL;
    SDL_DestroyAudioStream(a->stream);  /* also closes the bound device */
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
