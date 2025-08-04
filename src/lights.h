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

#pragma once

#include <array>
#include <bitset>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dali.h"

static const std::string RESERVED_PRESET_CUSTOM = "custom";
static const std::string RESERVED_PRESET_UNKNOWN = "unknown";

class Config;
class Network;

class Lights {
public:
	Lights(Network &network, const Config &config);

	void loop();
	void startup_complete(bool state);
	void address_config_changed();
	void address_config_changed(const std::string &group);

	std::array<uint8_t,MAX_ADDR+1> get_levels() const;
	void select_preset(const std::string &name, const std::string &lights, bool internal = false);
	void set_level(const std::string &lights, long level);
	void set_power(const std::bitset<MAX_ADDR+1> &lights, bool on);

private:
	static constexpr size_t REPUBLISH_PER_PERIOD = 5;
	static constexpr unsigned int LEVEL_PRESENT = (1U << 8);
	static constexpr unsigned int LEVEL_POWER_ON = (1U << 9);
	static constexpr unsigned int LEVEL_POWER_OFF = (1U << 10);

	void publish_active_presets();
	void publish_levels(bool force);

	Network &network_;
	const Config &config_;

	mutable std::recursive_mutex lights_mutex_;
	std::array<uint8_t,MAX_ADDR+1> levels_{};
	std::bitset<MAX_ADDR+1> power_on_;
	std::bitset<MAX_ADDR+1> power_known_;
	uint64_t last_publish_levels_us_{0};
	uint64_t last_activity_us_{0};

	std::mutex publish_mutex_;
	bool startup_complete_{false};
	std::array<std::string,MAX_ADDR+1> active_presets_{};
	std::unordered_set<std::string> republish_groups_;
	std::unordered_set<std::string> republish_presets_;
	uint64_t last_publish_active_us_{0};
	size_t publish_index_{0};
};
