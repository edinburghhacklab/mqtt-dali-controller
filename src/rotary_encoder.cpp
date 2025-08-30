/*
 * mqtt-dali-controller
 * Copyright 2025  Simon Arlott
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "rotary_encoder.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <hal/rtc_cntl_ll.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <utility>

#include "thread.h"

RotaryEncoder::RotaryEncoder(std::array<gpio_num_t,2> pins)
		: pins_(pins) {
}

RotaryEncoder::~RotaryEncoder() {
	gpio_intr_disable(pins_[0]);
	gpio_intr_disable(pins_[1]);
}

void RotaryEncoder::start(WakeupThread &wakeup) {
	gpio_config_t config{};

	wakeup_ = &wakeup;

	config.pin_bit_mask = (1ULL << pins_[0]) | (1ULL << pins_[1]);
	config.mode = GPIO_MODE_INPUT;
	config.pull_up_en = GPIO_PULLUP_ENABLE;
	config.pull_down_en = GPIO_PULLDOWN_DISABLE;
	config.intr_type = GPIO_INTR_DISABLE;

	ESP_ERROR_CHECK(gpio_config(&config));
	uint64_t now_us = esp_timer_get_time();
	signal_[0].level = gpio_get_level(pins_[0]);
	signal_[1].level = gpio_get_level(pins_[1]);
	signal_[0].active_low = signal_[0].level;
	signal_[1].active_low = signal_[1].level;

	if (!signal_[0].active_low || !signal_[1].active_low) {
		if (!signal_[0].active_low) {
			ESP_ERROR_CHECK(gpio_set_pull_mode(pins_[0], GPIO_PULLDOWN_ONLY));
		}
		if (!signal_[1].active_low) {
			ESP_ERROR_CHECK(gpio_set_pull_mode(pins_[1], GPIO_PULLDOWN_ONLY));
		}
		now_us = esp_timer_get_time();
		signal_[0].level = gpio_get_level(pins_[0]);
		signal_[1].level = gpio_get_level(pins_[1]);
	}

	if (signal_[0].active()) {
		signal_[0].active_us = now_us;
	}
	if (signal_[1].active()) {
		signal_[1].active_us = now_us;
	}

	update_mode();

	ESP_ERROR_CHECK(gpio_isr_handler_add(pins_[0], rotary_encoder_interrupt_handler_0, this));
	ESP_ERROR_CHECK(gpio_isr_handler_add(pins_[1], rotary_encoder_interrupt_handler_1, this));
	ESP_ERROR_CHECK(gpio_set_intr_type(pins_[0], GPIO_INTR_ANYEDGE));
	ESP_ERROR_CHECK(gpio_set_intr_type(pins_[1], GPIO_INTR_ANYEDGE));
	ESP_ERROR_CHECK(gpio_intr_enable(pins_[0]));
	ESP_ERROR_CHECK(gpio_intr_enable(pins_[1]));
}

IRAM_ATTR inline void RotaryEncoder::update_mode() {
	mode_.store(static_cast<RotaryMode>(
		(signal_[0].active_low ? 0 : 1) | (signal_[1].active_low ? 0 : 2)));
}

std::pair<RotaryMode,long> RotaryEncoder::read() {
	static_assert(decltype(change_)::is_always_lock_free);
	static_assert(decltype(mode_)::is_always_lock_free);
	return {mode_.load(), change_.exchange(0L)};
}

void RotaryEncoder::debug(std::array<RotaryEncoderDebug,DEBUG_RECORDS> &records) const {
	size_t pos = debug_pos_;

	std::copy(debug_.cbegin(), debug_.cend(), records.begin());
	std::rotate(records.begin(), records.begin() + pos, records.end());
}

IRAM_ATTR void rotary_encoder_interrupt_handler_0(void *arg) {
	static_cast<RotaryEncoder*>(arg)->interrupt_handler(0);
}

IRAM_ATTR void rotary_encoder_interrupt_handler_1(void *arg) {
	static_cast<RotaryEncoder*>(arg)->interrupt_handler(1);
}

IRAM_ATTR void RotaryEncoder::interrupt_handler(int pin_id) {
	bool level = gpio_get_level(pins_[pin_id]);
	uint64_t now_us = esp_timer_get_time();
	auto &signal = signal_[pin_id];

	debug_[debug_pos_] = {
		.pin = (uint32_t)pin_id,
		.level = level,
		.time_us = (uint32_t)now_us,
	};
	debug_pos_ = (debug_pos_ + 1) % debug_.size();

	if (level == signal.level) {
		return;
	}

	signal.level = level;

	bool active = signal.active();

	if (!active) {
		if (signal.active_us && now_us - signal.active_us >= ACTIVE_CHANGE_US) {
			signal.active_us = 0;
			signal.active_low ^= true;
			ESP_ERROR_CHECK(gpio_set_pull_mode(pins_[pin_id],
				signal.active_low ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY));
			active = signal.active();
			count_ = 0;
		}
	}

	if (active) {
		signal.active_us = now_us;
	}

	if (count_ && now_us - start_us_ >= ENCODER_CHANGE_US) {
		count_ = 0;
	}

	switch (count_) {
	case 0:
		if (active) {
			count_++;
			first_ = pin_id;
			start_us_ = now_us;
		}
		break;

	case 1:
		if (active && first_ != pin_id) {
			count_++;
		} else {
			count_ = 0;
		}
		break;

	case 2:
		if (!active && first_ == pin_id) {
			count_++;
		} else {
			count_ = 0;
		}
		break;

	case 3:
		if (!active && first_ != pin_id) {
			count_ = 0;
			update_mode();

			if (first_ == 0) {
				change_.fetch_add(1);
			} else {
				change_.fetch_sub(1);
			}

			wakeup_->wake_up_isr();
			return;
		} else {
			count_ = 0;
		}
		break;

	default:
		count_ = 0;
		break;
	}
}
