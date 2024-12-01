/*
 * SPDX-FileCopyrightText: 2021 Kyunghwan Kwon <k@libmcu.org>
 *
 * SPDX-License-Identifier: MIT
 */

#include "libmcu/button.h"
#include "libmcu/button_overrides.h"
#include "libmcu/compiler.h"

#include <string.h>

#if !defined(BUTTON_MAX)
#define BUTTON_MAX			8
#endif
#if !defined(BUTTON_SAMPLING_INTERVAL_MS)
#define BUTTON_SAMPLING_INTERVAL_MS	10U
#endif
#if !defined(BUTTON_MIN_PRESS_TIME_MS)
#define BUTTON_MIN_PRESS_TIME_MS	60U
#endif
#if !defined(BUTTON_REPEAT_DELAY_MS)
#define BUTTON_REPEAT_DELAY_MS		300U
#endif
#if !defined(BUTTON_REPEAT_RATE_MS)
#define BUTTON_REPEAT_RATE_MS		200U
#endif
#if !defined(BUTTON_CLICK_WINDOW_MS)
#define BUTTON_CLICK_WINDOW_MS		500U
#endif
static_assert(BUTTON_MIN_PRESS_TIME_MS > BUTTON_SAMPLING_INTERVAL_MS,
		"The sampling period time must be less than press hold time.");

typedef enum {
	STATE_IDLE			= 0x00U,
	STATE_PRESSED			= 0x01U,
	STATE_RELEASED			= 0x02U,
	STATE_DOWN			= 0x04U,
	STATE_UP			= 0x08U,
	STATE_DEBOUNCING		= 0x10U,
} button_state_t;

typedef uint32_t waveform_t;

struct button_data {
	waveform_t waveform;
	uint32_t time_pressed;
	uint32_t time_released;
	uint32_t time_repeat;
	uint8_t clicks; /**< the number of clicks */
};

struct button {
	struct button_data data;
	struct button_param param;

	button_get_state_func_t get_state;
	void *get_state_ctx;
	button_callback_t callback;
	void *callback_ctx;

	uint32_t timestamp;

	bool allocated;
	bool active;
	bool pressed;
};

static struct button *new_button(struct button *btns)
{
	for (int i = 0; i < BUTTON_MAX; i++) {
		struct button *p = &btns[i];
		if (!p->allocated) {
			p->allocated = true;
			return p;
		}
	}

	return NULL;
}

static void free_button(struct button *btn)
{
	memset(btn, 0, sizeof(*btn));
}

static void get_default_param(struct button_param *param)
{
	*param = (struct button_param) {
		.sampling_interval_ms = BUTTON_SAMPLING_INTERVAL_MS,
		.min_press_time_ms = BUTTON_MIN_PRESS_TIME_MS,
		.repeat_delay_ms = BUTTON_REPEAT_DELAY_MS,
		.repeat_rate_ms = BUTTON_REPEAT_RATE_MS,
		.click_window_ms = BUTTON_CLICK_WINDOW_MS,
	};
}

static uint16_t get_min_pressed_pulse_count(const struct button *btn)
{
	return btn->param.min_press_time_ms / btn->param.sampling_interval_ms;
}

static waveform_t get_min_pressed_waveform(const struct button *btn)
{
	return 1U << get_min_pressed_pulse_count(btn);
}

static waveform_t get_waveform_debouncing_mask(const struct button *btn)
{
	return (1U << get_min_pressed_pulse_count(btn)) - 1;
}

static waveform_t get_waveform_mask(const struct button *btn)
{
	return (1U << (get_min_pressed_pulse_count(btn) + 1)) - 1;
}

static waveform_t get_waveform(const struct button *btn)
{
	return btn->data.waveform & get_waveform_mask(btn);
}

static void update_waveform(waveform_t *waveform, const button_level_t pressed)
{
	*waveform <<= 1;
	*waveform |= pressed;
}

static bool is_param_ok(const struct button_param *param,
		const uint16_t min_pulse_count)
{
	if (!param->sampling_interval_ms ||
			!param->repeat_delay_ms ||
			!param->repeat_rate_ms ||
			!param->click_window_ms) {
		return false;
	}

	if (param->min_press_time_ms < param->sampling_interval_ms) {
		return false;
	}

	if (min_pulse_count >= (uint16_t)(sizeof(waveform_t) * 8 - 2)) {
		return false;
	}

	return true;
}

static bool is_button_pressed(const struct button *btn)
{
	if (btn->pressed) { /* already pressed */
		return false;
	}

	/* 0b0111111 */
	const waveform_t expected = get_min_pressed_waveform(btn) - 1;
	const waveform_t mask = get_waveform_debouncing_mask(btn);
	return (get_waveform(btn) & mask) == expected;
}

static bool is_button_released(const struct button *btn)
{
	if (!btn->pressed) { /* already released */
		return false;
	}

	/* 0b1000000 */
	const waveform_t expected = get_min_pressed_waveform(btn);
	return get_waveform(btn) == expected;
}

static bool is_button_up(const struct button *btn)
{
	const waveform_t mask = get_waveform_debouncing_mask(btn);
	return (get_waveform(btn) & mask) == 0;
}

static bool is_button_down(const struct button *btn)
{
	const waveform_t mask = get_waveform_debouncing_mask(btn);
	return (get_waveform(btn) & mask) == mask;
}

static bool is_click_window_closed(const struct button *btn,
		const uint32_t time_ms)
{
	return time_ms - btn->data.time_released >= btn->param.click_window_ms;
}

#include <stdio.h>
static waveform_t update_state(struct button *btn, const uint32_t pulses)
{
	/* If the elapsed time is greater than the sampling interval, update
	 * historical waveform to the current state as well. */
	for (uint32_t i = 0; i < pulses; i++) {
		const button_level_t level =
			(*btn->get_state)(btn->get_state_ctx);
		update_waveform(&btn->data.waveform, level);
	}

	return get_waveform(btn);
}

static bool process_pressed(struct button *btn, const uint32_t time_ms)
{
	btn->data.time_pressed = time_ms;
	btn->pressed = true;
	return true;
}

static bool process_released(struct button *btn, const uint32_t time_ms)
{
	btn->data.time_released = time_ms;
	btn->pressed = false;
	btn->data.clicks++;
	btn->data.time_repeat = 0;
	return true;
}

static bool process_holding(struct button *btn, const uint32_t time_ms)
{
	bool state_updated = false;

	if (btn->data.time_repeat) {
		if ((time_ms - btn->data.time_repeat)
				>= btn->param.repeat_rate_ms) {
			state_updated = true;
		}
	} else {
		if ((time_ms - btn->data.time_pressed)
				>= btn->param.repeat_delay_ms) {
			state_updated = true;
		}
	}

	if (state_updated) {
		btn->data.time_repeat = time_ms;
	}

	return state_updated;
}

static button_event_t process_button(struct button *btn, const uint32_t time_ms)
{
	const uint32_t activity_mask = STATE_PRESSED | STATE_DOWN
		| STATE_DEBOUNCING;
	const uint32_t elapsed_ms = time_ms - btn->timestamp;
	const uint32_t pulses = elapsed_ms / btn->param.sampling_interval_ms;
	button_state_t state = STATE_IDLE;
	button_event_t event = BUTTON_EVT_NONE;

	if (!pulses) {
		goto out;
	}

	const waveform_t waveform = update_state(btn, pulses);

	if (is_button_pressed(btn)) {
		state = STATE_PRESSED;
		event = BUTTON_EVT_PRESSED;
		process_pressed(btn, time_ms);
	} else if (is_button_released(btn)) {
		state = STATE_RELEASED;
		event = BUTTON_EVT_RELEASED;
		process_released(btn, time_ms);
	} else if (is_button_down(btn)) {
		state = STATE_DOWN;
		if (process_holding(btn, time_ms)) {
			event = BUTTON_EVT_HOLDING;
		}
	} else if (is_button_up(btn)) {
		state = STATE_UP;
	} else if (waveform) {
		state = STATE_DEBOUNCING;
	}

	if (!(state & activity_mask) && is_click_window_closed(btn, time_ms)) {
		btn->data.clicks = 0;
	}

	btn->timestamp = time_ms;
out:
	return event;
}

button_error_t button_step(struct button *btn, const uint32_t time_ms)
{
	if (btn == NULL) {
		return BUTTON_ERROR_INVALID_PARAM;
	}
	if (!btn->active) {
		return BUTTON_ERROR_DISABLED;
	}

	const button_event_t event =
		process_button(btn, time_ms);

	if (event != BUTTON_EVT_NONE && btn->callback) {
		(*btn->callback)(btn, event, 0, btn->callback_ctx);

		if (event == BUTTON_EVT_RELEASED) {
			(*btn->callback)(btn, BUTTON_EVT_CLICK,
					btn->data.clicks, btn->callback_ctx);
		}
	}

	return BUTTON_ERROR_NONE;
}

button_error_t button_set_param(struct button *btn,
		const struct button_param *param)
{
	if (btn == NULL || param == NULL) {
		return BUTTON_ERROR_INVALID_PARAM;
	}

	if (is_param_ok(param, get_min_pressed_pulse_count(btn)) == false) {
		return BUTTON_ERROR_INCORRECT_PARAM;
	}

	memcpy(&btn->param, param, sizeof(*param));
	return BUTTON_ERROR_NONE;
}

button_error_t button_get_param(const struct button *btn,
		struct button_param *param)
{
	if (btn == NULL || param == NULL) {
		return BUTTON_ERROR_INVALID_PARAM;
	}

	memcpy(param, &btn->param, sizeof(*param));
	return BUTTON_ERROR_NONE;
}

bool button_busy(const struct button *btn)
{
	return !is_button_up(btn);
}

button_error_t button_enable(struct button *btn)
{
	if (btn == NULL) {
		return BUTTON_ERROR_INVALID_PARAM;
	}

	btn->active = true;
	return BUTTON_ERROR_NONE;
}

button_error_t button_disable(struct button *btn)
{
	if (btn == NULL) {
		return BUTTON_ERROR_INVALID_PARAM;
	}

	btn->active = false;
	return BUTTON_ERROR_NONE;
}

struct button *button_new(button_get_state_func_t f_get, void *f_get_ctx,
		button_callback_t cb, void *cb_ctx)
{
	static struct button btns[BUTTON_MAX];
	struct button *btn = NULL;

	if (f_get == NULL) {
		return NULL;
	}

	button_lock();
	btn = new_button(btns);
	button_unlock();

	if (btn) {
		btn->get_state = f_get;
		btn->get_state_ctx = f_get_ctx;
		btn->callback = cb;
		btn->callback_ctx = cb_ctx;
		get_default_param(&btn->param);
	}

	return btn;
}

void button_delete(struct button *btn)
{
	button_lock();
	free_button(btn);
	button_unlock();
}
