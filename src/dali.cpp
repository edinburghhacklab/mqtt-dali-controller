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

#include "dali.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include "config.h"
#include "lights.h"
#include "util.h"

static constexpr unsigned int RX_GPIO = 40;
static constexpr unsigned int TX_GPIO = 21;

Dali::Dali(const Config &config, const Lights &lights)
		: WakeupThread("dali", true), config_(config),
		lights_(lights) {
	tx_levels_.fill(LEVEL_NO_CHANGE);
}

void Dali::setup() {
	pinMode(RX_GPIO, INPUT);
	pinMode(TX_GPIO, OUTPUT);
	digitalWrite(TX_GPIO, BUS_HIGH);

	std::thread t;
	make_thread(t, "dali", 8192, 1, 19, &Dali::run_loop, this);
	t.detach();
}

unsigned long Dali::run_tasks() {
	const auto lights = config_.get_addresses();
	unsigned long delay_ms = std::min(WATCHDOG_INTERVAL_MS,
		std::max(TX_POWER_LEVEL_MS, REFRESH_PERIOD_MS / std::max(1U, lights.count())));
	auto levels = lights_.get_levels();
	bool changed = false;

	/*
	 * Set power level for lights that have changed level, cycling through the
	 * addresses each time to avoid preferring low-numbered lights.
	 */
	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		unsigned address = next_address_;

		next_address_++;
		next_address_ %= MAX_ADDR + 1;

		if (!lights[address]) {
			continue;
		}

		if (levels[address] != tx_levels_[address]) {
			tx_power_level(address, levels[address]);
			tx_levels_[address] = levels[address];
			changed = true;
			esp_task_wdt_reset();
		}
	}

	if (!changed) {
		/*
		* Refresh light power levels individually over a short time period,
		* cycling through the addresses each time to avoid preferring
		* low-numbered lights. Delays between lights keeps the bus idle most of
		* the time to improve responsiveness when dimming with a rotary encoder.
		*/
		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			unsigned address = next_address_;

			next_address_++;
			next_address_ %= MAX_ADDR + 1;

			if (!lights[address]) {
				continue;
			}

			tx_power_level(address, levels[address]);
			tx_levels_[address] = levels[address];
			break;
		}
	}

	esp_task_wdt_reset();
	return delay_ms;
}

void Dali::tx_power_level(uint8_t address, uint8_t level) {
	if (address > MAX_ADDR || level > MAX_LEVEL) {
		return;
	}

	//ESP_LOGE(TAG, "Power level %u = %u", address, level);
}
