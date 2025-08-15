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

#pragma once

#include <Arduino.h>
#include <CBOR.h>
#include <CBOR_parsing.h>
#include <CBOR_streams.h>

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dali.h"
#include "dimmers.h"
#include "switches.h"

static const std::string BUILTIN_GROUP_ALL = "all";
static const std::string BUILTIN_PRESET_OFF = "off";
static const std::string RESERVED_GROUP_DELETE = "delete";
static const std::string RESERVED_GROUP_LEVELS = "levels";
static const std::string RESERVED_PRESET_ORDER = "order";

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

struct ConfigDimmerData {
	std::string group;
	int encoder_steps;
	unsigned int level_steps;

	bool operator==(const ConfigDimmerData &other) const {
		return this->group == other.group
			&& this->encoder_steps == other.encoder_steps
			&& this->level_steps == other.level_steps;
	}

	inline bool operator!=(const ConfigDimmerData &other) const { return !(*this == other); }
};

struct ConfigData {
	Dali::addresses_t lights;
	std::array<ConfigSwitchData,NUM_SWITCHES> switches;
	std::array<ConfigDimmerData,NUM_DIMMERS> dimmers;
	std::unordered_map<std::string,Dali::addresses_t> groups;
	std::unordered_map<std::string,std::array<int16_t,Dali::num_addresses>> presets;
	std::vector<std::string> ordered;

	bool operator==(const ConfigData &other) const {
		return this->lights == other.lights
			&& this->dimmers == other.dimmers
			&& this->switches == other.switches
			&& this->groups == other.groups
			&& this->presets == other.presets
			&& this->ordered == other.ordered;
	}

	inline bool operator!=(const ConfigData &other) const { return !(*this == other); }
};

class ConfigFile {
public:
	explicit ConfigFile(Network &network);

	bool read_config(ConfigData &data);
	bool write_config(const ConfigData &data);

private:
	static constexpr const char *TAG = "ConfigFile";

	bool read_config(const std::string &filename, bool load);
	bool read_config(cbor::Reader &reader);
	bool read_config_lights(cbor::Reader &reader, Dali::addresses_t &lights);
	bool read_config_groups(cbor::Reader &reader);
	bool read_config_group(cbor::Reader &reader);
	bool read_config_switches(cbor::Reader &reader);
	bool read_config_switch(cbor::Reader &reader, unsigned int switch_id);
	bool read_config_dimmers(cbor::Reader &reader);
	bool read_config_dimmer(cbor::Reader &reader, unsigned int dimmer_id);
	bool read_config_presets(cbor::Reader &reader);
	bool read_config_preset(cbor::Reader &reader);
	bool read_config_preset_levels(cbor::Reader &reader, std::array<int16_t,Dali::num_addresses> &levels);
	bool read_config_order(cbor::Reader &reader);

	void write_config(cbor::Writer &writer) const;
	bool write_config(const std::string &filename) const;

	Network &network_;
	ConfigData data_;
};

class Config {
public:
	static constexpr int16_t LEVEL_NO_CHANGE = -1;

	explicit Config(std::mutex &file_mutex, Network &network);

	static bool valid_group_name(const std::string &name, bool use = false);
	static bool valid_preset_name(const std::string &name, bool use = false);
	static std::string addresses_text(const Dali::addresses_t &addresses);
	static std::string preset_levels_text(const std::array<int16_t,Dali::num_addresses> &levels,
		const Dali::addresses_t *filter);

	void setup();
	void loop();
	void load_config();
	void save_config();
	void publish_config() const;

	Dali::addresses_t get_addresses() const;
	void set_addresses(const std::string &addresses);
	std::string addresses_text() const;

	std::vector<std::string> group_names() const;
	Dali::addresses_t get_group_addresses(const std::string &name) const;
	void set_group_addresses(const std::string &name, const std::string &addresses);
	std::string group_addresses_text(const std::string &name) const;
	void delete_group(const std::string &name);

	std::string get_switch_name(unsigned int switch_id) const;
	void set_switch_name(unsigned int switch_id, const std::string &name);

	std::string get_switch_group(unsigned int switch_id) const;
	void set_switch_group(unsigned int switch_id, const std::string &name);

	std::string get_switch_preset(unsigned int switch_id) const;
	void set_switch_preset(unsigned int switch_id, const std::string &preset);

	std::string get_dimmer_group(unsigned int dimmer_id) const;
	void set_dimmer_group(unsigned int dimmer_id, const std::string &name);

	int get_dimmer_encoder_steps(unsigned int dimmer_id) const;
	void set_dimmer_encoder_steps(unsigned int dimmer_id, int encoder_steps);

	unsigned int get_dimmer_level_steps(unsigned int dimmer_id) const;
	void set_dimmer_level_steps(unsigned int dimmer_id, unsigned int level_steps);

	std::vector<std::string> preset_names() const;
	bool get_preset(const std::string &name, std::array<int16_t,Dali::num_addresses> &levels) const;
	bool get_ordered_preset(unsigned long long idx, std::string &name) const;
	void set_preset(const std::string &name, const std::string &light_ids, long level);
	void set_preset(const std::string &name, std::string levels);
	void set_ordered_presets(const std::string &names);
	void delete_preset(const std::string &name);

	Dali::addresses_t parse_light_ids(const std::string &light_ids, bool &idle_only) const;
	std::string lights_text(const Dali::addresses_t &lights) const;

private:
	static constexpr const char *TAG = "Config";
	static constexpr auto MAX_LEVEL = Dali::MAX_LEVEL;
	static constexpr size_t MAX_GROUPS = 16;
	static constexpr size_t MAX_GROUP_NAME_LEN = 50;
	static constexpr size_t MAX_PRESETS = 20;
	static constexpr size_t MAX_PRESET_NAME_LEN = 50;
	static constexpr size_t MAX_SWITCH_NAME_LEN = 50;

	Config(const Config&) = delete;
	Config& operator=(const Config&) = delete;

	void dirty_config();
	void set_addresses(const std::string &group, std::string addresses);
	void publish_preset(const std::string &name, const std::array<int16_t,Dali::num_addresses> &levels) const;

	Network &network_;

	std::mutex &file_mutex_;
	ConfigFile file_;
	ConfigData last_saved_;
	bool saved_{false};

	mutable std::recursive_mutex data_mutex_;
	ConfigData current_;
	bool dirty_{false};
};
