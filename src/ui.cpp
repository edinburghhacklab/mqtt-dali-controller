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
#include <esp_ota_ops.h>
#include <esp_timer.h>

#include <cstring>
#include <string>

#include "network.h"
#include "util.h"

UI::UI(Network &network) : network_(network) {
}

void UI::startup_complete(bool state) {
	startup_complete_ = state;

	if (startup_complete_) {
		status_report();
	}
}

template<typename T, size_t size>
static inline std::string null_terminated_string(T(&data)[size]) {
	T *found = reinterpret_cast<T*>(std::memchr(&data[0], '\0', size));
	return std::string{&data[0], found ? (found - &data[0]) : size};
};

static const char *ota_state_string(esp_ota_img_states_t state) {
	switch (state) {
	case ESP_OTA_IMG_NEW: return "new";
	case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
	case ESP_OTA_IMG_VALID: return "valid";
	case ESP_OTA_IMG_INVALID: return "invalid";
	case ESP_OTA_IMG_ABORTED: return "aborted";
	case ESP_OTA_IMG_UNDEFINED: return "undefined";
	}

	return "unknown";
}

void UI::status_report() {
	publish_partitions();
	publish_uptime();
}

void UI::publish_partitions() {
	const esp_partition_t *current = esp_ota_get_running_partition();
	const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
	const esp_partition_t *boot = esp_ota_get_boot_partition();
	const esp_partition_t *part = current;

	if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
		part = esp_ota_get_next_update_partition(part);
	}

	for (int i = 0; i < esp_ota_get_app_partition_count(); i++, part = esp_ota_get_next_update_partition(part)) {
		esp_app_desc_t desc;
		esp_ota_img_states_t state;
		std::string topic = std::string{MQTT_TOPIC} + "/partition/";

		if (esp_ota_get_state_partition(part, &state)) {
			state = ESP_OTA_IMG_UNDEFINED;
		}

		topic += std::to_string(i);

		std::string ota_payload = part->label;
		if (part == current) {
			ota_payload += " [current]";
		}
		if (part == next) {
			ota_payload += " [next]";
		}
		if (part == boot) {
			ota_payload += " [boot]";
		}
		ota_payload += ' ';
		ota_payload += ota_state_string(state);

		network_.publish(topic + "/ota", ota_payload);

		if (!esp_ota_get_partition_description(part, &desc)) {
			network_.publish(topic + "/name", null_terminated_string(desc.project_name));
			network_.publish(topic + "/version", null_terminated_string(desc.version));
			network_.publish(topic + "/timestamp", null_terminated_string(desc.date) + " " + null_terminated_string(desc.time));
		}
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
