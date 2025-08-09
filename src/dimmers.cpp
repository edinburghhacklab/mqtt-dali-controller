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

#include "dimmers.h"

#include <Arduino.h>
#include <esp_crc.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <array>
#include <string>

#include "config.h"
#include "lights.h"
#include "util.h"

static constexpr std::array<std::array<gpio_num_t,2>,NUM_DIMMERS> DIMMER_GPIO{{
    {(gpio_num_t)1, (gpio_num_t)2},
    {(gpio_num_t)3, (gpio_num_t)4},
    {(gpio_num_t)5, (gpio_num_t)6},
    {(gpio_num_t)7, (gpio_num_t)8},
    {(gpio_num_t)9, (gpio_num_t)10},
}};

Dimmers::Dimmers(const Config &config, Lights &lights)
		: WakeupThread("dimmers", true), config_(config),
		lights_(lights), encoder_({
			RotaryEncoder{DIMMER_GPIO[0]},
			RotaryEncoder{DIMMER_GPIO[1]},
			RotaryEncoder{DIMMER_GPIO[2]},
			RotaryEncoder{DIMMER_GPIO[3]},
			RotaryEncoder{DIMMER_GPIO[4]},
		}) {
}

void Dimmers::setup() {
	for (unsigned int i = 0; i < NUM_DIMMERS; i++) {
		encoder_[i].start(*this);
	}

	std::thread t;
	make_thread(t, "dimmers", 8192, 1, 20, &Dimmers::run_loop, this);
	t.detach();
}

unsigned long Dimmers::run_tasks() {
	unsigned long wait_ms = WATCHDOG_INTERVAL_MS;

	esp_task_wdt_reset();

	for (unsigned int i = 0; i < NUM_DIMMERS; i++) {
		wait_ms = std::min(wait_ms, run_dimmer(i));
	}

	return wait_ms;
}

unsigned long Dimmers::run_dimmer(unsigned int dimmer_id) {
	auto group = config_.get_dimmer_group(dimmer_id);

	return ULONG_MAX;
}
