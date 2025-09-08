/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented.
  2. Altered source versions must be plainly marked as such.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifndef SDL_wiiuaudio_h_
#define SDL_wiiuaudio_h_

#include "../SDL_sysaudio.h"
#include <sndcore2/voice.h>

#define NUM_BUFFERS 2
#define SIZEOF_ARR(arr) (sizeof(arr) / sizeof(arr[0]))

/* Hidden private data for the Wii U audio device */
typedef struct SDL_PrivateAudioData {
    /* 6 possible voices for 6 channels */
    AXVoice* voice[6];
    /* Raw allocated mixing buffer */
    Uint8* rawbuf;
    /* Individual mixing buffers */
    Uint8* mixbufs[NUM_BUFFERS];
    /* Deinterleaving buffer */
    Uint8* deintvbuf;

    int renderingid;
    int playingid;
    uint32_t last_loopcount;
} SDL_PrivateAudioData;

#endif /* SDL_wiiuaudio_h_ */
