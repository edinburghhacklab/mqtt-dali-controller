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

#include "selector.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_log.h>

#include <array>

static constexpr std::array<gpio_num_t,2> SELECTOR_GPIO{{
	(gpio_num_t)16,
	(gpio_num_t)17,
}};

Selector::Selector() : pins_(SELECTOR_GPIO) {
}

void Selector::setup() {
	gpio_config_t config{};

	config.pin_bit_mask = (1ULL << pins_[0]) | (1ULL << pins_[1]);
	config.mode = GPIO_MODE_INPUT;
	config.pull_up_en = GPIO_PULLUP_ENABLE;
	config.pull_down_en = GPIO_PULLDOWN_DISABLE;
	config.intr_type = GPIO_INTR_DISABLE;

	ESP_ERROR_CHECK(gpio_config(&config));
}

int Selector::read() const {
	int value = 0;

	for (unsigned int i = 0; i < pins_.size(); i++) {
		if (gpio_get_level(pins_[i]) == 0) {
			value |= 1 << i;
		}
	}

	return value;
}
