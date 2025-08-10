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

#include <atomic>
#include <array>

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
	state_[0] = gpio_get_level(pins_[0]) == 0;
	state_[1] = gpio_get_level(pins_[1]) == 0;
	ESP_ERROR_CHECK(gpio_isr_handler_add(pins_[0], rotary_encoder_interrupt_handler_0, this));
	ESP_ERROR_CHECK(gpio_isr_handler_add(pins_[1], rotary_encoder_interrupt_handler_1, this));
	ESP_ERROR_CHECK(gpio_set_intr_type(pins_[0], GPIO_INTR_ANYEDGE));
	ESP_ERROR_CHECK(gpio_set_intr_type(pins_[1], GPIO_INTR_ANYEDGE));
	ESP_ERROR_CHECK(gpio_intr_enable(pins_[0]));
	ESP_ERROR_CHECK(gpio_intr_enable(pins_[1]));
}

long RotaryEncoder::read() {
	return change_.exchange(0L);
}

void rotary_encoder_interrupt_handler_0(void *arg) {
	static_cast<RotaryEncoder*>(arg)->interrupt_handler(0);
}

void rotary_encoder_interrupt_handler_1(void *arg) {
	static_cast<RotaryEncoder*>(arg)->interrupt_handler(1);
}

void RotaryEncoder::interrupt_handler(int pin_id) {
	bool state = gpio_get_level(pins_[pin_id]) == 0;

	if (state != state_[pin_id]) {
		state_[pin_id] = state;
	}

	if (state) {
		if (first_ == -1) {
			first_ = pin_id;
		}
	} else {
		first_ = -1;
		return;
	}

	if (!state_[0] || !state_[1]) {
		return;
	}

	if (first_ == 0) {
		change_.fetch_add(1);
	} else {
		change_.fetch_sub(1);
	}

	wakeup_->wake_up_isr();
}
