/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"

#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f
#define MAX_TOUCH_SLOTS 6

extern void port_trace(const char *format, ...);

static int touch_hardware_ids[MAX_TOUCH_SLOTS];

static int touch_slot_for_hardware_id(int hardware_id, int create) {
    for (int slot = 0; slot < MAX_TOUCH_SLOTS; ++slot) {
        if (touch_hardware_ids[slot] == hardware_id)
            return slot;
    }
    if (create) {
        for (int slot = 0; slot < MAX_TOUCH_SLOTS; ++slot) {
            if (touch_hardware_ids[slot] < 0) {
                touch_hardware_ids[slot] = hardware_id;
                return slot;
            }
        }
    }
    return -1;
}

void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0;
        *y = 0;
        return;
    }

    // normalize
    *x = *x / magnitude;
    *y = *y / magnitude;

    float multiplier = ((magnitude - deadzone) / (1 - deadzone));
    *x = *x * multiplier;
    *y = *y * multiplier;
}

void controls_init() {
    for (int slot = 0; slot < MAX_TOUCH_SLOTS; ++slot)
        touch_hardware_ids[slot] = -1;

    // Enable analog sticks and touchscreen
    int ctrl_result = sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    int touch_result = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);

    // Enable accelerometer
    int motion_result = sceMotionStartSampling();
    port_trace("INPUT: init ctrl=0x%x touch=0x%x motion=0x%x",
               ctrl_result, touch_result, motion_result);
}

void poll_touch();
void poll_pad();
void poll_accel();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

void controls_poll() {
    poll_touch();
    poll_pad();
    //poll_accel();
}

SceTouchData touch;
SceTouchData touch_old;

void poll_touch() {
    int samples = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (samples < 0) {
        static int touch_error_logged;
        if (!touch_error_logged++)
            port_trace("INPUT: sceTouchPeek failed result=0x%x", samples);
        return;
    }

    for (int i = 0; i < touch.reportNum; i++) {
        float x = (float) touch.report[i].x * 960.f / 1920.0f;
        float y = (float) touch.report[i].y * 544.f / 1088.0f;

        // Check if the finger was down before to distinguish between the Move and Down events
        int finger_down = 0;

        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    finger_down = 1;
                    break;
                }
            }
        }

        if (!finger_down) {
            int slot = touch_slot_for_hardware_id(touch.report[i].id, 1);
            if (slot >= 0)
                controls_handler_touch(slot, x, y, CONTROLS_ACTION_DOWN);
        } else {
            int slot = touch_slot_for_hardware_id(touch.report[i].id, 1);
            if (slot >= 0)
                controls_handler_touch(slot, x, y, CONTROLS_ACTION_MOVE);
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;

        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id ) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = (float) touch_old.report[i].x * 960.f / 1920.0f;
            float y = (float) touch_old.report[i].y * 544.f / 1088.0f;

            int slot = touch_slot_for_hardware_id(touch_old.report[i].id, 0);
            if (slot >= 0) {
                controls_handler_touch(slot, x, y, CONTROLS_ACTION_UP);
                touch_hardware_ids[slot] = -1;
            }
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
}

static ButtonMapping mapping[] = {
        { SCE_CTRL_UP,        AKEYCODE_DPAD_UP },
        { SCE_CTRL_DOWN,      AKEYCODE_DPAD_DOWN },
        { SCE_CTRL_LEFT,      AKEYCODE_DPAD_LEFT },
        { SCE_CTRL_RIGHT,     AKEYCODE_DPAD_RIGHT },
        /* The target APK uses the pre-API-12 Xperia Play convention. */
        { SCE_CTRL_CROSS,     AKEYCODE_DPAD_CENTER },
        { SCE_CTRL_CIRCLE,    AKEYCODE_BACK },
        { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_X },
        { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_Y },
        { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
        { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
        { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
        { SCE_CTRL_SELECT,    AKEYCODE_BUTTON_SELECT },
};

uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;

float analog_lx[3] = { 0 };
float analog_ly[3] = { 0 };
float analog_rx[3] = { 0 };
float analog_ry[3] = { 0 };

void poll_pad() {
    SceCtrlData pad = {0};
    int samples = sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    if (samples <= 0) {
        /* Ext2 is not available on every taiHEN/kernel combination. */
        samples = sceCtrlPeekBufferPositive(0, &pad, 1);
    }
    if (samples <= 0) {
        static int pad_error_logged;
        if (!pad_error_logged++)
            port_trace("INPUT: controller polling failed result=0x%x", samples);
        return;
    }

    // Gamepad buttons
    old_buttons = current_buttons;
    current_buttons = pad.buttons;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    // Analog sticks
    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);
}

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone) {
    readings_x[0] = (raw_x - 128.0f) / 128.0f;
    readings_y[0] = (raw_y - 128.0f) / 128.0f;

    coord_normalize(&readings_x[0], &readings_y[0], deadzone);

    // Last two readings are 0, the one before that isn't ==> MOTION_ACTION_UP
    if (
        (readings_x[0] == 0.f && readings_y[0] == 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] != 0.f || readings_y[2] != 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_UP);
    }
    // Current reading isn't 0, the two before are ==> MOTION_ACTION_DOWN
    else if (
        (readings_x[0] != 0.f || readings_y[0] != 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] == 0.f && readings_y[2] == 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_DOWN);
    }
    // Other cases ==> MOTION_ACTION_MOVE
    else {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_MOVE);
    }

    readings_x[2] = readings_x[1];
    readings_y[2] = readings_y[1];
    readings_x[1] = readings_x[0];
    readings_y[1] = readings_y[0];
}
