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

#include "buttons.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <array>
#include <string>

#include "config.h"
#include "lights.h"
#include "util.h"

static constexpr std::array<gpio_num_t,NUM_BUTTONS> BUTTON_GPIO{{
	(gpio_num_t)18,
	(gpio_num_t)39,
	(gpio_num_t)41,
	(gpio_num_t)42,
}};

Buttons::Buttons(const Config &config, Lights &lights)
		: WakeupThread("buttons", true), config_(config), lights_(lights),
		debounce_({
			Debounce{BUTTON_GPIO[0], true, DEBOUNCE_US},
			Debounce{BUTTON_GPIO[1], true, DEBOUNCE_US},
			Debounce{BUTTON_GPIO[2], true, DEBOUNCE_US},
			Debounce{BUTTON_GPIO[3], true, DEBOUNCE_US},
		}) {
}

void Buttons::setup() {
	for (unsigned int i = 0; i < NUM_BUTTONS; i++) {
		debounce_[i].start(*this);
	}

	std::thread t;
	make_thread(t, "buttons", 8192, 1, 20, &Buttons::run_loop, this);
	t.detach();
}

unsigned long Buttons::run_tasks() {
	unsigned long wait_ms = WATCHDOG_INTERVAL_MS;

	esp_task_wdt_reset();

	for (unsigned int i = 0; i < NUM_BUTTONS; i++) {
		wait_ms = std::min(wait_ms, run_button(i));
	}

	return wait_ms;
}

unsigned long Buttons::run_button(unsigned int button_id) {
	DebounceResult debounce = debounce_[button_id].run();

	if (debounce.changed && !debounce_[button_id].first()
			&& debounce_[button_id].value()) {
		auto groups = config_.button_active_groups(button_id);
		auto preset = config_.get_button_preset(button_id);

		ESP_LOGE(TAG, "Button %u pressed", button_id);

		if (!groups.empty() && !preset.empty()) {
			lights_.select_preset(preset, groups, false);
		}
	}

	return debounce.wait_ms;
}
