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

#include "SDL_thread.h"
#include "SDL_systhread_c.h"

#include <coreinit/thread.h>
#include <coreinit/mutex.h>
#include <coreinit/systeminfo.h>
#include <stdlib.h>

/* Forward declaration */
static int SDLCALL SDL_RunThread(void *data);

int
SDL_SYS_CreateThread(SDL_Thread *thread, SDL_Function function, void *data, const char *name, void *stackaddr, size_t stacksize)
{
    OSThread *osthread;

    if (stacksize == 0) {
        stacksize = 64 * 1024; /* Wii U needs explicit stack size */
    }

    osthread = (OSThread *) SDL_malloc(sizeof(OSThread));
    if (!osthread) {
        return SDL_OutOfMemory();
    }

    void *thread_stack = SDL_malloc(stacksize);
    if (!thread_stack) {
        SDL_free(osthread);
        return SDL_OutOfMemory();
    }

    if (!OSCreateThread(osthread, SDL_RunThread, thread, thread_stack, stacksize, OS_THREAD_PRIORITY_DEFAULT, 0)) {
        SDL_free(thread_stack);
        SDL_free(osthread);
        return SDL_SetError("OSCreateThread() failed");
    }

    thread->handle = osthread;
    return 0;
}

static int SDLCALL
SDL_RunThread(void *data)
{
    SDL_Thread *thread = (SDL_Thread *)data;
    thread->retval = thread->func(thread->data);
    return 0;
}

void
SDL_SYS_SetupThread(const char *name)
{
    /* Not strictly needed for Wii U */
    (void)name;
}

SDL_ThreadID
SDL_ThreadID(void)
{
    return (SDL_ThreadID) OSGetCurrentThread();
}

int
SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
    /* Wii U threads have default priority; could be extended */
    (void)priority;
    return 0;
}

void
SDL_SYS_WaitThread(SDL_Thread *thread)
{
    if (thread && thread->handle) {
        OSJoinThread((OSThread *)thread->handle, NULL);
        SDL_free(thread->handle);
        thread->handle = NULL;
    }
}

void
SDL_SYS_DetachThread(SDL_Thread *thread)
{
    /* Nothing special needed on Wii U */
    (void)thread;
}

#endif /* SDL_THREAD_WIIU */

/* vi: set ts=4 sw=4 expandtab: */
