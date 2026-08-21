/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/workqueue.h>

#include <zmk/behavior.h>
#include <dt-bindings/zmk/rgb.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)

#error "A zmk,underglow chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_RAINBOW,
    UNDERGLOW_EFFECT_REACT,
    UNDERGLOW_EFFECT_LAYER,
    UNDERGLOW_EFFECT_KNIGHT,
    UNDERGLOW_EFFECT_FIRE,
    UNDERGLOW_EFFECT_CONFETTI,
    UNDERGLOW_EFFECT_RIPPLE,
    UNDERGLOW_EFFECT_HEAT,
    UNDERGLOW_EFFECT_POMODORO,
    UNDERGLOW_EFFECT_BATTERY,
    UNDERGLOW_EFFECT_MATRIX,
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_rgb_underglow_effect_solid(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_rgb_underglow_effect_breathe(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_underglow_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

// Full colour wheel spread twice over the strip, so two neighbouring LEDs are
// always far apart on the wheel, scrolling fast. Saturation is pinned to the
// maximum: this effect is meant to be loud whatever the stored colour is.
static void zmk_rgb_underglow_effect_rainbow(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX * 2 / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;
        hsb.s = SAT_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 4;
    state.animation_step = state.animation_step % HUE_MAX;
}

// Keypress reactive: every key lights one LED in its own colour and fades out.
// Both halves react to the keys of the half they are on.
static uint8_t react_level[STRIP_NUM_PIXELS];
static uint16_t react_hue[STRIP_NUM_PIXELS];

static int64_t pomodoro_start;

static int8_t ripple_origin = -1;
static uint16_t ripple_step;
static uint8_t heat;

static void zmk_rgb_underglow_effect_react(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = react_hue[i];
        hsb.s = SAT_MAX;
        hsb.b = react_level[i];

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));

        uint8_t fade = 2 + state.animation_speed * 2;
        react_level[i] = react_level[i] > fade ? react_level[i] - fade : 0;
    }
}

static int rgb_underglow_react_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int i = ev->position % STRIP_NUM_PIXELS;
    react_hue[i] = (ev->position * 47) % HUE_MAX;
    react_level[i] = BRT_MAX;

    // the ripple and heat effects ride on the same event
    ripple_origin = i;
    ripple_step = 0;
    heat = heat > 92 ? 100 : heat + 8;

    // spill a little onto the neighbours so a keypress reads as a burst
    int l = (i + STRIP_NUM_PIXELS - 1) % STRIP_NUM_PIXELS;
    int r = (i + 1) % STRIP_NUM_PIXELS;
    if (react_level[l] < BRT_MAX / 2) {
        react_hue[l] = react_hue[i];
        react_level[l] = BRT_MAX / 2;
    }
    if (react_level[r] < BRT_MAX / 2) {
        react_hue[r] = react_hue[i];
        react_level[r] = BRT_MAX / 2;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rgb_underglow_react, rgb_underglow_react_listener);
ZMK_SUBSCRIPTION(rgb_underglow_react, zmk_position_state_changed);

// One hue per active layer, with a gentle gradient along the strip so it does
// not look flat. Only the central half knows the active layer, so it pushes
// the hue to the peripherals through the rgb behaviour, whose locality is
// global; both halves therefore end up on the same colour.
static uint16_t layer_hue = 0;

int zmk_rgb_underglow_set_layer_hue(uint16_t hue) {
    layer_hue = hue % HUE_MAX;
    return 0;
}

static void zmk_rgb_underglow_effect_layer(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (layer_hue + i * 6) % HUE_MAX;
        hsb.s = SAT_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static const struct device *rgb_ug_behavior = DEVICE_DT_GET_ANY(zmk_behavior_rgb_underglow);

static int rgb_underglow_layer_listener(const zmk_event_t *eh) {
    if (rgb_ug_behavior == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint16_t hue = (zmk_keymap_highest_layer_active() * 72) % HUE_MAX;

    // Invoking the behaviour rather than setting the hue directly is what gets
    // this to the other half: the behaviour is global, so the central relays
    // it to every peripheral and then runs it locally.
    struct zmk_behavior_binding binding = {
        .behavior_dev = rgb_ug_behavior->name,
        .param1 = RGB_LAYER_CMD,
        .param2 = hue,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    zmk_behavior_invoke_binding(&binding, event, true);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rgb_underglow_layer, rgb_underglow_layer_listener);
ZMK_SUBSCRIPTION(rgb_underglow_layer, zmk_layer_state_changed);
#endif

// xorshift, so the sparkly effects do not need an entropy source
static uint32_t rgb_rand_state = 0x2545f491;

static uint32_t rgb_rand(void) {
    rgb_rand_state ^= rgb_rand_state << 13;
    rgb_rand_state ^= rgb_rand_state >> 17;
    rgb_rand_state ^= rgb_rand_state << 5;
    return rgb_rand_state;
}

// Knight Rider: one bright dot bouncing along the strip, trail fading behind.
static uint8_t knight_level[STRIP_NUM_PIXELS];

static void zmk_rgb_underglow_effect_knight(void) {
    int span = STRIP_NUM_PIXELS > 1 ? (STRIP_NUM_PIXELS - 1) * 2 : 1;
    int pos = (state.animation_step / 16) % span;
    if (pos >= STRIP_NUM_PIXELS) {
        pos = span - pos;
    }

    uint8_t fade = 10 + state.animation_speed * 5;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        knight_level[i] = knight_level[i] > fade ? knight_level[i] - fade : 0;
    }
    knight_level[pos] = BRT_MAX;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.s = SAT_MAX;
        hsb.b = knight_level[i];

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 3;
    if (state.animation_step > 30000) {
        state.animation_step = 0;
    }
}

// Embers: every LED flickers on its own between deep red and yellow.
static uint8_t fire_level[STRIP_NUM_PIXELS];

static void zmk_rgb_underglow_effect_fire(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int drift = (int)(rgb_rand() % 25) - 12;
        int level = (int)fire_level[i] + drift;

        if (level < 20) {
            level = 20 + (int)(rgb_rand() % 25);
        }
        if (level > BRT_MAX) {
            level = BRT_MAX;
        }
        fire_level[i] = (uint8_t)level;

        struct zmk_led_hsb hsb = state.color;
        hsb.h = 5 + fire_level[i] * 45 / BRT_MAX;
        hsb.s = SAT_MAX;
        hsb.b = fire_level[i];

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }
}

// Confetti: random LEDs pop in random colours and fade out.
static uint8_t confetti_level[STRIP_NUM_PIXELS];
static uint16_t confetti_hue[STRIP_NUM_PIXELS];

static void zmk_rgb_underglow_effect_confetti(void) {
    if ((rgb_rand() % 10) < 2 + state.animation_speed) {
        int i = rgb_rand() % STRIP_NUM_PIXELS;
        confetti_hue[i] = rgb_rand() % HUE_MAX;
        confetti_level[i] = BRT_MAX;
    }

    uint8_t fade = 3 + state.animation_speed * 2;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = confetti_hue[i];
        hsb.s = SAT_MAX;
        hsb.b = confetti_level[i];

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));

        confetti_level[i] = confetti_level[i] > fade ? confetti_level[i] - fade : 0;
    }
}

// Ripple: a keypress sends two fronts outwards from the LED it maps to.
static void zmk_rgb_underglow_effect_ripple(void) {
    struct zmk_led_hsb off = state.color;
    off.b = 0;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(off);
    }

    if (ripple_origin < 0) {
        return;
    }

    int radius = ripple_step / 10;
    int brightness = BRT_MAX - ripple_step * BRT_MAX / (10 * STRIP_NUM_PIXELS);

    if (brightness <= 0 || radius >= STRIP_NUM_PIXELS) {
        ripple_origin = -1;
        return;
    }

    for (int d = -radius; d <= radius; d += (radius == 0 ? 1 : 2 * radius)) {
        int i = ripple_origin + d;
        if (i < 0 || i >= STRIP_NUM_PIXELS) {
            continue;
        }

        struct zmk_led_hsb hsb = state.color;
        hsb.h = (state.color.h + radius * 25) % HUE_MAX;
        hsb.s = SAT_MAX;
        hsb.b = brightness;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    ripple_step += 2 + state.animation_speed;
}

// Typing heat: idle is a calm blue, the strip climbs through green and amber
// to red as you type faster. Each half warms up with its own hand.
static void zmk_rgb_underglow_effect_heat(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (220 - heat * 220 / 100 + i * 5) % HUE_MAX;
        hsb.s = SAT_MAX;
        hsb.b = 20 + heat * 80 / 100;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    if (heat > 0) {
        heat--;
    }
}

// Pomodoro: the strip fills up over 25 minutes and drifts from green to red,
// then flashes when the session is over. Selecting the effect starts it, and
// since selecting is a global behaviour both halves start together.
#define POMODORO_MS (25 * 60 * 1000)

static void zmk_rgb_underglow_effect_pomodoro(void) {
    int64_t elapsed = k_uptime_get() - pomodoro_start;

    if (elapsed >= POMODORO_MS) {
        bool lit = ((elapsed / 400) % 2) == 0;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            struct zmk_led_hsb hsb = state.color;
            hsb.h = 0;
            hsb.s = SAT_MAX;
            hsb.b = lit ? BRT_MAX : 0;

            pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
        }
        return;
    }

    int filled = (int)(elapsed * STRIP_NUM_PIXELS / POMODORO_MS);
    int hue = 120 - (int)(elapsed * 120 / POMODORO_MS);

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = hue;
        hsb.s = SAT_MAX;
        hsb.b = i <= filled ? BRT_MAX : 8;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }
}

// Battery gauge: lit LEDs are the charge of the half you are looking at, and
// the colour walks from red to green with it.
static void zmk_rgb_underglow_effect_battery(void) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    uint8_t level = zmk_battery_state_of_charge();
#else
    uint8_t level = 100;
#endif

    int lit = (level * STRIP_NUM_PIXELS + 50) / 100;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = level * 120 / 100;
        hsb.s = SAT_MAX;
        hsb.b = i < lit ? BRT_MAX : 0;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }
}

// Digital rain: green drops light up at random and trail off.
static uint8_t matrix_level[STRIP_NUM_PIXELS];

static void zmk_rgb_underglow_effect_matrix(void) {
    if ((rgb_rand() % 10) < 3) {
        int i = rgb_rand() % STRIP_NUM_PIXELS;
        if (matrix_level[i] < 40) {
            matrix_level[i] = BRT_MAX;
        }
    }

    uint8_t fade = 2 + state.animation_speed;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = 120;
        hsb.s = SAT_MAX;
        hsb.b = matrix_level[i];

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));

        matrix_level[i] = matrix_level[i] > fade ? matrix_level[i] - fade : 0;
    }
}

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
    case UNDERGLOW_EFFECT_RAINBOW:
        zmk_rgb_underglow_effect_rainbow();
        break;
    case UNDERGLOW_EFFECT_REACT:
        zmk_rgb_underglow_effect_react();
        break;
    case UNDERGLOW_EFFECT_LAYER:
        zmk_rgb_underglow_effect_layer();
        break;
    case UNDERGLOW_EFFECT_KNIGHT:
        zmk_rgb_underglow_effect_knight();
        break;
    case UNDERGLOW_EFFECT_FIRE:
        zmk_rgb_underglow_effect_fire();
        break;
    case UNDERGLOW_EFFECT_CONFETTI:
        zmk_rgb_underglow_effect_confetti();
        break;
    case UNDERGLOW_EFFECT_RIPPLE:
        zmk_rgb_underglow_effect_ripple();
        break;
    case UNDERGLOW_EFFECT_HEAT:
        zmk_rgb_underglow_effect_heat();
        break;
    case UNDERGLOW_EFFECT_POMODORO:
        zmk_rgb_underglow_effect_pomodoro();
        break;
    case UNDERGLOW_EFFECT_BATTERY:
        zmk_rgb_underglow_effect_battery();
        break;
    case UNDERGLOW_EFFECT_MATRIX:
        zmk_rgb_underglow_effect_matrix();
        break;
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_tick_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            if (state.on) {
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
            }

            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_rgb_underglow_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

static int zmk_rgb_underglow_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct rgb_underglow_state){
        color : {
            h : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
    }

    return 0;
}

int zmk_rgb_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on;
    return 0;
}

int zmk_rgb_underglow_on(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    state.animation_step = 0;
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));

    return zmk_rgb_underglow_save_state();
}

static void zmk_rgb_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_underglow_off_handler);

int zmk_rgb_underglow_off(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    k_timer_stop(&underglow_tick);
    state.on = false;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;

    if (effect == UNDERGLOW_EFFECT_POMODORO) {
        pomodoro_start = k_uptime_get();
    }

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.color = color;

    return 0;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;

    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;

    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;

    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_hue(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_sat(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_brt(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_underglow_save_state();
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
struct rgb_underglow_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};

static int rgb_underglow_auto_state(bool target_wake_state) {
    static struct rgb_underglow_sleep_state sleep_state = {
        is_awake : true,
        rgb_state_before_sleeping : false
    };

    // wake up event while awake, or sleep event while sleeping -> no-op
    if (target_wake_state == sleep_state.is_awake) {
        return 0;
    }
    sleep_state.is_awake = target_wake_state;

    if (sleep_state.is_awake) {
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_rgb_underglow_on();
        } else {
            return zmk_rgb_underglow_off();
        }
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_rgb_underglow_off();
    }
}

static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
