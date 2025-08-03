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

#include "switches.h"

#include <Arduino.h>
#include <esp_timer.h>

#include <array>
#include <string>

#include "config.h"
#include "lights.h"
#include "network.h"
#include "util.h"

static constexpr std::array<unsigned int,NUM_SWITCHES> SWITCH_GPIO = {11, 12};

Switches::Switches(Network &network, Config &config, Lights &lights)
		: network_(network), config_(config), lights_(lights) {
}

void Switches::setup() {
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		pinMode(SWITCH_GPIO[i], INPUT_PULLUP);
	}
}

void Switches::loop() {
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		auto group = config_.get_switch_group(i);
		auto preset = config_.get_switch_preset(i);
		int switch_value = (preset.empty() || group.empty())
			? LOW : digitalRead(SWITCH_GPIO[i]);

		if (switch_value != state_[i].value) {
			state_[i].value = switch_value;

			std::string name = config_.get_switch_name(i);

			if (name.empty()) {
				name = "Light switch ";
				name += std::to_string(i);
			}

			if (network_.connected()) {
				network_.publish(std::string{MQTT_TOPIC}
					+ "/switch/" + std::to_string(i) + "/state",
					state_[i].value == LOW ? "1" : "0",
					true);
			}
			state_[i].report_us = esp_timer_get_time();

			network_.report("switch", name + " "
				+ (state_[i].value == LOW ? "ON" : "OFF")
				+ " (levels reset to " + preset + ")");

			lights_.select_preset(preset, group, true);
		} else if (state_[i].report_us
				&& esp_timer_get_time() - state_[i].report_us >= ONE_M) {
			network_.publish(std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i) + "/state",
					state_[i].value == LOW ? "1" : "0",
					true);
			state_[i].report_us = esp_timer_get_time();
		}
	}
}
