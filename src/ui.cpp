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

#include "ui.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "network.h"
#include "util.h"

UI::UI(Network &network) : network_(network) {
}

void UI::startup_complete(bool state) {
	startup_complete_ = state;

	if (startup_complete_) {
		publish_uptime();
	}
}

void UI::publish_uptime() {
	network_.publish(std::string{MQTT_TOPIC} + "/uptime_us",
		std::to_string(esp_timer_get_time()));
	last_publish_us_ = esp_timer_get_time();
}

void UI::setup() {
	led_.begin();
}

void UI::loop() {
	if (!last_led_us_ || esp_timer_get_time() - last_led_us_ >= ONE_M) {
		led_.show();
		last_led_us_ = esp_timer_get_time();
	}

	if (startup_complete_ && network_.connected()) {
		if (!last_publish_us_ || esp_timer_get_time() - last_publish_us_ >= ONE_M) {
			publish_uptime();
		}
	}
}
