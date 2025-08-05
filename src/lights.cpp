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

#include "lights.h"

#include <Arduino.h>
#include <esp_crc.h>
#include <esp_timer.h>

#include <array>
#include <bitset>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include "config.h"
#include "dali.h"
#include "network.h"
#include "util.h"

RTC_NOINIT_ATTR uint32_t Lights::rtc_levels_[RTC_LEVELS_SIZE];
RTC_NOINIT_ATTR uint32_t Lights::rtc_crc_;

Lights::Lights(Network &network, const Config &config)
		: network_(network), config_(config) {
	levels_.fill(0xFFU);
	active_presets_.fill(RESERVED_PRESET_UNKNOWN);
	republish_presets_.insert(BUILTIN_PRESET_OFF);
	republish_presets_.insert(RESERVED_PRESET_CUSTOM);
	load_rtc_state();
}

void Lights::loop() {
	if (startup_complete_ && network_.connected()) {
		publish_levels(false);
		publish_active_presets();
	}
}

void Lights::startup_complete(bool state) {
	std::lock_guard lock{publish_mutex_};

	startup_complete_ = state;
}

void Lights::address_config_changed() {
	std::lock_guard lock{publish_mutex_};
	auto groups = config_.group_names();

	republish_groups_.insert(groups.begin(), groups.end());
}

void Lights::address_config_changed(const std::string &group) {
	std::lock_guard lock{publish_mutex_};

	republish_groups_.insert(group);
}

std::array<uint8_t,MAX_ADDR+1> Lights::get_levels() const {
	std::lock_guard lock{lights_mutex_};

	return levels_;
}

bool Lights::is_idle() {
	return esp_timer_get_time() - last_activity_us_ >= IDLE_US;
}

uint32_t Lights::rtc_crc(const std::array<uint32_t,RTC_LEVELS_SIZE> &levels) {
	return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&levels), sizeof(levels));
}

void Lights::load_rtc_state() {
	if (esp_reset_reason() == ESP_RST_POWERON) {
		ESP_LOGE(TAG, "Ignoring light levels in RTC memory, first power on");
		return;
	}

	std::array<uint32_t,RTC_LEVELS_SIZE> levels{};

	for (unsigned int i = 0; i < RTC_LEVELS_SIZE; i++) {
		levels[i] = rtc_levels_[i];
	}

	uint32_t expected_crc = rtc_crc(levels);

	if (rtc_crc_ == expected_crc) {
		ESP_LOGE(TAG, "Restoring light levels from RTC memory");

		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			levels_[i] = (levels[i/4] >> (8 * (i % 4))) & 0xFFU;
		}
	} else {
		ESP_LOGE(TAG, "Ignoring light levels in RTC memory, checksum mismatch 0x%08X != 0x%08X",
			rtc_crc_, expected_crc);
	}
}

void Lights::save_rtc_state() {
	std::array<uint32_t,RTC_LEVELS_SIZE> levels{};

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		levels[i/4] |= levels_[i] << (8 * (i % 4));
	}

	for (unsigned int i = 0; i < levels.size(); i++) {
		rtc_levels_[i] = levels[i];
	}

	rtc_crc_ = rtc_crc(levels);
}

void Lights::select_preset(std::string name, const std::string &lights, bool internal) {
	const auto addresses = config_.get_addresses();
	bool idle_only;
	const auto light_ids = config_.parse_light_ids(lights, idle_only);
	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	std::array<int16_t,MAX_ADDR+1> preset_levels;
	bool changed = false;

	if (name.empty()) {
		return;
	}

	errno = 0;
	char *endptr = nullptr;
	unsigned long long value = std::strtoull(name.c_str(), &endptr, 10);

	if (endptr && !endptr[0] && !errno) {
		if (!config_.get_ordered_preset(value, name)) {
			return;
		}
	}

	if (!config_.get_preset(name, preset_levels)) {
		return;
	}

	if (!internal && idle_only && !is_idle()) {
		network_.report(TAG, config_.lights_text(light_ids) + " = " + name + " (ignored - not idle)");
		return;
	}

	for (int i = 0; i < MAX_ADDR; i++) {
		if (addresses[i]) {
			if (preset_levels[i] != -1) {
				if (light_ids.find(i) != light_ids.end()) {
					levels_[i] = preset_levels[i];
					republish_presets_.insert(active_presets_[i]);
					active_presets_[i] = name;
					republish_presets_.insert(active_presets_[i]);
					changed = true;
				}
			}
		} else {
			if (!active_presets_[i].empty()) {
				republish_presets_.insert(active_presets_[i]);
				active_presets_[i] = "";
			}
		}
	}

	if (!idle_only) {
		last_activity_us_ = esp_timer_get_time();
	}

	if (changed) {
		save_rtc_state();

		if (!internal) {
			network_.report(TAG, config_.lights_text(light_ids) + " = " + name + (idle_only ? " (idle only)" : ""));
		}

		publish_levels(true);
	}
}

void Lights::set_level(const std::string &lights, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	const auto addresses = config_.get_addresses();
	bool idle_only;
	const auto light_ids = config_.parse_light_ids(lights, idle_only);
	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	bool changed = false;

	if (idle_only && !is_idle()) {
		return;
	}

	for (int light_id : light_ids) {
		if (!addresses[light_id]) {
			continue;
		}

		levels_[light_id] = level;
		republish_presets_.insert(active_presets_[light_id]);
		active_presets_[light_id] = RESERVED_PRESET_CUSTOM;
		republish_presets_.insert(active_presets_[light_id]);
		changed = true;
	}

	last_activity_us_ = esp_timer_get_time();

	if (!changed) {
		return;
	}

	save_rtc_state();
	network_.report(TAG, config_.lights_text(light_ids) + " = " + std::to_string(level));
	publish_levels(true);
}

void Lights::set_power(const std::bitset<MAX_ADDR+1> &lights, bool on) {
	std::lock_guard lock{lights_mutex_};

	power_known_ |= lights;

	if (on) {
		power_on_ |= lights;
	} else {
		power_on_ &= ~lights;
	}
}

void Lights::publish_active_presets() {
	std::lock_guard publish_lock{publish_mutex_};
	bool force = (!last_publish_active_us_ || esp_timer_get_time() - last_publish_active_us_ >= ONE_M);

	if (!force && republish_groups_.empty() && republish_presets_.empty()) {
		return;
	}

	const auto groups = config_.group_names();
	const auto presets = config_.preset_names();
	size_t i = 0;

	for (const auto &group : groups) {
		const auto lights = config_.get_group_addresses(group);
		bool republish_group = republish_groups_.find(group) != republish_groups_.end();

		for (const auto &preset : presets) {
			bool republish_preset = republish_presets_.find(preset) != republish_presets_.end();

			if (republish_group || republish_preset
					|| (force && i >= publish_index_
						&& i < publish_index_ + REPUBLISH_PER_PERIOD)) {
				bool is_active = false;

				for (unsigned int j = 0; j <= MAX_ADDR; j++) {
					if (lights[j] && active_presets_[j] == preset) {
						is_active = true;
						break;
					}
				}

				network_.publish(std::string{MQTT_TOPIC} + "/active/"
					+ group + "/" + preset,
					is_active ? "1" : "0", true);
			}

			i++;
		}
	}

	republish_groups_.clear();
	republish_presets_.clear();

	if (force) {
		/*
		 * Republish only one of the groups every time because the total message
		 * count can get very high (groups * presets).
		 */
		publish_index_ += REPUBLISH_PER_PERIOD;
		publish_index_ %= groups.size() * presets.size();
		last_publish_active_us_ = esp_timer_get_time();
	}
}

void Lights::publish_levels(bool force) {
	if (!force && last_publish_levels_us_
			&& esp_timer_get_time() - last_publish_active_us_ < ONE_M) {
		return;
	}

	std::lock_guard publish_lock{lights_mutex_};
	const auto addresses = config_.get_addresses();
	std::vector<char> buffer(3 * (MAX_ADDR + 1) + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		unsigned int value = (levels_[i] & 0xFFU);

		if (addresses[i]) {
			value |= LEVEL_PRESENT;
		}

		if (power_known_[i]) {
			value |= power_on_[i] ? LEVEL_POWER_ON : LEVEL_POWER_OFF;
		}

		snprintf(&buffer[offset], 4, "%03X", value);
		offset += 3;
	}

	network_.publish(std::string{MQTT_TOPIC} + "/levels",
		{buffer.data(), offset}, true);
	if (!force) {
		network_.publish(std::string{MQTT_TOPIC} + "/idle_us",
			std::to_string(esp_timer_get_time() - last_activity_us_));
	}
	last_publish_levels_us_ = esp_timer_get_time();
}
