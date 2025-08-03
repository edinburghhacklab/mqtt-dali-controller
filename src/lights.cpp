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

#include "lights.h"

#include <Arduino.h>
#include <esp_timer.h>

#include <array>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include "config.h"
#include "dali.h"
#include "network.h"
#include "util.h"

Lights::Lights(Network &network, Config &config)
		: network_(network), config_(config) {
	active_presets_.fill(RESERVED_PRESET_UNKNOWN);
	republish_presets_.insert(BUILTIN_PRESET_OFF);
	republish_presets_.insert(RESERVED_PRESET_CUSTOM);
	republish_presets_.insert(RESERVED_PRESET_UNKNOWN);
}

void Lights::loop() {
	if (startup_complete_ && network_.connected()) {
		publish_active_presets();
	}
}

void Lights::startup_complete(bool state) {
	startup_complete_ = state;
}

void Lights::address_config_changed() {
	republish_groups_ = config_.group_names();
}

void Lights::address_config_changed(const std::string &group) {
	republish_groups_.insert(group);
}

std::array<uint8_t,MAX_ADDR+1> Lights::get_levels() const {
	return levels_;
}

void Lights::select_preset(const std::string &name, const std::string &lights, bool internal) {
	const auto addresses = config_.get_addresses();
	const auto light_ids = config_.parse_light_ids(lights);
	std::array<int16_t,MAX_ADDR+1> preset_levels;
	bool changed = false;

	if (!config_.get_preset(name, preset_levels)) {
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
			levels_[i] = 0;
			if (!active_presets_[i].empty()) {
				republish_presets_.insert(active_presets_[i]);
				active_presets_[i] = "";
			}
		}
	}

	if (changed && !internal) {
		network_.report("lights", config_.lights_text(light_ids) + " = " + name);
	}
}

void Lights::set_level(const std::string &lights, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	const auto addresses = config_.get_addresses();
	const auto light_ids = config_.parse_light_ids(lights);
	bool changed = false;

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

	if (!changed) {
		return;
	}

	network_.report("lights", config_.lights_text(light_ids) + " = " + std::to_string(level));
}

void Lights::publish_active_presets() {
	bool force = (!last_publish_us_ || esp_timer_get_time() - last_publish_us_ >= ONE_M);

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

			if (republish_group || republish_preset || (force && publish_index_ == i)) {
				bool is_active = false;

				for (unsigned int i = 0; i <= MAX_ADDR; i++) {
					if (lights[i] && active_presets_[i] == preset) {
						is_active = true;
						break;
					}
				}

				network_.publish(std::string{MQTT_TOPIC} + "/active/"
					+ group + "/" + preset,
					is_active ? "1" : "0", true);
			}
		}

		i++;
	}

	republish_groups_.clear();
	republish_presets_.clear();

	if (force) {
		/*
		 * Republish only one of the groups every time because the total message
		 * count can get very high (groups * presets).
		 */
		publish_index_++;
		publish_index_ %= groups.size();
		last_publish_us_ = esp_timer_get_time();
	}
}
