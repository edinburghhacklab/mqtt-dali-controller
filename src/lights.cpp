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

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "dali.h"
#include "network.h"
#include "util.h"

RTC_NOINIT_ATTR uint32_t Lights::rtc_levels_[RTC_LEVELS_SIZE];
RTC_NOINIT_ATTR uint32_t Lights::rtc_crc_;

Lights::Lights(Network &network, const Config &config)
		: network_(network), config_(config) {
	levels_.fill(LEVEL_NO_CHANGE);
	active_presets_.fill(RESERVED_PRESET_UNKNOWN);
	republish_presets_.insert(BUILTIN_PRESET_OFF);
	republish_presets_.insert(RESERVED_PRESET_CUSTOM);
}

void Lights::setup() {
	load_rtc_state();
}

void Lights::set_dali(Dali &dali) {
	dali_ = &dali;
}

void Lights::loop() {
	if (startup_complete_ && network_.connected()) {
		std::bitset<MAX_ADDR+1> lights;

		lights.set();

		report_dimmed_levels(lights, DIM_REPORT_DELAY_US);
		publish_levels(false);
		publish_active_presets();
	}
}

void Lights::startup_complete(bool state) {
	std::lock_guard lock{publish_mutex_};

	startup_complete_ = state;
}

std::string Lights::rtc_boot_memory() {
	std::vector<char> buffer(64);

	snprintf(buffer.data(), buffer.size(), "%p+%zu, %p+%zu",
		&rtc_crc_, sizeof(rtc_crc_),
		rtc_levels_, sizeof(rtc_levels_));

	return {buffer.data()};
}

BootRTCStatus Lights::rtc_boot_status() const {
	return boot_rtc_;
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
	return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&levels), sizeof(levels)) ^ RTC_MAGIC;
}

void Lights::load_rtc_state() {
	ESP_LOGE(TAG, "RTC state at %s", rtc_boot_memory().c_str());

	if (esp_reset_reason() == ESP_RST_POWERON) {
		ESP_LOGE(TAG, "Ignoring light levels in RTC memory, first power on");
		boot_rtc_ = BootRTCStatus::POWER_ON_IGNORED;
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

		boot_rtc_ = BootRTCStatus::LOADED_OK;
	} else {
		ESP_LOGE(TAG, "Ignoring light levels in RTC memory, checksum mismatch 0x%08X != 0x%08X",
			rtc_crc_, expected_crc);
		boot_rtc_ = BootRTCStatus::CHECKSUM_MISMATCH;
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

void Lights::select_preset(std::string name, const std::string &light_ids, bool internal) {
	const auto addresses = config_.get_addresses();
	bool idle_only;
	const auto lights = config_.parse_light_ids(light_ids, idle_only);
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
		network_.report(TAG, config_.lights_text(lights) + " = " + name + " (ignored - not idle)");
		return;
	}

	report_dimmed_levels(lights, 0);

	for (int i = 0; i <= MAX_ADDR; i++) {
		if (addresses[i]) {
			if (preset_levels[i] != -1) {
				if (lights[i]) {
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

		if (dali_) {
			dali_->wake_up();
		}

		if (!internal) {
			network_.report(TAG, config_.lights_text(lights) + " = " + name + (idle_only ? " (idle only)" : ""));
		}

		publish_levels(true);
	}
}

void Lights::set_level(const std::string &light_ids, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	const auto addresses = config_.get_addresses();
	bool idle_only;
	const auto lights = config_.parse_light_ids(light_ids, idle_only);
	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	bool changed = false;

	if (idle_only && !is_idle()) {
		return;
	}

	report_dimmed_levels(lights, 0);

	for (int i = 0; i <= MAX_ADDR; i++) {
		if (!addresses[i] || !lights[i]) {
			continue;
		}

		levels_[i] = level;
		republish_presets_.insert(active_presets_[i]);
		active_presets_[i] = RESERVED_PRESET_CUSTOM;
		republish_presets_.insert(active_presets_[i]);
		changed = true;
	}

	last_activity_us_ = esp_timer_get_time();

	if (changed) {
		save_rtc_state();

		if (dali_) {
			dali_->wake_up();
		}

		network_.report(TAG, config_.lights_text(lights) + " = " + std::to_string(level));
		publish_levels(true);
	}
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

void Lights::dim_adjust(const std::string &group, long level) {
	if (level < -(long)MAX_LEVEL || level > (long)MAX_LEVEL) {
		return;
	}

	const auto addresses = config_.get_addresses();
	const auto lights = config_.get_group_addresses(group);
	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	uint64_t now = esp_timer_get_time();
	bool changed;

	for (int i = 0; i <= MAX_ADDR; i++) {
		if (!addresses[i] || !lights[i]) {
			continue;
		}

		levels_[i] = std::max(0L, std::min((long)MAX_LEVEL, (long)levels_[i] + level));
		dim_time_us_[i] = now;
		republish_presets_.insert(active_presets_[i]);
		active_presets_[i] = RESERVED_PRESET_CUSTOM;
		republish_presets_.insert(active_presets_[i]);
		changed = true;
	}

	last_activity_us_ = esp_timer_get_time();

	if (changed) {
		save_rtc_state();

		if (dali_) {
			dali_->wake_up();
		}

		publish_levels(true);
	}
}

void Lights::report_dimmed_levels(const std::bitset<MAX_ADDR+1> &lights, uint64_t time_us) {
	std::lock_guard lock{lights_mutex_};
	std::bitset<MAX_ADDR+1> dimmed_lights;
	uint8_t min_level = MAX_LEVEL;
	uint8_t max_level = 0;
	uint64_t now = esp_timer_get_time();

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (lights[i] && dim_time_us_[i] && now - dim_time_us_[i] >= time_us) {
			dimmed_lights[i] = true;
			min_level = std::min(min_level, levels_[i]);
			max_level = std::max(max_level, levels_[i]);
			dim_time_us_[i] = 0;
		}
	}

	if (dimmed_lights.count()) {
		if (min_level == max_level) {
			network_.report(TAG, config_.lights_text(dimmed_lights) + " = "
				+ std::to_string(min_level) + " (dimmer)");
		} else {
			network_.report(TAG, config_.lights_text(dimmed_lights) + " = "
				+ std::to_string(min_level) + ".." + std::to_string(max_level) + " (dimmer)");
		}
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

	std::lock_guard lock{lights_mutex_};
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
