/*
 * Copyright (C) 2008 The Android Open Source Project.
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// #define LOG_NDEBUG 0

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)<(b)?(b):(a))
#endif

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;
static struct light_state_t g_battery;
static int g_attention = 0;
static int g_led_is_dt = 0;

char const*const RED_LED_FILE
        = "/sys/class/leds/red/brightness";

char const*const GREEN_LED_FILE
        = "/sys/class/leds/green/brightness";

char const*const BLUE_LED_FILE
        = "/sys/class/leds/blue/brightness";

char const*const LCD_FILE
        = "/sys/class/leds/lcd-backlight/brightness";

char const*const RED_BLINK_FILE
        = "/sys/class/leds/red/blink";

char const*const GREEN_BLINK_FILE
        = "/sys/class/leds/green/blink";

#if 0
char const*const BLUE_BLINK_FILE
        = "/sys/class/leds/blue/blink";
#endif

char const *const LED_DT_RED_RAMP_STEP_FILE
        = "/sys/class/leds/red/ramp_step_ms";

char const *const LED_DT_GREEN_RAMP_STEP_FILE
        = "/sys/class/leds/green/ramp_step_ms";

#if 0
char const *const LED_DT_BLUE_RAMP_STEP_FILE
        = "/sys/class/leds/blue/ramp_step_ms";
#endif

char const *const LED_DT_RED_DUTY_FILE
        = "/sys/class/leds/red/duty_pcts";

char const *const LED_DT_GREEN_DUTY_FILE
        = "/sys/class/leds/green/duty_pcts";

#if 0
char const *const LED_DT_BLUE_DUTY_FILE
        = "/sys/class/leds/blue/duty_pcts";
#endif

/* Number of steps to use in the duty array */
#define LED_DT_DUTY_STEPS       50

/* Brightness ramp up/down time for blinking */
#define LED_DT_RAMP_MS          500

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);

    /*
     * Determine whether LED is DT based.
     *
     * Traditional LED drivers control blinking via grpfreq/grppwm.
     * DT based LED drivers control blinking via ramp_step_ms/duty_pcts.
     *
     * Thus, if duty_pcts exists, the driver is DT based.
     */
    g_led_is_dt = (
            access(LED_DT_RED_DUTY_FILE, R_OK) == 0
            && access(LED_DT_GREEN_DUTY_FILE, R_OK) == 0
#if 0
            && access(LED_DT_BLUE_DUTY_FILE, R_OK) == 0
#endif
    );
}

static int
write_string(const char *path, const char *buffer)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        int bytes = strlen(buffer);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_string failed to open %s (%s)\n", path, strerror(errno));
            already_warned = 1;
        }
        return -errno;
    }
}

static int
write_int(char const* path, int value)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[20];
        int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
        ssize_t amt = write(fd, buffer, (size_t)bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int
is_lit(struct light_state_t const* state)
{
    return state->color & 0x00ffffff;
}

static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int
set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    err = write_int(LCD_FILE, brightness);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_speaker_light_locked(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int len;
    int alpha, red, green, blue;
    int blink;
    int onMS, offMS;
    unsigned int colorRGB;

    /* DT LED */
    char dutystr[(3+1)*LED_DT_DUTY_STEPS+1];
    char* p = dutystr;
    int stepMS;
    int n;

    switch (state->flashMode) {
        case LIGHT_FLASH_TIMED:
            onMS = state->flashOnMS;
            offMS = state->flashOffMS;
            break;
        case LIGHT_FLASH_NONE:
        default:
            onMS = 0;
            offMS = 0;
            break;
    }

    colorRGB = state->color;

#if 0
    ALOGD("set_speaker_light_locked mode %d, colorRGB=%08X, onMS=%d, offMS=%d\n",
            state->flashMode, colorRGB, onMS, offMS);
#endif

    red = (colorRGB >> 16) & 0xFF;
    green = (colorRGB >> 8) & 0xFF;
    blue = colorRGB & 0xFF;

    if (onMS > 0 && offMS > 0) {
        blink = 1;
    } else {
        blink = 0;
    }

    if (blink) {
        if (g_led_is_dt) {
            onMS = max(onMS, LED_DT_RAMP_MS);
            offMS = max(offMS, LED_DT_RAMP_MS);
            stepMS = (onMS+offMS)/LED_DT_DUTY_STEPS;

            p += sprintf(p, "0");
            for (n = 1; n < (onMS/stepMS); ++n) {
                p += sprintf(p, ",%d", min((100*n*stepMS)/LED_DT_RAMP_MS, 100));
            }
            for (n = 0; n < LED_DT_DUTY_STEPS-(onMS/stepMS); ++n) {
                p += sprintf(p, ",%d", 100 - min((100*n*stepMS)/LED_DT_RAMP_MS, 100));
            }
            p += sprintf(p, "\n");

            if (red) {
                write_int(LED_DT_RED_RAMP_STEP_FILE, stepMS);
                write_string(LED_DT_RED_DUTY_FILE, dutystr);
            }
            if (green) {
                write_int(LED_DT_GREEN_RAMP_STEP_FILE, stepMS);
                write_string(LED_DT_GREEN_DUTY_FILE, dutystr);
            }
#if 0
            if (blue) {
                write_int(LED_DT_BLUE_RAMP_STEP_FILE, stepMS);
                write_string(LED_DT_BLUE_DUTY_FILE, dutystr);
            }
#endif
        }

        if (red)
            write_int(RED_BLINK_FILE, blink);
        if (green)
            write_int(GREEN_BLINK_FILE, blink);
#if 0
        if (blue)
            write_int(BLUE_BLINK_FILE, blink);
#endif
    } else {
        write_int(RED_LED_FILE, red);
        write_int(GREEN_LED_FILE, green);
        write_int(BLUE_LED_FILE, blue);
    }

    return 0;
}

static void
handle_speaker_battery_locked(struct light_device_t* dev)
{
    if (is_lit(&g_battery)) {
        set_speaker_light_locked(dev, &g_battery);
    } else {
        set_speaker_light_locked(dev, &g_notification);
    }
}

static int
set_light_notifications(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    g_notification = *state;
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_attention(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    if (state->flashMode == LIGHT_FLASH_HARDWARE) {
        g_attention = state->flashOnMS;
    } else if (state->flashMode == LIGHT_FLASH_NONE) {
        g_attention = 0;
    }
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_battery(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    g_battery = *state;
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return 0;
}


/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_light_notifications;
    else if (0 == strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_light_battery;
    else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
        set_light = set_light_attention;
    else
        return -EINVAL;

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));

    if(!dev)
        return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "lights Module",
    .author = "Google, Inc.",
    .methods = &lights_module_methods,
};
