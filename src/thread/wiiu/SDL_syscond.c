/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.
*/

#include "../../SDL_internal.h"

#if SDL_THREAD_WIIU

/* Use SDL3 thread header path */
#include "SDL3/SDL_thread.h"

#include <stdbool.h>
#include <coreinit/alarm.h>
#include <coreinit/mutex.h>
#include <coreinit/condition.h>

/* Helper struct used to signal a timeout via an alarm callback */
typedef struct
{
   OSCondition *cond;
   bool timed_out;
} WIIU_CondWaitTimeoutData;

/* Create a condition variable */
SDL_cond *
SDL_CreateCond(void)
{
    OSCondition *cond;

    cond = (OSCondition *) SDL_malloc(sizeof(OSCondition));
    if (cond) {
        OSInitCond(cond);
    } else {
        (void) SDL_OutOfMemory();
    }
    return (SDL_cond *)cond;
}

/* Destroy a condition variable */
void
SDL_DestroyCond(SDL_cond * cond)
{
    if (cond) {
        SDL_free(cond);
    }
}

/* Signal one waiter (SDL_CondSignal maps to broadcast on this platform) */
int
SDL_CondSignal(SDL_cond * cond)
{
    return SDL_CondBroadcast(cond);
}

/* Broadcast to all waiters */
int
SDL_CondBroadcast(SDL_cond * cond)
{
    if (!cond) {
        return SDL_SetError("Passed a NULL condition variable");
    }

    OSSignalCond((OSCondition *)cond);
    return 0;
}

/* Alarm callback used for timed wait */
static void
SDL_CondWaitTimeoutCallback(OSAlarm *alarm, OSContext *context)
{
   WIIU_CondWaitTimeoutData *data = (WIIU_CondWaitTimeoutData *)OSGetAlarmUserData(alarm);
   if (data) {
       data->timed_out = true;
       OSSignalCond(data->cond);
   }
}

/* Wait on a condition variable with timeout (milliseconds) */
/* The mutex must be locked before calling. Returns 0 or SDL_MUTEX_TIMEDOUT. */
int
SDL_CondWaitTimeout(SDL_cond * cond, SDL_mutex * mutex, Uint32 ms)
{
    WIIU_CondWaitTimeoutData data;
    OSAlarm alarm;

    if (!cond || !mutex) {
        return SDL_SetError("Passed a NULL condition or mutex");
    }

    data.timed_out = false;
    data.cond = (OSCondition *)cond;

    /* zero timeout -> immediate timeout */
    if (ms == 0) {
        return SDL_MUTEX_TIMEDOUT;
    }

    /* create and set alarm */
    OSCreateAlarm(&alarm);
    OSSetAlarmUserData(&alarm, &data);
    OSSetAlarm(&alarm, OSMillisecondsToTicks(ms), &SDL_CondWaitTimeoutCallback);

    /* wait (this unlocks the mutex internally on Wii U cond API) */
    OSWaitCond((OSCondition *)cond, (OSMutex *)mutex);

    /* cancel alarm (if it already fired this is a no-op) */
    OSCancelAlarm(&alarm);

    return data.timed_out ? SDL_MUTEX_TIMEDOUT : 0;
}

/* Wait forever on the condition variable */
int
SDL_CondWait(SDL_cond * cond, SDL_mutex * mutex)
{
    if (!cond || !mutex) {
        return SDL_SetError("Passed a NULL condition or mutex");
    }

    OSWaitCond((OSCondition *)cond, (OSMutex *)mutex);
    return 0;
}

#endif /* SDL_THREAD_WIIU */
