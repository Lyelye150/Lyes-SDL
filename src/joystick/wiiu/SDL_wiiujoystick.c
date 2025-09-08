/*
  Simple DirectMedia Layer
  Copyright (C) 2018 Roberto Van Eeden <r.r.qwertyuiop.r.r@gmail.com>
  Copyright (C) 2019 Ash Logan <ash@heyquark.com>

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

#if SDL_JOYSTICK_WIIU

#include <vpad/input.h>
#include <padscore/wpad.h>
#include <padscore/kpad.h>
#include <coreinit/debug.h>

#include "SDL3/SDL_log.h"
#include "SDL3/SDL_assert.h"
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_joystick.h"

#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "../../events/SDL_touch_c.h"
#include "../../video/SDL_sysvideo.h"

#include "SDL_wiiujoystick.h"

/* Local helpers */
static int deviceMap[MAX_CONTROLLERS];            /* index -> WIIU_DEVICE */
static SDL_JoystickID instanceMap[MAX_CONTROLLERS]; /* index -> instance id */
static WPADExtensionType lastKnownExts[WIIU_NUM_WPADS];

static int  WIIU_JoystickInit(void);
static int  WIIU_JoystickGetCount(void);
static void WIIU_JoystickDetect(void);
static const char *WIIU_JoystickGetDeviceName(int device_index);
static const char *WIIU_JoystickGetDevicePath(int device_index);
static int  WIIU_JoystickGetDevicePlayerIndex(int device_index);
static void WIIU_JoystickSetDevicePlayerIndex(int device_index, int player_index);
static SDL_JoystickGUID WIIU_JoystickGetDeviceGUID(int device_index);
static SDL_JoystickID WIIU_JoystickGetDeviceInstanceID(int device_index);
static int  WIIU_JoystickOpen(SDL_Joystick *joystick, int device_index);
static int  WIIU_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble);
static int  WIIU_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble);
static Uint32 WIIU_JoystickGetCapabilities(SDL_Joystick *joystick);
static int  WIIU_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue);
static int  WIIU_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size);
static int  WIIU_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled);
static void WIIU_JoystickUpdate(SDL_Joystick *joystick);
static void WIIU_JoystickClose(SDL_Joystick *joystick);
static void WIIU_JoystickQuit(void);
static SDL_bool WIIU_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping * out);
static SDL_Window *WIIU_GetGamepadWindow(void);

/* small helper accessors */
static int WIIU_GetDeviceForIndex(int device_index) {
    return deviceMap[device_index];
}
static int WIIU_GetIndexForDevice(int wiiu_device) {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (deviceMap[i] == wiiu_device) return i;
    }
    return -1;
}
static int WIIU_GetNextDeviceIndex(void) {
    return WIIU_GetIndexForDevice(WIIU_DEVICE_INVALID);
}
static SDL_JoystickID WIIU_GetInstForIndex(int device_index) {
    if (device_index == -1) return -1;
    return instanceMap[device_index];
}
static SDL_JoystickID WIIU_GetInstForDevice(int wiiu_device) {
    int idx = WIIU_GetIndexForDevice(wiiu_device);
    return WIIU_GetInstForIndex(idx);
}
static int WIIU_GetDeviceForInst(SDL_JoystickID instance) {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (instanceMap[i] == instance) return deviceMap[i];
    }
    return WIIU_DEVICE_INVALID;
}

static void WIIU_RemoveDevice(int wiiu_device) {
    int device_index = WIIU_GetIndexForDevice(wiiu_device);
    if (device_index == -1) return;

    for (int i = device_index; i < MAX_CONTROLLERS; i++) {
        if (i + 1 < MAX_CONTROLLERS) {
            deviceMap[i] = deviceMap[i + 1];
            instanceMap[i] = instanceMap[i + 1];
        } else {
            deviceMap[i] = WIIU_DEVICE_INVALID;
            instanceMap[i] = -1;
        }
    }
}

/* Find a gamepad window (first visible and not TV-exclusive) */
static SDL_Window *WIIU_GetGamepadWindow(void) {
    SDL_VideoDevice *dev = SDL_GetVideoDevice();
    if (!dev) return NULL;
    for (SDL_Window *win = dev->windows; win; win = win->next) {
        if ((win->flags & SDL_WINDOW_SHOWN) && !(win->flags & SDL_WINDOW_WIIU_TV_ONLY))
            return win;
    }
    return NULL;
}

/* Initialize joystick subsystem for WiiU */
static int WIIU_JoystickInit(void)
{
    VPADInit();
    KPADInit();
    WPADEnableURCC(1);

    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        deviceMap[i] = WIIU_DEVICE_INVALID;
        instanceMap[i] = -1;
    }
    WIIU_JoystickDetect();
    return 0;
}

/* Return count (next free index) */
static int WIIU_JoystickGetCount(void)
{
    return WIIU_GetNextDeviceIndex();
}

/* Detect and add/remove controllers */
static void WIIU_JoystickDetect(void)
{
    /* sanity checks for dangling pairs */
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (deviceMap[i] == WIIU_DEVICE_INVALID && instanceMap[i] != -1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "WiiU device_index %d dangling instance %d!\n", i, instanceMap[i]);
            SDL_PrivateJoystickRemoved(instanceMap[i]);
            instanceMap[i] = -1;
        }
        if (deviceMap[i] != WIIU_DEVICE_INVALID && instanceMap[i] == -1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "WiiU device_index %d assigned to %d, but has no instance!\n", i, deviceMap[i]);
            instanceMap[i] = -1;
        }
    }

    /* Detect Gamepad (VPAD) */
    if (WIIU_GetIndexForDevice(WIIU_DEVICE_GAMEPAD) == -1) {
        VPADStatus status;
        VPADReadError err;
        VPADRead(VPAD_CHAN_0, &status, 1, &err);
        if (err == VPAD_READ_SUCCESS || err == VPAD_READ_NO_SAMPLES) {
            int device_index = WIIU_GetNextDeviceIndex();
            if (device_index != -1) {
                deviceMap[device_index]   = WIIU_DEVICE_GAMEPAD;
                instanceMap[device_index] = SDL_GetNextJoystickInstanceID();
                SDL_PrivateJoystickAdded(instanceMap[device_index]);
                SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                            "WiiU: Detected Gamepad, assigned device %d/instance %d\n",
                            device_index, instanceMap[device_index]);
            }
        }
    }

    /* Detect WPAD / KPAD controllers */
    for (int i = 0; i < WIIU_NUM_WPADS; i++) {
        WPADExtensionType ext;
        int wiiu_device = WIIU_DEVICE_WPAD(i);
        int ret = WPADProbe(WIIU_WPAD_CHAN(wiiu_device), &ext);
        if (ret == 0) { /* controller connected */
            if (WIIU_GetIndexForDevice(wiiu_device) == -1) {
                int device_index = WIIU_GetNextDeviceIndex();
                if (device_index != -1) {
                    deviceMap[device_index]   = wiiu_device;
                    instanceMap[device_index] = SDL_GetNextJoystickInstanceID();
                    lastKnownExts[WIIU_WPAD_CHAN(wiiu_device)] = ext;
                    SDL_PrivateJoystickAdded(instanceMap[device_index]);
                    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                                "WiiU: Detected WPAD, assigned device %d/instance %d\n",
                                device_index, instanceMap[device_index]);
                }
            } else if (ext != lastKnownExts[WIIU_WPAD_CHAN(wiiu_device)]) {
                /* extension changed, force a reconnect */
                SDL_JoystickID instance = WIIU_GetInstForDevice(wiiu_device);
                SDL_PrivateJoystickRemoved(instance);
                WIIU_RemoveDevice(wiiu_device);
            }
        } else if (ret == -1) { /* no controller */
            if (WIIU_GetIndexForDevice(wiiu_device) != -1) {
                SDL_JoystickID instance = WIIU_GetInstForDevice(wiiu_device);
                SDL_PrivateJoystickRemoved(instance);
                WIIU_RemoveDevice(wiiu_device);
            }
        }
    }
}

/* Device name */
static const char *WIIU_JoystickGetDeviceName(int device_index)
{
    int wiiu_device = WIIU_GetDeviceForIndex(device_index);
    if (wiiu_device == WIIU_DEVICE_GAMEPAD) {
        return "WiiU Gamepad";
    } else if (wiiu_device == WIIU_DEVICE_WPAD(0)) {
        RETURN_WPAD_NAME(1, lastKnownExts[0]);
    } else if (wiiu_device == WIIU_DEVICE_WPAD(1)) {
        RETURN_WPAD_NAME(2, lastKnownExts[1]);
    } else if (wiiu_device == WIIU_DEVICE_WPAD(2)) {
        RETURN_WPAD_NAME(3, lastKnownExts[2]);
    } else if (wiiu_device == WIIU_DEVICE_WPAD(3)) {
        RETURN_WPAD_NAME(4, lastKnownExts[3]);
    }
    return "Unknown";
}

static const char * WIIU_JoystickGetDevicePath(int device_index)
{
    return NULL;
}

/* Player index helpers */
static int WIIU_JoystickGetDevicePlayerIndex(int device_index)
{
    int wiiu_device = WIIU_GetDeviceForIndex(device_index);
    switch (wiiu_device) {
        case WIIU_DEVICE_GAMEPAD: return 0;
        case WIIU_DEVICE_WPAD(0): return 1;
        case WIIU_DEVICE_WPAD(1): return 2;
        case WIIU_DEVICE_WPAD(2): return 3;
        case WIIU_DEVICE_WPAD(3): return 4;
        default: return -1;
    }
}
static void WIIU_JoystickSetDevicePlayerIndex(int device_index, int player_index) { (void)device_index; (void)player_index; }

/* GUID */
static SDL_JoystickGUID WIIU_JoystickGetDeviceGUID(int device_index)
{
    SDL_JoystickGUID guid;
    const int wiiu_device = WIIU_GetDeviceForIndex(device_index);

    switch (wiiu_device)
    {
        case WIIU_DEVICE_GAMEPAD:
            guid = SDL_CreateJoystickGUIDForName("Wii U Gamepad");
            break;

        case WIIU_DEVICE_WPAD(0):
        case WIIU_DEVICE_WPAD(1):
        case WIIU_DEVICE_WPAD(2):
        case WIIU_DEVICE_WPAD(3):
        {
            switch (lastKnownExts[WIIU_WPAD_CHAN(wiiu_device)])
            {
                case WPAD_EXT_CORE:
                case WPAD_EXT_MPLUS:
                default:
                    guid = SDL_CreateJoystickGUIDForName("Wii Remote");
                    break;

                case WPAD_EXT_NUNCHUK:
                case WPAD_EXT_MPLUS_NUNCHUK:
                    guid = SDL_CreateJoystickGUIDForName("Wii Nunchuk");
                    break;

                case WPAD_EXT_CLASSIC:
                case WPAD_EXT_MPLUS_CLASSIC:
                    guid = SDL_CreateJoystickGUIDForName("Wii Classic Controller");
                    break;

                case WPAD_EXT_PRO_CONTROLLER:
                    guid = SDL_CreateJoystickGUIDForName("Wii U Pro Controller");
                    break;
            }
            break;
        }

        default:
            SDL_zero(guid);
            break;
    }

    return guid;
}

static SDL_JoystickID WIIU_JoystickGetDeviceInstanceID(int device_index)
{
    return WIIU_GetInstForIndex(device_index);
}

/* Open joystick */
static int WIIU_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    int wiiu_device = WIIU_GetDeviceForIndex(device_index);
    switch (wiiu_device) {
        case WIIU_DEVICE_GAMEPAD: {
            /* Touch device on gamepad */
            /* SDL3 touch API changed slightly from SDL2 — adapt as needed. */
#if 0
            /* Example if your SDL3 provides SDL_AddTouchDevice(name, type) */
            SDL_AddTouchDevice(0, SDL_TOUCH_DEVICE_DIRECT, "WiiU Gamepad Touchscreen");
#else
            /* If your SDL3 doesn't have SDL_AddTouchDevice, you can register touch
             * differently or skip registering here. The important thing is to send
             * finger events below when the VPAD reports touch. */
#endif

            joystick->nbuttons = SIZEOF_ARR(vpad_button_map);
            joystick->naxes    = 4;
            joystick->nhats    = 0;
            break;
        }

        case WIIU_DEVICE_WPAD(0):
        case WIIU_DEVICE_WPAD(1):
        case WIIU_DEVICE_WPAD(2):
        case WIIU_DEVICE_WPAD(3): {
            WPADExtensionType ext;
            int ret = WPADProbe(WIIU_WPAD_CHAN(wiiu_device), &ext);
            if (ret != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "WiiU_JoystickOpen: WPAD device %d failed probe!",
                    WIIU_WPAD_CHAN(wiiu_device));
                return -1;
            }

            switch (ext) {
                case WPAD_EXT_CORE:
                case WPAD_EXT_MPLUS:
                default:
                    joystick->nbuttons = SIZEOF_ARR(wiimote_button_map);
                    joystick->naxes    = 0;
                    joystick->nhats    = 0;
                    break;
                case WPAD_EXT_NUNCHUK:
                case WPAD_EXT_MPLUS_NUNCHUK:
                    joystick->nbuttons = SIZEOF_ARR(nunchuk_button_map);
                    joystick->naxes    = 2;
                    joystick->nhats    = 0;
                    break;
                case WPAD_EXT_CLASSIC:
                case WPAD_EXT_MPLUS_CLASSIC:
                    joystick->nbuttons = SIZEOF_ARR(classic_button_map);
                    joystick->naxes    = 4;
                    joystick->nhats    = 0;
                    break;
                case WPAD_EXT_PRO_CONTROLLER:
                    joystick->nbuttons = SIZEOF_ARR(pro_button_map);
                    joystick->naxes    = 4;
                    joystick->nhats    = 0;
                    break;
            }
            break;
        }
    }

    joystick->instance_id = WIIU_GetInstForIndex(device_index);
    return 0;
}

/* Rumble — not supported here */
static int WIIU_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick; (void)low_frequency_rumble; (void)high_frequency_rumble;
    return SDL_Unsupported();
}
static int WIIU_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick; (void)left_rumble; (void)right_rumble;
    return SDL_Unsupported();
}

/* Capabilities / LED / Effects / Sensors — unsupported or TODO */
static Uint32 WIIU_JoystickGetCapabilities(SDL_Joystick *joystick) { (void)joystick; return 0; }
static int WIIU_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue) { (void)joystick; (void)red; (void)green; (void)blue; return SDL_Unsupported(); }
static int WIIU_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size) { (void)joystick; (void)data; (void)size; return SDL_Unsupported(); }
static int WIIU_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled) { (void)joystick; (void)enabled; return SDL_Unsupported(); }

/* Update (called regularly to poll / generate events) */
static void WIIU_JoystickUpdate(SDL_Joystick *joystick)
{
    int16_t x1, y1, x2, y2;

    /* Gamepad (VPAD) */
    if (joystick->instance_id == WIIU_GetInstForDevice(WIIU_DEVICE_GAMEPAD)) {
        static uint16_t last_touch_x = 0;
        static uint16_t last_touch_y = 0;
        static uint16_t last_touched = 0;

        static int16_t x1_old = 0, y1_old = 0, x2_old = 0, y2_old = 0;

        VPADStatus vpad;
        VPADReadError error;
        VPADTouchData tpdata;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &error);
        if (error == VPAD_READ_INVALID_CONTROLLER) {
            /* Gamepad disconnected */
            SDL_PrivateJoystickRemoved(joystick->instance_id);
            WIIU_RemoveDevice(WIIU_DEVICE_GAMEPAD);
            return;
        } else if (error != VPAD_READ_SUCCESS) {
            return;
        }

        /* Touchscreen: adapt to your SDL3 touch API.
         * Below are two approaches — choose the one that matches your SDL3:
         *
         * 1) If SDL3 has SDL_AddTouchDevice() and SDL_SendTouchFingerEvent(), uncomment
         *    the SDL_AddTouchDevice() call in WIIU_JoystickOpen() and use the code
         *    below to create/send finger events.
         *
         * 2) If SDL3 uses SDL_PushEvent with SDL_Event and SDL_EVENT_FINGER_*, construct
         *    SDL_Event and push it.
         *
         * I left the code here as comments so you can adapt it to your local SDL3.
         */

        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &tpdata, &vpad.tpNormal);
        if (tpdata.touched) {
            SDL_Window *window = WIIU_GetGamepadWindow();

#if 0
            /* Example: create and send a SDL touch finger event (pseudo-code) */
            SDL_TouchFingerEvent fe;
            SDL_zero(fe);
            fe.type = SDL_EVENT_FINGER_DOWN; /* or SDL_EVENT_FINGER_MOTION if appropriate */
            fe.timestamp = SDL_GetTicks();
            fe.touchId = 0; /* touch device id used earlier, if any */
            fe.fingerId = 0; /* finger id */
            fe.x = (float)tpdata.x / 1280.0f;
            fe.y = (float)tpdata.y / 720.0f;
            fe.pressure = 1.0f;
            SDL_PushEvent((SDL_Event*)&fe);
#else
            /* Fallback: if you don't want to send a touch event here, you could call
             * your own app-provided callback or skip. Make sure to add touch registration
             * in WIIU_JoystickOpen if your SDL3 uses explicit registration.
             */
#endif

            /* Update last touched state for "finger up" events */
            last_touch_x = tpdata.x;
            last_touch_y = tpdata.y;
            last_touched = 1;
        } else if (last_touched) {
            SDL_Window *window = WIIU_GetGamepadWindow();
#if 0
            /* Example send finger up:
             * SDL_TouchFingerEvent fe;
             * fe.type = SDL_EVENT_FINGER_UP; ...
             * SDL_PushEvent((SDL_Event*)&fe);
             */
#endif
            last_touched = 0;
        }

        /* Analog sticks */
        x1 = (int16_t) ((vpad.leftStick.x) * 0x7ff0);
        y1 = (int16_t) -((vpad.leftStick.y) * 0x7ff0);
        x2 = (int16_t) ((vpad.rightStick.x) * 0x7ff0);
        y2 = (int16_t) -((vpad.rightStick.y) * 0x7ff0);

        if (x1 != x1_old) { SDL_PrivateJoystickAxis(joystick, 0, x1); x1_old = x1; }
        if (y1 != y1_old) { SDL_PrivateJoystickAxis(joystick, 1, y1); y1_old = y1; }
        if (x2 != x2_old) { SDL_PrivateJoystickAxis(joystick, 2, x2); x2_old = x2; }
        if (y2 != y2_old) { SDL_PrivateJoystickAxis(joystick, 3, y2); y2_old = y2; }

        /* Buttons (trigger/release bitfields) */
        for (int i = 0; i < joystick->nbuttons; i++) {
            if (vpad.trigger & vpad_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_PRESSED);
            if (vpad.release & vpad_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_RELEASED);
        }

    } else {
        /* WPAD / KPAD controllers */
        int wiiu_device = WIIU_GetDeviceForInst(joystick->instance_id);
        WPADExtensionType ext;
        KPADStatus kpad;
        int32_t err;

        if (WPADProbe(WIIU_WPAD_CHAN(wiiu_device), &ext) != 0) {
            return; /* no controller */
        }

        KPADReadEx(WIIU_WPAD_CHAN(wiiu_device), &kpad, 1, &err);
        if (err != KPAD_ERROR_OK) return;

        switch (ext) {
            case WPAD_EXT_CORE:
            case WPAD_EXT_MPLUS:
            default:
                for (int i = 0; i < joystick->nbuttons; i++) {
                    if (kpad.trigger & wiimote_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_PRESSED);
                    if (kpad.release & wiimote_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_RELEASED);
                }
                break;

            case WPAD_EXT_NUNCHUK:
            case WPAD_EXT_MPLUS_NUNCHUK:
                for (int i = 0; i < joystick->nbuttons; i++) {
                    if ((kpad.trigger | (kpad.nunchuk.trigger << 16)) & nunchuk_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_PRESSED);
                    if ((kpad.release | (kpad.nunchuk.release << 16)) & nunchuk_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_RELEASED);
                }
                x1 = (int16_t) ((kpad.nunchuk.stick.x) * 0x7ff0);
                y1 = (int16_t) -((kpad.nunchuk.stick.y) * 0x7ff0);
                SDL_PrivateJoystickAxis(joystick, 0, x1);
                SDL_PrivateJoystickAxis(joystick, 1, y1);
                break;

            case WPAD_EXT_CLASSIC:
            case WPAD_EXT_MPLUS_CLASSIC:
                for (int i = 0; i < joystick->nbuttons; i++) {
                    if (kpad.classic.trigger & classic_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_PRESSED);
                    if (kpad.classic.release & classic_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_RELEASED);
                }
                x1 = (int16_t) ((kpad.classic.leftStick.x) * 0x7ff0);
                y1 = (int16_t) -((kpad.classic.leftStick.y) * 0x7ff0);
                x2 = (int16_t) ((kpad.classic.rightStick.x) * 0x7ff0);
                y2 = (int16_t) -((kpad.classic.rightStick.y) * 0x7ff0);
                SDL_PrivateJoystickAxis(joystick, 0, x1);
                SDL_PrivateJoystickAxis(joystick, 1, y1);
                SDL_PrivateJoystickAxis(joystick, 2, x2);
                SDL_PrivateJoystickAxis(joystick, 3, y2);
                break;

            case WPAD_EXT_PRO_CONTROLLER:
                for (int i = 0; i < joystick->nbuttons; i++) {
                    if (kpad.pro.trigger & pro_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_PRESSED);
                    if (kpad.pro.release & pro_button_map[i]) SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_RELEASED);
                }
                x1 = (int16_t) ((kpad.pro.leftStick.x) * 0x7ff0);
                y1 = (int16_t) -((kpad.pro.leftStick.y) * 0x7ff0);
                x2 = (int16_t) ((kpad.pro.rightStick.x) * 0x7ff0);
                y2 = (int16_t) -((kpad.pro.rightStick.y) * 0x7ff0);
                SDL_PrivateJoystickAxis(joystick, 0, x1);
                SDL_PrivateJoystickAxis(joystick, 1, y1);
                SDL_PrivateJoystickAxis(joystick, 2, x2);
                SDL_PrivateJoystickAxis(joystick, 3, y2);
                break;
        }
    }
}

/* Close, quit, mapping */
static void WIIU_JoystickClose(SDL_Joystick *joystick) { (void)joystick; }
static void WIIU_JoystickQuit(void) { }
static SDL_bool WIIU_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping * out) { (void)device_index; (void)out; return SDL_FALSE; }

/* Driver struct */
SDL_JoystickDriver SDL_WIIU_JoystickDriver =
{
    WIIU_JoystickInit,
    WIIU_JoystickGetCount,
    WIIU_JoystickDetect,
    WIIU_JoystickGetDeviceName,
    WIIU_JoystickGetDevicePath,
    WIIU_JoystickGetDevicePlayerIndex,
    WIIU_JoystickSetDevicePlayerIndex,
    WIIU_JoystickGetDeviceGUID,
    WIIU_JoystickGetDeviceInstanceID,
    WIIU_JoystickOpen,
    WIIU_JoystickRumble,
    WIIU_JoystickRumbleTriggers,
    WIIU_JoystickGetCapabilities,
    WIIU_JoystickSetLED,
    WIIU_JoystickSendEffect,
    WIIU_JoystickSetSensorsEnabled,
    WIIU_JoystickUpdate,
    WIIU_JoystickClose,
    WIIU_JoystickQuit,
    WIIU_JoystickGetGamepadMapping,
};

#endif /* SDL_JOYSTICK_WIIU */
