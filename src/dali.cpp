/*
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

#include "dali.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"
#include "lights.h"
#include "util.h"

#if __has_include("fixed_config.h")
# include "fixed_config.h"
#else
# include "fixed_config.h.example"
#endif

static constexpr unsigned int RX_GPIO = 40;
static constexpr unsigned int TX_GPIO = 21;

Dali::Dali(Config &config, Lights &lights) : config_(config), lights_(lights) {
}

void Dali::transmit_one(uint8_t address, uint8_t level) {
	if (address > MAX_ADDR || level > MAX_LEVEL) {
		return;
	}

	// TODO
}

void Dali::transmit_all() {
	static uint64_t last_tx_us_ = 0;
	bool repeat = !last_tx_us_ || esp_timer_get_time() - last_tx_us_ >= ONE_S;
	auto levels = lights_.get_levels();

	if (repeat || levels != tx_levels_) {
		const auto lights = config_.get_addresses();

		for (uint8_t i = 0; i <= MAX_ADDR; i++) {
			if (lights[i]) {
				/*
				 * Only transmit changed levels immediately, to improve
				 * responsiveness when dimming with a rotary encoder.
				 */
				if (repeat || levels[i] != tx_levels_[i]) {
					transmit_one(i, levels[i]);
				}
			}
		}

		tx_levels_ = levels;
		if (repeat) {
			last_tx_us_ = esp_timer_get_time();
		}
	}
}

void Dali::setup() {
	pinMode(TX_GPIO, OUTPUT);
	digitalWrite(TX_GPIO, HIGH);
}

void Dali::loop() {
	transmit_all();
}
