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

#include <Arduino.h>
#include <CBOR.h>
#include <CBOR_parsing.h>
#include <CBOR_streams.h>

#include <array>
#include <bitset>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dali.h"
#include "switches.h"

static const std::string BUILTIN_GROUP_ALL = "all";
static const std::string BUILTIN_PRESET_OFF = "off";

namespace cbor = qindesign::cbor;

class Network;

struct ConfigSwitchData {
	std::string name;
	std::string group;
	std::string preset;

	bool operator==(const ConfigSwitchData &other) const {
		return this->name == other.name
			&& this->group == other.group
			&& this->preset == other.preset;
	}

	inline bool operator!=(const ConfigSwitchData &other) const { return !(*this == other); }
};

struct ConfigData {
	std::bitset<MAX_ADDR+1> lights;
	std::array<ConfigSwitchData,NUM_SWITCHES> switches;
	std::unordered_map<std::string,std::bitset<MAX_ADDR+1>> groups;
	std::unordered_map<std::string,std::array<int16_t,MAX_ADDR+1>> presets;

	bool operator==(const ConfigData &other) const {
		return this->lights == other.lights
			&& this->switches == other.switches
			&& this->groups == other.groups
			&& this->presets == other.presets;
	}

	inline bool operator!=(const ConfigData &other) const { return !(*this == other); }
};

class ConfigFile {
public:
	ConfigFile(Network &network);

	bool read_config(ConfigData &data);
	bool write_config(const ConfigData &data);

private:
	bool read_config(const std::string &filename, bool load);
	bool read_config(cbor::Reader &reader);
	bool read_config_lights(cbor::Reader &reader, std::bitset<MAX_ADDR+1> &lights);
	bool read_config_groups(cbor::Reader &reader);
	bool read_config_group(cbor::Reader &reader);
	bool read_config_switches(cbor::Reader &reader);
	bool read_config_switch(cbor::Reader &reader, unsigned int switch_id);
	bool read_config_presets(cbor::Reader &reader);
	bool read_config_preset(cbor::Reader &reader);
	bool read_config_preset_levels(cbor::Reader &reader, std::array<int16_t,MAX_ADDR+1> &levels);

	void write_config(cbor::Writer &writer);
	bool write_config(const std::string &filename);

	Network &network_;
	ConfigData data_;
};

class Config {
public:
	explicit Config(Network &network);

	static bool valid_group_name(const std::string &name);
	static bool valid_preset_name(const std::string &name);
	static std::string addresses_text(const std::bitset<MAX_ADDR+1> &addresses);
	static std::string preset_levels_text(const std::array<int16_t,MAX_ADDR+1> &levels,
		const std::bitset<MAX_ADDR+1> *filter);

	void setup();
	void loop();
	void load_config();
	void save_config();
	void publish_config();

	std::bitset<MAX_ADDR+1> get_addresses();
	void set_addresses(const std::string &addresses);
	std::string addresses_text();

	std::unordered_set<std::string> group_names();
	std::bitset<MAX_ADDR+1> get_group_addresses(const std::string &name);
	void set_group_addresses(const std::string &name, const std::string &addresses);
	std::string group_addresses_text(const std::string &name);
	void delete_group(const std::string &name);

	std::string get_switch_name(unsigned int switch_id);
	void set_switch_name(unsigned int switch_id, const std::string &name);

	std::string get_switch_group(unsigned int switch_id);
	void set_switch_group(unsigned int switch_id, const std::string &name);

	std::string get_switch_preset(unsigned int switch_id);
	void set_switch_preset(unsigned int switch_id, const std::string &preset);

	std::unordered_set<std::string> preset_names();
	bool get_preset(const std::string &name, std::array<int16_t,MAX_ADDR+1> &levels);
	void set_preset(const std::string &name, const std::string &lights, long level);
	void set_preset(const std::string &name, std::string levels);
	void delete_preset(const std::string &name);

	std::set<unsigned int> parse_light_ids(const std::string &light_id);
	std::string lights_text(const std::set<unsigned int> &light_ids);

private:
	static constexpr size_t MAX_GROUPS = 10;
	static constexpr size_t MAX_GROUP_NAME_LEN = 50;
	static constexpr size_t MAX_PRESETS = 10;
	static constexpr size_t MAX_PRESET_NAME_LEN = 50;
	static constexpr size_t MAX_SWITCH_NAME_LEN = 50;

	void dirty_config();
	void set_addresses(const std::string &group, std::string addresses);
	void publish_preset(const std::string &name, const std::array<int16_t,MAX_ADDR+1> &levels);

	Network &network_;
	ConfigFile file_;
	ConfigData current_;
	ConfigData last_saved_;
	bool dirty_{false};
	bool saved_{false};
};
