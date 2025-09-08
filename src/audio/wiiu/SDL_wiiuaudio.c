/*
  Simple DirectMedia Layer
  Copyright (C) 2018-2018 Ash Logan <ash@heyquark.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute
  it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented.
  2. Altered source versions must be plainly marked as such.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_WIIU

#include <stdio.h>
#include <malloc.h>

#include "SDL3/SDL_audio.h"
#include "SDL3/SDL_error.h"
#include "SDL3/SDL_timer.h"
#include "../SDL_audio_c.h"
#include "../SDL_audiodev_c.h"
#include "../SDL_sysaudio.h"
#include "SDL_wiiuaudio.h"

#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <sndcore2/drcvs.h>
#include <coreinit/core.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/memorymap.h>

#define WIIUAUDIO_DRIVER_NAME "wiiu"
#define AX_MAIN_AFFINITY OS_THREAD_ATTRIB_AFFINITY_CPU1
#define NUM_BUFFERS 2
#define WIIU_MAX_VALID_CHANNELS 2

static Uint8 mono_mix[2][6] = {0};   // dummy mixes, adjust if needed
static Uint8 stereo_mix[2][6] = {0};

static void _WIIUAUDIO_framecallback(void);
static SDL_AudioDevice* cb_this;
#define cb_hidden cb_this->hidden

/* helpers for AX-related math */
#define calc_ax_offset(offs, addr) (((void*)addr - offs.data) / sizeof_sample(offs))
#define sizeof_sample(offs) (offs.dataType == AX_VOICE_FORMAT_LPCM8 ? 1 : 2)
#define next_id(id) ((id + 1) % NUM_BUFFERS)

/* Open the device */
static int WIIUAUDIO_OpenDevice(SDL_AudioDevice *this, const char* devname)
{
    int ret = 0;
    AXVoiceOffsets offs;
    AXVoiceVeData vol = { .volume = 0x8000 };
    uint32_t old_affinity;
    float srcratio;
    Uint8* mixbuf = NULL;
    uint32_t mixbuf_allocation_count = 0;
    Uint8* mixbuf_allocations[32];

    this->hidden = (struct SDL_PrivateAudioData*)SDL_malloc(sizeof(*this->hidden));
    if (!this->hidden) return SDL_OutOfMemory();
    SDL_zerop(this->hidden);

    /* Keep thread affinity */
    old_affinity = OSGetThreadAffinity(OSGetCurrentThread());
    OSSetThreadAffinity(OSGetCurrentThread(), AX_MAIN_AFFINITY);

    if (!AXIsInit()) {
        AXInitParams initparams = {
            .renderer = AX_INIT_RENDERER_48KHZ,
            .pipeline = AX_INIT_PIPELINE_SINGLE,
        };
        AXInitWithParams(&initparams);
    }

    if (this->spec.channels < 1) this->spec.channels = 1;
    if (this->spec.channels > WIIU_MAX_VALID_CHANNELS)
        this->spec.channels = WIIU_MAX_VALID_CHANNELS;

    switch (SDL_AUDIO_BITSIZE(this->spec.format)) {
        case 8: break;
        case 16:
        default:
            this->spec.format = AUDIO_S16MSB;
            break;
    }

    if (SDL_AudioSpecGetSamples(&this->spec) < AXGetInputSamplesPerFrame())
        SDL_AudioSpecSetSamples(&this->spec, AXGetInputSamplesPerFrame());

    SDL_CalculateAudioSpec(&this->spec);

    /* Allocate buffers */
    for (int i = 0; i < 32; i++) {
        Uint32 physStart, physEnd;
        mixbuf = memalign(0x40, SDL_AudioSpecGetBufferSize(&this->spec) * NUM_BUFFERS);
        if (!mixbuf) break;
        physStart = OSEffectiveToPhysical((uint32_t) mixbuf) & 0x1fffffff;
        physEnd = physStart + SDL_AudioSpecGetBufferSize(&this->spec) * NUM_BUFFERS;
        if ((physEnd & 0xe0000000) == 0) break;
        mixbuf_allocations[mixbuf_allocation_count++] = mixbuf;
        mixbuf = NULL;
    }

    while (mixbuf_allocation_count--) SDL_free(mixbuf_allocations[mixbuf_allocation_count]);

    if (!mixbuf) { ret = SDL_OutOfMemory(); goto end; }

    SDL_memset(mixbuf, 0, SDL_AudioSpecGetBufferSize(&this->spec) * NUM_BUFFERS);
    DCStoreRange(mixbuf, SDL_AudioSpecGetBufferSize(&this->spec) * NUM_BUFFERS);

    for (int i = 0; i < NUM_BUFFERS; i++)
        this->hidden->mixbufs[i] = mixbuf + SDL_AudioSpecGetBufferSize(&this->spec) * i;

    this->hidden->deintvbuf = SDL_malloc(SDL_AudioSpecGetBufferSize(&this->spec));
    if (!this->hidden->deintvbuf) { AXQuit(); ret = SDL_OutOfMemory(); goto end; }

    for (int i = 0; i < this->spec.channels; i++) {
        this->hidden->voice[i] = AXAcquireVoice(31, NULL, NULL);
        if (!this->hidden->voice[i]) { AXQuit(); ret = SDL_OutOfMemory(); goto end; }

        AXVoiceBegin(this->hidden->voice[i]);
        AXSetVoiceType(this->hidden->voice[i], 0);
        AXSetVoiceVe(this->hidden->voice[i], &vol);

        switch (this->spec.channels) {
            case 1:
                AXSetVoiceDeviceMix(this->hidden->voice[i], AX_DEVICE_TYPE_DRC, 0, mono_mix[i]);
                AXSetVoiceDeviceMix(this->hidden->voice[i], AX_DEVICE_TYPE_TV, 0, mono_mix[i]);
                break;
            case 2:
                AXSetVoiceDeviceMix(this->hidden->voice[i], AX_DEVICE_TYPE_DRC, 0, stereo_mix[i]);
                AXSetVoiceDeviceMix(this->hidden->voice[i], AX_DEVICE_TYPE_TV, 0, stereo_mix[i]);
                break;
        }

        srcratio = (float)this->spec.freq / (float)AXGetInputSamplesPerSec();
        AXSetVoiceSrcRatio(this->hidden->voice[i], srcratio);
        AXSetVoiceSrcType(this->hidden->voice[i], AX_VOICE_SRC_TYPE_LINEAR);

        offs.loopingEnabled = AX_VOICE_LOOP_ENABLED;
        offs.currentOffset = 0;
        offs.loopOffset = 0;
        offs.endOffset = SDL_AudioSpecGetSamples(&this->spec);
        offs.dataType = (SDL_AUDIO_BITSIZE(this->spec.format) == 8) ? AX_VOICE_FORMAT_LPCM8 : AX_VOICE_FORMAT_LPCM16;
        offs.data = this->hidden->mixbufs[0] + SDL_AudioSpecGetSamples(&this->spec) * i * ((offs.dataType == AX_VOICE_FORMAT_LPCM8) ? 1 : 2);

        AXSetVoiceOffsets(this->hidden->voice[i], &offs);
        this->hidden->last_loopcount = AXGetVoiceLoopCount(this->hidden->voice[i]);
        this->hidden->playingid = 0;
        this->hidden->renderingid = 1;
        AXSetVoiceState(this->hidden->voice[i], AX_VOICE_STATE_PLAYING);
        AXVoiceEnd(this->hidden->voice[i]);
    }

    cb_this = this;
    AXRegisterAppFrameCallback(_WIIUAUDIO_framecallback);

end:
    OSSetThreadAffinity(OSGetCurrentThread(), old_affinity);
    return ret;
}

/* Frame callback */
static void _WIIUAUDIO_framecallback(void)
{
    int playing_buffer = -1;
    AXVoiceOffsets offs[6];
    void *endaddr;

    for (int i = 0; i < cb_this->spec.channels; i++)
        AXGetVoiceOffsets(cb_hidden->voice[i], &offs[i]);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        void* buf = cb_hidden->mixbufs[i];
        uint32_t startOffset = calc_ax_offset(offs[0], buf);
        uint32_t endOffset = startOffset + SDL_AudioSpecGetSamples(&cb_this->spec);
        if (offs[0].currentOffset >= startOffset && offs[0].currentOffset <= endOffset) {
            playing_buffer = i; break;
        }
    }

    if (playing_buffer < 0 || playing_buffer >= NUM_BUFFERS) playing_buffer = 0;
    cb_hidden->playingid = playing_buffer;

    for (int i = 0; i < cb_this->spec.channels; i++) {
        endaddr = cb_hidden->mixbufs[cb_hidden->playingid] + SDL_AudioSpecGetSamples(&cb_this->spec) * sizeof_sample(offs[i]) * (i + 1);
        endaddr -= 2;
        AXSetVoiceEndOffset(cb_hidden->voice[i], calc_ax_offset(offs[i], endaddr));

        void* loopaddr = (cb_hidden->renderingid != next_id(cb_hidden->playingid)) ?
            cb_hidden->mixbufs[next_id(cb_hidden->playingid)] + SDL_AudioSpecGetSamples(&cb_this->spec) * sizeof_sample(offs[i]) * i :
            cb_hidden->mixbufs[cb_hidden->playingid] + SDL_AudioSpecGetSamples(&cb_this->spec) * sizeof_sample(offs[i]) * i;
        AXSetVoiceLoopOffset(cb_hidden->voice[i], calc_ax_offset(offs[i], loopaddr));
    }
}

/* Play audio */
static void WIIUAUDIO_PlayDevice(SDL_AudioDevice *this)
{
    switch (SDL_AUDIO_BITSIZE(this->spec.format)) {
        case 8: {
            Uint8 *samples = (Uint8*)this->hidden->mixbufs[this->hidden->renderingid];
            Uint8 *deintv = (Uint8*)this->hidden->deintvbuf;
            for (int ch = 0; ch < this->spec.channels; ch++)
                for (int i = 0; i < SDL_AudioSpecGetSamples(&this->spec); i++)
                    deintv[SDL_AudioSpecGetSamples(&this->spec) * ch + i] = samples[i * this->spec.channels + ch];
        } break;
        case 16: {
            Uint16 *samples = (Uint16*)this->hidden->mixbufs[this->hidden->renderingid];
            Uint16 *deintv = (Uint16*)this->hidden->deintvbuf;
            for (int ch = 0; ch < this->spec.channels; ch++)
                for (int i = 0; i < SDL_AudioSpecGetSamples(&this->spec); i++)
                    deintv[SDL_AudioSpecGetSamples(&this->spec) * ch + i] = samples[i * this->spec.channels + ch];
        } break;
        default: break;
    }

    SDL_memcpy(this->hidden->mixbufs[this->hidden->renderingid], this->hidden->deintvbuf, SDL_AudioSpecGetBufferSize(&this->spec));
    DCStoreRange(this->hidden->mixbufs[this->hidden->renderingid], SDL_AudioSpecGetBufferSize(&this->spec));
    this->hidden->renderingid = next_id(this->hidden->renderingid);
}

/* Wait for device */
static void WIIUAUDIO_WaitDevice(SDL_AudioDevice *this)
{
    while (SDL_AtomicGet(&this->running) && this->hidden->renderingid == this->hidden->playingid)
        OSSleepTicks(OSMillisecondsToTicks(3));
}

/* Get device buffer */
static Uint8* WIIUAUDIO_GetDeviceBuf(SDL_AudioDevice *this, int *size)
{
    if (size) *size = SDL_AudioSpecGetBufferSize(&this->spec);
    return this->hidden->mixbufs[this->hidden->renderingid];
}

/* Close device */
static void WIIUAUDIO_CloseDevice(SDL_AudioDevice *this)
{
    if (AXIsInit()) {
        AXDeregisterAppFrameCallback(_WIIUAUDIO_framecallback);
        for (int i = 0; i < SIZEOF_ARR(this->hidden->voice); i++) {
            if (this->hidden->voice[i]) {
                AXFreeVoice(this->hidden->voice[i]);
                this->hidden->voice[i] = NULL;
            }
        }
        AXQuit();
    }
    if (this->hidden->mixbufs[0]) free(this->hidden->mixbufs[0]);
    if (this->hidden->deintvbuf) SDL_free(this->hidden->deintvbuf);
    SDL_free(this->hidden);
}

/* Thread init */
static void WIIUAUDIO_ThreadInit(SDL_AudioDevice *this)
{
    OSThread *currentThread = OSGetCurrentThread();
    int32_t priority = OSGetThreadPriority(currentThread) - 1;
    OSSetThreadPriority(currentThread, priority);
}

/* Driver init */
static SDL_bool WIIUAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = WIIUAUDIO_OpenDevice;
    impl->PlayDevice = WIIUAUDIO_PlayDevice;
    impl->WaitDevice = WIIUAUDIO_WaitDevice;
    impl->GetDeviceBuf = WIIUAUDIO_GetDeviceBuf;
    impl->CloseDevice = WIIUAUDIO_CloseDevice;
    impl->ThreadInit = WIIUAUDIO_ThreadInit;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    return SDL_TRUE;
}

/* Bootstrap */
AudioBootStrap WIIUAUDIO_bootstrap = {
    WIIUAUDIO_DRIVER_NAME, "Wii U AX Audio Driver", WIIUAUDIO_Init, 0,
};

#endif // SDL_AUDIO_DRIVER_WIIU
