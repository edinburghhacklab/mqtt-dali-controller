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

#include "config.h"
#include "dali.h"
#include "network.h"
#include "util.h"

#if __has_include("fixed_config.h")
# include "fixed_config.h"
#else
# include "fixed_config.h.example"
#endif

Lights::Lights(Network &network, Config &config)
        : network_(network), config_(config) {
    active_presets_.fill(RESERVED_PRESET_UNKNOWN);
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
    republish_active_presets_ = true;
}

std::array<uint8_t,MAX_ADDR+1> Lights::get_levels() const {
    return levels_;
}

void Lights::select_preset(const std::string &name, std::bitset<MAX_ADDR+1> *filter) {
	const auto lights = config_.get_addresses();
	std::array<int16_t,MAX_ADDR+1> preset_levels;

	if (!config_.get_preset(name, preset_levels)) {
		return;
	}

	if (!filter) {
		network_.report("lights", std::string{"Preset = "} + name);
	}

	for (int i = 0; i < MAX_ADDR; i++) {
		if (lights[i]) {
			if (preset_levels[i] != -1) {
				if (filter == nullptr || filter->test(i)) {
					levels_[i] = preset_levels[i];
					active_presets_[i] = name;
					republish_active_presets_ = true;
				}
			}
		} else {
			levels_[i] = 0;
			if (!active_presets_[i].empty()) {
				active_presets_[i] = "";
			}
		}
	}
}

void Lights::set_level(const std::string &lights, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	const auto addresses = config_.get_addresses();
	const auto light_ids = Config::parse_light_ids(lights);
	unsigned int changed = 0;

	for (int light_id : light_ids) {
		if (!addresses[light_id]) {
			continue;
		}

		levels_[light_id] = level;
		active_presets_[light_id] = RESERVED_PRESET_CUSTOM;
		republish_active_presets_ = true;
		changed++;
	}

	if (!changed) {
		return;
	}

	network_.report("lights", config_.lights_text(light_ids) + " = " + std::to_string(level));
}

void Lights::publish_active_presets() {
	bool force = !last_published_active_presets_us_
			|| esp_timer_get_time() - last_published_active_presets_us_ >= ONE_M;

	if (!force && !republish_active_presets_) {
		return;
	}

	const auto lights = config_.get_addresses();
	const std::unordered_set<std::string> all = config_.preset_names();
	std::unordered_set<std::string> active;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (lights[i]) {
			active.insert(active_presets_[i]);
		}
	}

	for (const auto &preset : all) {
		bool is_active = active.find(preset) != active.end();
		bool last_active = last_active_presets_.find(preset) != last_active_presets_.end();

		if (force || (is_active != last_active)) {
			network_.publish(std::string{MQTT_TOPIC} + "/preset/" + preset + "/active", is_active ? "1" : "0", true);
		}

		if (is_active) {
			last_active_presets_.insert(preset);
		} else {
			last_active_presets_.erase(preset);
		}
	}

	if (force) {
		last_published_active_presets_us_ = esp_timer_get_time();
	}
}
