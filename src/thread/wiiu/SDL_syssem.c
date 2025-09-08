/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_THREAD_WIIU

#include "SDL_timer.h"
#include "SDL_thread.h"
#include "SDL_systhread_c.h"

#include <coreinit/semaphore.h>
#include <coreinit/condition.h>
#include <coreinit/alarm.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>

#include <stdbool.h>

typedef struct
{
    OSCondition *cond;
    bool timed_out;
} WIIU_SemWaitTimeoutData;

struct SDL_semaphore
{
    OSMutex mtx;
    OSSemaphore sem;
    OSCondition cond;
};

SDL_sem *
SDL_CreateSemaphore(Uint32 initial_value)
{
    SDL_sem *sem = (SDL_sem *)SDL_malloc(sizeof(*sem));
    if (!sem) {
        SDL_OutOfMemory();
        return NULL;
    }

    OSInitSemaphore(&sem->sem, initial_value);
    OSInitMutex(&sem->mtx);
    OSInitCond(&sem->cond);

    return sem;
}

void
SDL_DestroySemaphore(SDL_sem *sem)
{
    if (sem) {
        SDL_free(sem);
    }
}

int
SDL_SemTryWait(SDL_sem *sem)
{
    if (!sem) {
        return SDL_InvalidParamError("sem");
    }
    return (OSTryWaitSemaphore(&sem->sem) > 0) ? 0 : SDL_MUTEX_TIMEDOUT;
}

static void
SDL_SemWaitTimeoutCallback(OSAlarm *alarm, OSContext *context)
{
    WIIU_SemWaitTimeoutData *data = (WIIU_SemWaitTimeoutData *)OSGetAlarmUserData(alarm);
    data->timed_out = true;
    OSSignalCond(data->cond);
}

int
SDL_SemWaitTimeout(SDL_sem *sem, Uint32 ms)
{
    if (!sem) {
        return SDL_InvalidParamError("sem");
    }

    /* zero timeout = try once */
    if (ms == 0) {
        return SDL_SemTryWait(sem);
    }

    WIIU_SemWaitTimeoutData data;
    OSAlarm alarm;

    OSLockMutex(&sem->mtx);

    /* setup callback data */
    data.timed_out = false;
    data.cond = &sem->cond;

    /* set an alarm */
    OSCreateAlarm(&alarm);
    OSSetAlarmUserData(&alarm, &data);
    OSSetAlarm(&alarm, OSMillisecondsToTicks(ms), &SDL_SemWaitTimeoutCallback);

    /* try until we succeed or timeout */
    while ((OSTryWaitSemaphore(&sem->sem) <= 0) && !data.timed_out) {
        OSWaitCond(&sem->cond, &sem->mtx);
    }

    OSCancelAlarm(&alarm);
    OSUnlockMutex(&sem->mtx);

    return data.timed_out ? SDL_MUTEX_TIMEDOUT : 0;
}

int
SDL_SemWait(SDL_sem *sem)
{
    if (!sem) {
        return SDL_InvalidParamError("sem");
    }
    OSWaitSemaphore(&sem->sem);
    return 0;
}

Uint32
SDL_SemValue(SDL_sem *sem)
{
    if (!sem) {
        SDL_InvalidParamError("sem");
        return 0;
    }
    return OSGetSemaphoreCount(&sem->sem);
}

int
SDL_SemPost(SDL_sem *sem)
{
    if (!sem) {
        return SDL_InvalidParamError("sem");
    }
    OSSignalSemaphore(&sem->sem);
    OSSignalCond(&sem->cond);
    return 0;
}

#endif /* SDL_THREAD_WIIU */

/* vi: set ts=4 sw=4 expandtab: */
