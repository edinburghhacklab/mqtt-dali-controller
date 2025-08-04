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

#include "switches.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <string>

#include "config.h"
#include "lights.h"
#include "network.h"
#include "util.h"

static constexpr std::array<unsigned int,NUM_SWITCHES> SWITCH_GPIO = {11, 12};

Switches::Switches(Network &network, const Config &config, Lights &lights)
		: WakeupThread("switches", true), network_(network),
		config_(config), lights_(lights), debounce_({
			Debounce{(gpio_num_t)SWITCH_GPIO[0], true, DEBOUNCE_US},
			Debounce{(gpio_num_t)SWITCH_GPIO[1], true, DEBOUNCE_US},
		}) {
}

void Switches::setup() {
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		debounce_[i].start(*this);
	}

	std::thread t;
	make_thread(t, "switches", 4096, 19, &Switches::run_loop, this);
	t.detach();
}

unsigned long Switches::run_tasks() {
	unsigned long wait_ms = WATCHDOG_INTERVAL_MS;

	esp_task_wdt_reset();

	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		wait_ms = std::min(wait_ms, run_switch(i));
	}

	return wait_ms;
}

unsigned long Switches::run_switch(unsigned int switch_id) {
	auto group = config_.get_switch_group(switch_id);
	auto preset = config_.get_switch_preset(switch_id);
	DebounceResult debounce = debounce_[switch_id].run();

	if (debounce.changed) {
		state_[switch_id].active = debounce_[switch_id].value();
		ESP_LOGE(TAG, "Switch %u turned %s", switch_id,
			state_[switch_id].active ? "on" : "off");

		network_.publish(std::string{MQTT_TOPIC}
			+ "/switch/" + std::to_string(switch_id) + "/state",
			state_[switch_id].active ? "1" : "0", true);
		state_[switch_id].report_us = esp_timer_get_time();

		if (!group.empty()) {
			lights_.set_power(config_.get_group_addresses(group),
				state_[switch_id].active);
		}

		if (!debounce_[switch_id].first()
				&& !group.empty() && !preset.empty()) {
			std::string name = config_.get_switch_name(switch_id);

			if (name.empty()) {
				name = "Light switch ";
				name += std::to_string(switch_id);
			}

			network_.report(TAG, name + " "
				+ (debounce_[switch_id].value() ? "ON" : "OFF")
				+ " (levels reset to " + preset + ")");

			lights_.select_preset(preset, group, true);
		}
	} else if (state_[switch_id].report_us
			&& esp_timer_get_time() - state_[switch_id].report_us >= ONE_M) {
		if (!group.empty()) {
			lights_.set_power(config_.get_group_addresses(group),
				state_[switch_id].active);
		}

		network_.publish(std::string{MQTT_TOPIC}
			+ "/switch/" + std::to_string(switch_id) + "/state",
			state_[switch_id].active ? "1" : "0", true);
		state_[switch_id].report_us = esp_timer_get_time();
	}

	return debounce.wait_ms;
}
