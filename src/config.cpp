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

/**
 * Config writes are saved asynchronously by marking it as dirty and then saving
 * it to a file on the main loop. Config reads will always get the latest data,
 * without blocking on the file write and even if it hasn't been saved yet.
 */
#include "config.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <CBOR.h>
#include <CBOR_parsing.h>
#include <CBOR_streams.h>
#include <FS.h>
#include <LittleFS.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <mutex>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "dali.h"
#include "dimmers.h"
#include "lights.h"
#include "network.h"
#include "selector.h"
#include "switches.h"
#include "util.h"

static constexpr auto &FS = LittleFS;

static const std::string BUILTIN_GROUP_IDLE = "idle";
static constexpr size_t MAX_TEXT_LEN = 256;
static const std::string FILENAME = "/config.cbor";
static const std::string BACKUP_FILENAME = "/config.cbor~";

namespace cbor = qindesign::cbor;

#define CFG_LOG ESP_LOGD

static std::string quoted_string(const std::string &text) {
	if (text.empty()) {
		return "`(null)`";
	} else {
		return std::string{"`"} + text + "`";
	}
}

static std::string vector_text(const std::vector<std::string> &vector) {
	std::string text;

	for (const auto &item : vector) {
		if (!text.empty()) {
			text += ",";
		}

		text += item;
	}

	return text;
}

static bool readText(cbor::Reader &reader, std::string &text, size_t max_length) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectText(reader, &length, &indefinite) || indefinite)
			return false;

	if (length > max_length)
			return false;

	std::vector<char> data(length + 1);

	if (cbor::readFully(reader, reinterpret_cast<uint8_t*>(data.data()), length) != length)
			return false;

	text = {data.data()};
	return true;
}

static void writeText(cbor::Writer &writer, const std::string &value) {
	size_t length = std::min(value.length(), MAX_TEXT_LEN);

	writer.beginText(length);
	writer.writeBytes(reinterpret_cast<const uint8_t*>(value.c_str()), length);
}

Config::Config(std::mutex &file_mutex, Network &network,
	const Selector &selector) : network_(network), selector_(selector),
	file_mutex_(file_mutex), file_(network) {
}

ConfigFile::ConfigFile(Network &network) : network_(network) {
}

void Config::setup() {
	if (!FS.begin(true)) {
		ESP_LOGE(TAG, "Filesystem failed to start");
		esp_restart();
	}
	load_config();
}

void Config::loop() {
	save_config();
}

bool Config::valid_group_name(const std::string &name, bool use) {
	if ((name == BUILTIN_GROUP_ALL && !use)
			|| name == BUILTIN_GROUP_IDLE
			|| name == RESERVED_GROUP_DELETE
			|| name == RESERVED_GROUP_LEVELS
			|| name == RESERVED_GROUP_SYNC
			|| name.empty()
			|| name.length() > MAX_GROUP_NAME_LEN) {
		return false;
	}

	if (!(name[0] >= 'a' && name[0] <= 'z')) {
		return false;
	}

	for (size_t i = 0; i < name.length(); i++) {
		if (name[i] >= '0' && name[i] <= '9') {
			continue;
		} else if (name[i] >= 'a' && name[i] <= 'z') {
			continue;
		} else if (name[i] == '.' || name[i] == '-' || name[i] == '_') {
			continue;
		}

		return false;
	}

	return true;
}

bool Config::valid_preset_name(const std::string &name, bool use) {
	if ((name == BUILTIN_PRESET_OFF && !use)
			|| name == RESERVED_PRESET_CUSTOM
			|| name == RESERVED_PRESET_ORDER
			|| name == RESERVED_PRESET_UNKNOWN
			|| name.empty()
			|| name.length() > MAX_PRESET_NAME_LEN) {
		return false;
	}

	if (!(name[0] >= 'a' && name[0] <= 'z')) {
		return false;
	}

	for (size_t i = 0; i < name.length(); i++) {
		if (name[i] >= '0' && name[i] <= '9') {
			continue;
		} else if (name[i] >= 'a' && name[i] <= 'z') {
			continue;
		} else if (name[i] == '.' || name[i] == '-' || name[i] == '_') {
			continue;
		}

		return false;
	}

	return true;
}

std::string Config::addresses_text() const {
	return addresses_text(get_addresses());
}

std::string Config::group_addresses_text(const std::string &group) const {
	return addresses_text(get_group_addresses(group));
}

std::string Config::addresses_text(const Dali::addresses_t &addresses) {
	std::vector<char> buffer(2 * addresses.size() + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i < addresses.size(); i++) {
		if (addresses[i]) {
			snprintf(&buffer[offset], 3, "%02X", (unsigned int)(i & 0xFFU));
			offset += 2;
		}
	}

	return {buffer.data(), offset};
}

std::string Config::preset_levels_text(
		const std::array<Dali::level_fast_t,Dali::num_addresses> &levels,
		const Dali::addresses_t *filter) {
	std::vector<char> buffer(2 * levels.size() + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i < levels.size(); i++) {
		if (filter == nullptr || filter->test(i)) {
			snprintf(&buffer[offset], 3, "%02X", (unsigned int)(levels[i] & 0xFFU));
			offset += 2;
		}
	}

	if (!offset) {
		return "(null)";
	}

	return {buffer.data(), offset};
}

void ConfigData::assign_group_ids() {
	std::bitset<Dali::num_groups> group_ids;

	groups_by_id.fill({});

	for (auto &group : groups_by_name) {
		if (group.second.id == Dali::GROUP_NONE) {
			continue;
		}

		if (group.second.id >= group_ids.size() || group_ids[group.second.id]) {
			group.second.id = Dali::GROUP_NONE;
			continue;
		}

		group_ids[group.second.id] = true;
		groups_by_id[group.second.id] = group.second.addresses;
	}

	for (auto &group : groups_by_name) {
		if (group.second.id != Dali::GROUP_NONE) {
			continue;
		}

		for (Dali::group_t i = 0; i < Dali::num_groups; i++) {
			if (!group_ids[i]) {
				group.second.id = i;
				group_ids[i] = true;
				groups_by_id[i] = group.second.addresses;
				break;
			}
		}
	}
}

void Config::load_config() {
	std::lock_guard file_lock{file_mutex_};
	ConfigData new_data;

	if (!file_.read_config(new_data)) {
		return;
	}

	std::lock_guard data_lock{data_mutex_};

	current_ = new_data;
	last_saved_ = current_;
	dirty_ = false;
	saved_ = true;
}

bool ConfigFile::read_config(ConfigData &data) {
	if (!read_config(FILENAME, true)) {
		if (!read_config(BACKUP_FILENAME, true)) {
			return false;
		}
		write_config(FILENAME);
	}

	data = data_;
	return true;
}

bool ConfigFile::read_config(const std::string &filename, bool load) {
	uint64_t start = esp_timer_get_time();

	CFG_LOG(TAG, "Reading config file %s", filename.c_str());
	const char mode[2] = {'r', '\0'};
	auto file = FS.open(filename.c_str(), mode);
	if (file) {
		cbor::Reader reader{file};

		if (!cbor::expectValue(reader, cbor::DataType::kTag, cbor::kSelfDescribeTag)
				|| !reader.isWellFormed()) {
			ESP_LOGE(TAG, "Failed to parse config file %s", filename.c_str());
			return false;
		} else {
			if (load) {
				CFG_LOG(TAG, "Loading config from file %s", filename.c_str());
				file.seek(0);

				if (!cbor::expectValue(reader, cbor::DataType::kTag, cbor::kSelfDescribeTag))
					return false;

				if (read_config(reader)) {
					CFG_LOG(TAG, "Loaded config from file %s", filename.c_str());
					uint64_t finish = esp_timer_get_time();
					network_.publish(FixedConfig::mqttTopic("/loaded_config"), filename);
					network_.publish(FixedConfig::mqttTopic("/config_size"), std::to_string(file.size()), true);
					network_.publish(FixedConfig::mqttTopic("/config_read_time_us"), std::to_string(finish - start));
				} else {
					ESP_LOGE(TAG, "Invalid config file %s", filename.c_str());
				}
			}
			return true;
		}
	} else {
		ESP_LOGE(TAG, "Config file %s does not exist", filename.c_str());
		return false;
	}
}

bool ConfigFile::read_config(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;

	data_ = {};

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "lights") {
			if (!read_config_lights(reader, data_.lights)) {
				return false;
			}

			CFG_LOG(TAG, "Lights = %s", Config::addresses_text(data_.lights).c_str());
		} else if (key == "groups") {
			if (!read_config_groups(reader)) {
				return false;
			}
		} else if (key == "switches") {
			if (!read_config_switches(reader)) {
				return false;
			}
		} else if (key == "buttons") {
			if (!read_config_buttons(reader)) {
				return false;
			}
		} else if (key == "dimmers") {
			if (!read_config_dimmers(reader)) {
				return false;
			}
		} else if (key == "selector") {
			if (!read_config_selectors(reader)) {
				return false;
			}
		} else if (key == "presets") {
			if (!read_config_presets(reader)) {
				return false;
			}
		} else if (key == "order") {
			if (!read_config_order(reader)) {
				return false;
			}
		} else {
			CFG_LOG(TAG, "Unknown key: %s", key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_lights(cbor::Reader &reader, Dali::addresses_t &lights) {
	uint64_t length;
	bool indefinite;
	unsigned int i = 0;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		bool value;

		if (!cbor::expectBoolean(reader, &value)) {
			return false;
		}

		if (i < lights.size()) {
			lights[i] = value;
			i++;
		}
	}

	return true;
}

bool ConfigFile::read_config_groups(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		if (!read_config_group(reader)) {
			return false;
		}
	}

	data_.assign_group_ids();

	return true;
}

bool ConfigFile::read_config_group(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	std::string name;
	ConfigGroupData group;

	group.id = Dali::GROUP_NONE;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "name") {
			if (!readText(reader, name, UINT8_MAX)) {
				return false;
			}
		} else if (key == "id") {
			uint64_t value;

			if (!cbor::expectUnsignedInt(reader, &value)) {
				return false;
			}

			if (value < Dali::num_groups) {
				group.id = value;
			}
		} else if (key == "lights") {
			if (!read_config_lights(reader, group.addresses)) {
				return false;
			}
		} else {
			CFG_LOG(TAG, "Unknown group key: %s", key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	if (Config::valid_group_name(name)) {
		if (data_.groups_by_name.size() < Config::MAX_GROUPS) {
			auto result = data_.groups_by_name.emplace(name, std::move(group));

			if (result.second) {
				CFG_LOG(TAG, "Group %s (%u) = %s", name.c_str(),
					result.first->second.id,
					Config::addresses_text(result.first->second.addresses).c_str());
			} else {
				CFG_LOG(TAG, "Ignoring duplicate group: %s", name.c_str());
			}
		} else {
			CFG_LOG(TAG, "Too many groups, ignoring: %s", name.c_str());
		}
	} else {
		CFG_LOG(TAG, "Ignoring invalid group: %s", name.c_str());
	}

	return true;
}

bool ConfigFile::read_config_switches(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	unsigned int i = 0;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		if (i < NUM_SWITCHES) {
			if (!read_config_switch(reader, i)) {
				return false;
			}

			i++;
		} else {
			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_switch(cbor::Reader &reader, unsigned int switch_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "name") {
			std::string name;

			if (!readText(reader, name, UINT8_MAX)) {
				return false;
			}

			CFG_LOG(TAG, "Switch %u name = %s", switch_id, name.c_str());
			data_.switches[switch_id].name = std::move(name);
		} else if (key == "group") {
			std::string group;

			if (!readText(reader, group, UINT8_MAX)) {
				return false;
			}

			if (group.empty() || Config::valid_group_name(group, true)) {
				CFG_LOG(TAG, "Switch %u group = %s", switch_id, group.c_str());
				data_.switches[switch_id].group = std::move(group);
			} else {
				CFG_LOG(TAG, "Switch %u invalid group ignored: %s", switch_id, group.c_str());
			}
		} else if (key == "preset") {
			std::string preset;

			if (!readText(reader, preset, UINT8_MAX)) {
				return false;
			}

			if (preset.empty() || Config::valid_preset_name(preset, true)) {
				CFG_LOG(TAG, "Switch %u preset = %s", switch_id, preset.c_str());
				data_.switches[switch_id].preset = std::move(preset);
			} else {
				CFG_LOG(TAG, "Switch %u invalid preset ignored: %s", switch_id, preset.c_str());
			}
		} else {
			CFG_LOG(TAG, "Unknown switch %u key: %s", switch_id, key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_buttons(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	unsigned int i = 0;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		if (i < NUM_BUTTONS) {
			if (!read_config_button(reader, i)) {
				return false;
			}

			i++;
		} else {
			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_button(cbor::Reader &reader, unsigned int button_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "groups") {
			if (!read_config_button_groups(reader, button_id)) {
				return false;
			}
		} else if (key == "preset") {
			std::string preset;

			if (!readText(reader, preset, UINT8_MAX)) {
				return false;
			}

			if (preset.empty() || Config::valid_preset_name(preset, true)) {
				CFG_LOG(TAG, "Button %u preset = %s", button_id, preset.c_str());
				data_.buttons[button_id].preset = std::move(preset);
			} else {
				CFG_LOG(TAG, "Button %u invalid preset ignored: %s", button_id, preset.c_str());
			}
		} else {
			CFG_LOG(TAG, "Unknown button %u key: %s", button_id, key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_button_groups(cbor::Reader &reader, unsigned int button_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string group;

		if (!readText(reader, group, UINT8_MAX)) {
			return false;
		}

		if (group.empty() || Config::valid_group_name(group, true)) {
			CFG_LOG(TAG, "Button %u group += %s", button_id, group.c_str());
			data_.buttons[button_id].groups.push_back(std::move(group));
		} else {
			CFG_LOG(TAG, "Button %u invalid group ignored: %s", button_id, group.c_str());
		}
	}

	return true;
}

bool ConfigFile::read_config_dimmers(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	unsigned int i = 0;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		if (i < NUM_DIMMERS) {
			if (!read_config_dimmer(reader, i)) {
				return false;
			}

			i++;
		} else {
			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_dimmer(cbor::Reader &reader, unsigned int dimmer_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "group") {
			std::string group;

			if (!readText(reader, group, UINT8_MAX)) {
				return false;
			}

			if (group.empty() || Config::valid_group_name(group, true)) {
				CFG_LOG(TAG, "Dimmer %u group = %s", dimmer_id, group.c_str());
				data_.dimmers[dimmer_id].groups.clear();
				data_.dimmers[dimmer_id].groups.push_back(std::move(group));
			} else {
				CFG_LOG(TAG, "Dimmer %u invalid group ignored: %s", dimmer_id, group.c_str());
			}
		} else if (key == "groups") {
			if (!read_config_dimmer_groups(reader, dimmer_id)) {
				return false;
			}
		} else if (key == "encoder_steps") {
			int64_t steps;

			if (!cbor::expectInt(reader, &steps)) {
				return false;
			}

			if (steps >= MIN_ENCODER_STEPS && steps <= MAX_ENCODER_STEPS) {
				CFG_LOG(TAG, "Dimmer %u encoder steps = %" PRId64, dimmer_id, steps);
				data_.dimmers[dimmer_id].encoder_steps = steps;
			}
		} else if (key == "level_steps") {
			uint64_t steps;

			if (!cbor::expectUnsignedInt(reader, &steps)) {
				return false;
			}

			if (steps <= Dali::MAX_LEVEL) {
				CFG_LOG(TAG, "Dimmer %u level steps = %" PRIu64, dimmer_id, steps);
				data_.dimmers[dimmer_id].level_steps = steps;
			}
		} else if (key == "mode") {
			std::string mode;
			DimmerMode value;

			if (!readText(reader, mode, UINT8_MAX)) {
				return false;
			}

			if (Dimmers::mode_value(mode, value)) {
				CFG_LOG(TAG, "Dimmer %u mode = %s", dimmer_id, Dimmers::mode_text(value));
				data_.dimmers[dimmer_id].mode = value;
			} else {
				CFG_LOG(TAG, "Dimmer %u invalid mode ignored: %s", dimmer_id, mode.c_str());
			}
		} else {
			CFG_LOG(TAG, "Unknown dimmer %u key: %s", dimmer_id, key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_dimmer_groups(cbor::Reader &reader, unsigned int dimmer_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string group;

		if (!readText(reader, group, UINT8_MAX)) {
			return false;
		}

		if (group.empty() || Config::valid_group_name(group, true)) {
			CFG_LOG(TAG, "Dimmer %u group += %s", dimmer_id, group.c_str());
			data_.dimmers[dimmer_id].groups.push_back(std::move(group));
		} else {
			CFG_LOG(TAG, "Dimmer %u invalid group ignored: %s", dimmer_id, group.c_str());
		}
	}

	return true;
}

bool ConfigFile::read_config_selectors(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	unsigned int i = 0;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		if (i < NUM_OPTIONS) {
			if (!read_config_selector(reader, i)) {
				return false;
			}

			i++;
		} else {
			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_selector(cbor::Reader &reader, unsigned int option_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "groups") {
			if (!read_config_selector_groups(reader, option_id)) {
				return false;
			}
		} else {
			CFG_LOG(TAG, "Unknown selector option %u key: %s", option_id, key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool ConfigFile::read_config_selector_groups(cbor::Reader &reader, unsigned int option_id) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string group;

		if (!readText(reader, group, UINT8_MAX)) {
			return false;
		}

		if (group.empty() || Config::valid_group_name(group, true)) {
			CFG_LOG(TAG, "Selector option %u group += %s", option_id, group.c_str());
			data_.selector_groups[option_id].push_back(std::move(group));
		} else {
			CFG_LOG(TAG, "Selector option %u invalid group ignored: %s", option_id, group.c_str());
		}
	}

	return true;
}

bool ConfigFile::read_config_presets(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		if (!read_config_preset(reader)) {
			return false;
		}
	}

	return true;
}

bool ConfigFile::read_config_preset(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	std::string name;
	std::array<Dali::level_fast_t,Dali::num_addresses> levels;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	levels.fill(Dali::LEVEL_NO_CHANGE);

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "name") {
			if (!readText(reader, name, UINT8_MAX)) {
				return false;
			}
		} else if (key == "levels") {
			if (!read_config_preset_levels(reader, levels)) {
				return false;
			}
		} else {
			CFG_LOG(TAG, "Unknown preset key: %s", key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	if (Config::valid_preset_name(name)) {
		auto result = data_.presets.emplace(name, std::move(levels));

		if (result.second) {
			CFG_LOG(TAG, "Preset %s = %s", name.c_str(),
				Config::preset_levels_text(result.first->second, nullptr).c_str());
		} else {
			CFG_LOG(TAG, "Ignoring duplicate preset: %s", name.c_str());
		}
	} else {
		CFG_LOG(TAG, "Ignoring invalid preset: %s", name.c_str());
	}

	return true;
}

bool ConfigFile::read_config_preset_levels(cbor::Reader &reader,
		std::array<Dali::level_fast_t,Dali::num_addresses> &levels) {
	uint64_t length;
	bool indefinite;
	unsigned int i = 0;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		int64_t value;

		if (!cbor::expectInt(reader, &value)) {
			return false;
		}

		if (i < levels.size()) {
			if (value == Config::LEVEL_NO_CHANGE) {
				levels[i] = Dali::LEVEL_NO_CHANGE;
			} else if (value >= 0 && value <= Dali::MAX_LEVEL) {
				levels[i] = value;
			}
			i++;
		}
	}

	return true;
}

bool ConfigFile::read_config_order(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;

	if (!cbor::expectArray(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string preset;

		if (!readText(reader, preset, UINT8_MAX)) {
			return false;
		}

		if (Config::valid_preset_name(preset, true)) {
			CFG_LOG(TAG, "Ordered preset %zu: %s", data_.ordered.size(), preset.c_str());
			data_.ordered.push_back(std::move(preset));
		} else {
			CFG_LOG(TAG, "Ignoring invalid preset: %s", preset.c_str());
		}
	}

	return true;
}

void Config::dirty_config() {
	std::lock_guard lock{data_mutex_};

	dirty_ = true;
}

void Config::save_config() {
	std::lock_guard file_lock{file_mutex_};
	std::unique_lock data_lock{data_mutex_};

	if (saved_ && !dirty_) {
		return;
	}

	if (current_ == last_saved_) {
		dirty_ = false;
		return;
	}

	ConfigData save_data{current_};

	/*
	 * If the config changes while we're writing it,
	 * it'll have to be written again.
	 */
	dirty_ = false;

	data_lock.unlock();
	/* If this fails, don't retry - wait until the config changes again */
	file_.write_config(save_data);
	data_lock.lock();

	last_saved_ = save_data;
	saved_ = true;
}

bool ConfigFile::write_config(const ConfigData &data) {
	data_ = data;
	return write_config(FILENAME) && read_config(FILENAME, false) && write_config(BACKUP_FILENAME);
}

bool ConfigFile::write_config(const std::string &filename) const {
	uint64_t start = esp_timer_get_time();
	CFG_LOG(TAG, "Writing config file %s", filename.c_str());
	{
		const char mode[2] = {'w', '\0'};
		auto file = FS.open(filename.c_str(), mode);
		if (file) {
			cbor::Writer writer{file};

			writer.writeTag(cbor::kSelfDescribeTag);
			write_config(writer);

			if (file.getWriteError()) {
				network_.report(TAG, std::string{"Failed to write config file "} + filename
						+ ": " + std::to_string(file.getWriteError()));
				return false;
			}
		} else {
			network_.report(TAG, std::string{"Unable to open config file "} + filename + " for writing");
			return false;
		}
	}
	{
		const char mode[2] = {'r', '\0'};
		auto file = FS.open(filename.c_str(), mode);
		if (file) {
			CFG_LOG(TAG, "Saved config to file %s", filename.c_str());
			uint64_t finish = esp_timer_get_time();
			network_.publish(FixedConfig::mqttTopic("/saved_config"), filename);
			network_.publish(FixedConfig::mqttTopic("/config_size"), std::to_string(file.size()), true);
			network_.publish(FixedConfig::mqttTopic("/config_write_time_us"), std::to_string(finish - start));
			return true;
		} else {
			network_.report(TAG, std::string{"Unable to open config file "} + filename + " for reading");
			return false;
		}
	}
}

void ConfigFile::write_config(cbor::Writer &writer) const {
	writer.beginMap(8);

	writeText(writer, "lights");
	writer.beginArray(data_.lights.size());
	for (unsigned int i = 0; i < data_.lights.size(); i++) {
		writer.writeBoolean(data_.lights[i]);
	}

	writeText(writer, "groups");
	writer.beginArray(data_.groups_by_name.size());
	for (const auto &group : data_.groups_by_name) {
		writer.beginMap(3);

		writeText(writer, "name");
		writeText(writer, group.first);

		writeText(writer, "id");
		writer.writeUnsignedInt(group.second.id);

		writeText(writer, "lights");
		writer.beginArray(group.second.addresses.size());
		for (unsigned int i = 0; i < group.second.addresses.size(); i++) {
			writer.writeBoolean(group.second.addresses[i]);
		}
	}

	writeText(writer, "switches");
	writer.beginArray(NUM_SWITCHES);
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		writer.beginMap(3);

		writeText(writer, "name");
		writeText(writer, data_.switches[i].name);

		writeText(writer, "group");
		writeText(writer, data_.switches[i].group);

		writeText(writer, "preset");
		writeText(writer, data_.switches[i].preset);
	}

	writeText(writer, "buttons");
	writer.beginArray(NUM_BUTTONS);
	for (unsigned int i = 0; i < NUM_BUTTONS; i++) {
		writer.beginMap(2);

		writeText(writer, "groups");
		writer.beginArray(data_.buttons[i].groups.size());
		for (const auto &group : data_.buttons[i].groups) {
			writeText(writer, group);
		}

		writeText(writer, "preset");
		writeText(writer, data_.buttons[i].preset);
	}

	writeText(writer, "dimmers");
	writer.beginArray(NUM_DIMMERS);
	for (unsigned int i = 0; i < NUM_DIMMERS; i++) {
		writer.beginMap(4);

		writeText(writer, "groups");
		writer.beginArray(data_.dimmers[i].groups.size());
		for (const auto &group : data_.dimmers[i].groups) {
			writeText(writer, group);
		}

		writeText(writer, "encoder_steps");
		writer.writeInt(data_.dimmers[i].encoder_steps);

		writeText(writer, "level_steps");
		writer.writeUnsignedInt(data_.dimmers[i].level_steps);

		writeText(writer, "mode");
		writeText(writer, Dimmers::mode_text(data_.dimmers[i].mode));
	}

	writeText(writer, "selector");
	writer.beginArray(NUM_OPTIONS);
	for (unsigned int i = 0; i < NUM_OPTIONS; i++) {
		writer.beginMap(1);

		writeText(writer, "groups");
		writer.beginArray(data_.selector_groups[i].size());
		for (const auto &group : data_.selector_groups[i]) {
			writeText(writer, group);
		}
	}

	writeText(writer, "presets");
	writer.beginArray(data_.presets.size());
	for (const auto &preset : data_.presets) {
		writer.beginMap(2);

		writeText(writer, "name");
		writeText(writer, preset.first);

		writeText(writer, "levels");
		writer.beginArray(preset.second.size());
		for (unsigned int i = 0; i < preset.second.size(); i++) {
			if (preset.second[i] == Dali::LEVEL_NO_CHANGE) {
				writer.writeInt(Config::LEVEL_NO_CHANGE);
			} else {
				writer.writeInt(preset.second[i]);
			}
		}
	}

	writeText(writer, "order");
	writer.beginArray(data_.ordered.size());
	for (const auto &preset : data_.ordered) {
		writeText(writer, preset);
	}
}

void Config::publish_config() const {
	std::lock_guard lock{data_mutex_};

	network_.publish(FixedConfig::mqttTopic("/addresses"),
		addresses_text(current_.lights), true);

	for (const auto &group : current_.groups_by_name) {
		network_.publish(FixedConfig::mqttTopic("/group/") + group.first,
			addresses_text(group.second.addresses), true);
	}

	publish_group_ids();

	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		auto switch_prefix = FixedConfig::mqttTopic("/switch/") + std::to_string(i);

		network_.publish(switch_prefix + "/name", current_.switches[i].name, true);
		network_.publish(switch_prefix + "/group", current_.switches[i].group, true);
		network_.publish(switch_prefix + "/preset", current_.switches[i].preset, true);
	}

	for (unsigned int i = 0; i < NUM_BUTTONS; i++) {
		auto button_prefix = FixedConfig::mqttTopic("/button/") + std::to_string(i);

		network_.publish(button_prefix + "/groups",
				vector_text(current_.buttons[i].groups), true);
		network_.publish(button_prefix + "/preset", current_.buttons[i].preset, true);
	}

	for (unsigned int i = 0; i < NUM_DIMMERS; i++) {
		auto dimmer_prefix = FixedConfig::mqttTopic("/dimmer/") + std::to_string(i);

		network_.publish(dimmer_prefix + "/groups",
			vector_text(current_.dimmers[i].groups), true);
		network_.publish(dimmer_prefix + "/encoder_steps",
			std::to_string(current_.dimmers[i].encoder_steps), true);
		network_.publish(dimmer_prefix + "/level_steps",
			std::to_string(current_.dimmers[i].level_steps), true);
		network_.publish(dimmer_prefix + "/mode",
			Dimmers::mode_text(current_.dimmers[i].mode), true);
	}

	for (unsigned int i = 0; i < NUM_OPTIONS; i++) {
		auto selector_prefix = FixedConfig::mqttTopic("/selector/") + std::to_string(i);

		network_.publish(selector_prefix + "/groups",
				vector_text(current_.selector_groups[i]), true);
	}

	for (const auto &preset : current_.presets) {
		publish_preset(preset.first, preset.second);
	}

	network_.publish(FixedConfig::mqttTopic("/preset/order"),
		vector_text(current_.ordered), true);
}

void Config::publish_group_ids() const {
	std::array<std::string,Dali::num_groups> groups;

	for (const auto &group : current_.groups_by_name) {
		if (group.second.id < groups.size()) {
			groups[group.second.id] = group.first;
		}
	}

	std::string text;
	bool first = true;

	for (const auto &group : groups) {
		if (!first) {
			text += ',';
		}

		text += group;
		first = false;
	}

	network_.publish(FixedConfig::mqttTopic("/groups/ids"), text, true);
}

void Config::publish_preset(const std::string &name,
		const std::array<Dali::level_fast_t,Dali::num_addresses> &levels) const {
	network_.publish(FixedConfig::mqttTopic("/preset/") + name + "/levels",
		preset_levels_text(levels, nullptr), true);
}

Dali::addresses_t Config::get_addresses() const {
	return get_group_addresses(BUILTIN_GROUP_ALL);
}

std::vector<std::string> Config::group_names() const {
	std::vector<std::string> groups;

	groups.reserve(MAX_GROUPS + 1);
	groups.emplace_back(BUILTIN_GROUP_ALL);

	std::unique_lock lock{data_mutex_};

	for (const auto &group : current_.groups_by_name) {
		groups.emplace_back(group.first);
	}

	lock.unlock();

	std::sort(groups.begin(), groups.end());

	return groups;
}

Dali::group_t Config::get_group_id(const std::string &group) const {
	std::lock_guard lock{data_mutex_};
	auto it = current_.groups_by_name.find(group);

	if (it == current_.groups_by_name.end()) {
		return Dali::GROUP_NONE;
	}

	return it->second.id;
}

Dali::addresses_t Config::get_group_addresses(const std::string &group) const {
	std::lock_guard lock{data_mutex_};

	if (group == BUILTIN_GROUP_ALL) {
		return current_.lights;
	} else {
		auto it = current_.groups_by_name.find(group);

		if (it == current_.groups_by_name.end()) {
			return {};
		}

		return it->second.addresses;
	}
}

Dali::addresses_t Config::get_group_addresses(Dali::group_t group) const {
	std::lock_guard lock{data_mutex_};

	if (group < current_.groups_by_id.size()) {
		return current_.groups_by_id[group];
	} else {
		return {};
	}
}

std::array<Dali::addresses_t,Dali::num_groups> Config::get_group_addresses() const {
	std::lock_guard lock{data_mutex_};

	return current_.groups_by_id;
}

void Config::set_addresses(const std::string &addresses) {
	std::lock_guard lock{data_mutex_};

	set_addresses(BUILTIN_GROUP_ALL, addresses);
}

bool Config::set_group_addresses(const std::string &name, const std::string &addresses) {
	std::lock_guard lock{data_mutex_};

	if (!valid_group_name(name)) {
		return false;
	}

	return set_addresses(name, addresses);
}

bool Config::set_addresses(const std::string &group, std::string addresses) {
	std::lock_guard lock{data_mutex_};
	Dali::addresses_t lights;

	auto before = group_addresses_text(group);

	while (addresses.length() >= 2) {
		unsigned int address = 0;

		if (addresses[0] >= '0' && addresses[0] <= '9') {
			address |= (addresses[0] - '0') << 4;
		} else if (addresses[0] >= 'A' && addresses[0] <= 'F') {
			address |= (addresses[0] - 'A' + 10) << 4;
		} else {
			break;
		}

		if (addresses[1] >= '0' && addresses[1] <= '9') {
			address |= addresses[1] - '0';
		} else if (addresses[1] >= 'A' && addresses[1] <= 'F') {
			address |= addresses[1] - 'A' + 10;
		} else {
			break;
		}

		if (address < lights.size()) {
			lights[address] = true;
		}

		addresses = addresses.substr(2);
	}

	if (group == BUILTIN_GROUP_ALL) {
		current_.lights = lights;
	} else {
		auto it = current_.groups_by_name.find(group);

		if (it == current_.groups_by_name.end()) {
			ConfigGroupData data{Dali::GROUP_NONE, lights};

			if (current_.groups_by_name.size() >= MAX_GROUPS) {
				return false;
			}

			current_.groups_by_name.emplace(group, std::move(data));
			current_.assign_group_ids();
			publish_group_ids();
		} else {
			it->second.addresses = lights;

			if (it->second.id < current_.groups_by_id.size()) {
				current_.groups_by_id[it->second.id] = lights;
			}
		}
	}

	auto after = addresses_text(lights);
	bool changed = before != after;

	if (changed) {
		if (group == BUILTIN_GROUP_ALL) {
			CFG_LOG(TAG, "Configure light addresses: %s", addresses.c_str());
			network_.publish(FixedConfig::mqttTopic("/addresses"), after, true);
			network_.report(TAG, std::string{"Addresses: "}
				+ quoted_string(before) + " -> " + quoted_string(after));
		} else {
			CFG_LOG(TAG, "Configure group %s addresses: %s", group.c_str(), addresses.c_str());
			network_.publish(FixedConfig::mqttTopic("/group/") + group, after, true);
			network_.report(TAG, std::string{"Group "} + quoted_string(group)
				+ " addresses: " + quoted_string(before) + " -> "
				+ quoted_string(after));
		}
	}

	dirty_config();

	return changed;
}

void Config::delete_group(const std::string &name) {
	std::lock_guard lock{data_mutex_};
	const auto it = current_.groups_by_name.find(name);

	if (it == current_.groups_by_name.cend()) {
		return;
	}

	CFG_LOG(TAG, "Delete group %s", name.c_str());
	network_.report(TAG, std::string{"Group "} + name + ": "
		+ quoted_string(group_addresses_text(name)) + " (deleted)");

	if (it->second.id < current_.groups_by_id.size()) {
		current_.groups_by_id[it->second.id].reset();
	}
	current_.groups_by_name.erase(it);
	network_.publish(FixedConfig::mqttTopic("/group/") + name, "", true);
	publish_group_ids();
	for (const auto &preset : preset_names()) {
		network_.publish(FixedConfig::mqttTopic("/active/") + name + "/" + preset, "", true);
	}

	dirty_config();
}

std::string Config::get_switch_name(unsigned int switch_id) const {
	std::lock_guard lock{data_mutex_};

	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].name;
	} else {
		return "";
	}
}

void Config::set_switch_name(unsigned int switch_id, const std::string &name) {
	std::lock_guard lock{data_mutex_};

	if (switch_id < NUM_SWITCHES) {
		auto new_name = name.substr(0, MAX_SWITCH_NAME_LEN);

		if (current_.switches[switch_id].name != new_name) {
			network_.report(TAG, std::string{"Switch "}
				+ std::to_string(switch_id) + " name: "
				+ quoted_string(current_.switches[switch_id].name)
				+ " -> " + quoted_string(new_name));

			current_.switches[switch_id].name = new_name;
			dirty_config();
		}
	}
}

std::string Config::get_switch_group(unsigned int switch_id) const {
	std::lock_guard lock{data_mutex_};

	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].group;
	} else {
		return "";
	}
}

void Config::set_switch_group(unsigned int switch_id, const std::string &group) {
	std::lock_guard lock{data_mutex_};

	if (switch_id < NUM_SWITCHES) {
		if (!group.empty() && !valid_group_name(group, true)) {
			return;
		}

		if (current_.switches[switch_id].group != group) {
			network_.report(TAG, std::string{"Switch "}
				+ std::to_string(switch_id) + " group: "
				+ quoted_string(current_.switches[switch_id].group)
				+ " -> " + quoted_string(group));

			current_.switches[switch_id].group = group;
			dirty_config();
		}
	}
}

std::string Config::get_switch_preset(unsigned int switch_id) const {
	std::lock_guard lock{data_mutex_};

	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].preset;
	} else {
		return "";
	}
}

void Config::set_switch_preset(unsigned int switch_id, const std::string &preset) {
	std::lock_guard lock{data_mutex_};

	if (switch_id < NUM_SWITCHES) {
		if (!preset.empty() && !valid_preset_name(preset, true)) {
			return;
		}

		if (current_.switches[switch_id].preset != preset) {
			network_.report(TAG, std::string{"Switch "}
				+ std::to_string(switch_id) + " preset: "
				+ quoted_string(current_.switches[switch_id].preset)
				+ " -> " + quoted_string(preset));

			current_.switches[switch_id].preset = preset;
			dirty_config();
		}
	}
}

std::vector<std::string> Config::get_button_groups(unsigned int button_id) const {
	std::lock_guard lock{data_mutex_};

	if (button_id < NUM_BUTTONS) {
		return current_.buttons[button_id].groups;
	} else {
		return {};
	}
}

std::vector<std::string> Config::button_active_groups(unsigned int button_id) const {
	std::lock_guard lock{data_mutex_};

	if (button_id < NUM_BUTTONS) {
		return selector_group(current_.buttons[button_id].groups);
	} else {
		return {};
	}
}

void Config::set_button_groups(unsigned int button_id, const std::string &groups) {
	if (button_id >= NUM_BUTTONS) {
		return;
	}

	std::lock_guard lock{data_mutex_};
	std::istringstream input{groups};
	std::string item;
	std::vector<std::string> new_groups;

	auto before = vector_text(current_.buttons[button_id].groups);

	while (std::getline(input, item, ',')) {
		if (valid_group_name(item, true)) {
			new_groups.push_back(std::move(item));
		}
	}

	current_.buttons[button_id].groups = std::move(new_groups);

	auto after = vector_text(current_.buttons[button_id].groups);

	if (before != after) {
		network_.report(TAG, std::string{"Button "}
			+ std::to_string(button_id) + " groups: "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}

	dirty_config();
}

std::string Config::get_button_preset(unsigned int button_id) const {
	std::lock_guard lock{data_mutex_};

	if (button_id < NUM_BUTTONS) {
		return current_.buttons[button_id].preset;
	} else {
		return "";
	}
}

void Config::set_button_preset(unsigned int button_id, const std::string &preset) {
	std::lock_guard lock{data_mutex_};

	if (button_id < NUM_BUTTONS) {
		if (!preset.empty() && !valid_preset_name(preset, true)) {
			return;
		}

		if (current_.buttons[button_id].preset != preset) {
			network_.report(TAG, std::string{"Button "}
				+ std::to_string(button_id) + " preset: "
				+ quoted_string(current_.buttons[button_id].preset)
				+ " -> " + quoted_string(preset));

			current_.buttons[button_id].preset = preset;
			dirty_config();
		}
	}
}

DimmerConfig Config::get_dimmer(unsigned int dimmer_id) const {
	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		return make_dimmer(current_.dimmers[dimmer_id].mode,
			selector_group(current_.dimmers[dimmer_id].groups));
	} else {
		return {
			.mode = DimmerMode::INDIVIDUAL,
			.addresses{},
			.groups{},
			.address_group{},
			.group_addresses{},
			.all = false,
		};
	}
}

DimmerConfig Config::make_dimmer(DimmerMode mode, const std::string &groups) const {
	std::istringstream input{groups};
	std::string item;
	std::vector<std::string> groups_vec;

	while (std::getline(input, item, ',')) {
		if (valid_group_name(item, true)) {
			groups_vec.push_back(std::move(item));
		}
	}

	return make_dimmer(mode, groups_vec);
}

DimmerConfig Config::make_dimmer(DimmerMode mode,
		const std::vector<std::string> &groups) const {
	std::lock_guard lock{data_mutex_};

	DimmerConfig dimmer_config{
		.mode = mode,
		.addresses{},
		.groups{},
		.address_group{},
		.group_addresses{},
		.all = false,
	};

	dimmer_config.address_group.fill(Dali::GROUP_NONE);

	for (const auto &group : groups) {
		if (group == BUILTIN_GROUP_ALL) {
			dimmer_config.all = true;
			dimmer_config.addresses = current_.lights;

			if (dimmer_config.groups.any()) {
				goto invalid;
			}
		} else if (dimmer_config.all) {
			goto invalid;
		} else {
			auto it = current_.groups_by_name.find(group);

			if (it == current_.groups_by_name.end()) {
				continue;
			}

			if (it->second.id >= dimmer_config.groups.size()) {
				continue;
			}

			dimmer_config.groups[it->second.id] = true;

			for (unsigned int address = 0;
					address < dimmer_config.address_group.size(); address++) {
				if (current_.lights[address] && it->second.addresses[address]) {
					if (dimmer_config.address_group[address] != Dali::GROUP_NONE) {
						goto invalid;
					}

					dimmer_config.addresses[address] = true;
					dimmer_config.group_addresses[it->second.id][address] = true;
					dimmer_config.address_group[address] = it->second.id;
				}
			}
		}
	}

	return dimmer_config;

invalid:
	return {
		.mode = DimmerMode::INDIVIDUAL,
		.addresses{},
		.groups{},
		.address_group{},
		.group_addresses{},
		.all = false,
	};
}

const std::vector<std::string>& Config::selector_group(
		const std::vector<std::string> &groups) const {
	static const std::vector<std::string> empty;

	if (!groups.empty()) {
		return groups;
	}

	int option_id = selector_.read();

	if (option_id < NUM_OPTIONS) {
		return current_.selector_groups[option_id];
	}

	return empty;
}

std::vector<std::string> Config::get_dimmer_groups(unsigned int dimmer_id) const {
	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		return current_.dimmers[dimmer_id].groups;
	} else {
		return {};
	}
}

void Config::set_dimmer_groups(unsigned int dimmer_id, const std::string &groups) {
	if (dimmer_id >= NUM_DIMMERS) {
		return;
	}

	std::lock_guard lock{data_mutex_};
	std::istringstream input{groups};
	std::string item;
	std::vector<std::string> new_groups;

	auto before = vector_text(current_.dimmers[dimmer_id].groups);

	while (std::getline(input, item, ',')) {
		if (valid_group_name(item, true)) {
			new_groups.push_back(std::move(item));
		}
	}

	current_.dimmers[dimmer_id].groups = std::move(new_groups);

	auto after = vector_text(current_.dimmers[dimmer_id].groups);

	if (before != after) {
		network_.report(TAG, std::string{"Dimmer "}
			+ std::to_string(dimmer_id) + " groups: "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}

	dirty_config();
}

int Config::get_dimmer_encoder_steps(unsigned int dimmer_id) const {
	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		return current_.dimmers[dimmer_id].encoder_steps;
	} else {
		return 0;
	}
}

void Config::set_dimmer_encoder_steps(unsigned int dimmer_id, int encoder_steps) {
	if (encoder_steps < MIN_ENCODER_STEPS || encoder_steps > MAX_ENCODER_STEPS) {
		return;
	}

	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		if (current_.dimmers[dimmer_id].encoder_steps != encoder_steps) {
			network_.report(TAG, std::string{"Dimmer "}
				+ std::to_string(dimmer_id) + " encoder steps: "
				+ std::to_string(current_.dimmers[dimmer_id].encoder_steps)
				+ " -> " + std::to_string(encoder_steps));

			current_.dimmers[dimmer_id].encoder_steps = encoder_steps;
			dirty_config();
		}
	}
}

unsigned int Config::get_dimmer_level_steps(unsigned int dimmer_id) const {
	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		return current_.dimmers[dimmer_id].level_steps;
	} else {
		return 0;
	}
}

void Config::set_dimmer_level_steps(unsigned int dimmer_id, unsigned int level_steps) {
	if (level_steps > MAX_LEVEL) {
		return;
	}

	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		if (current_.dimmers[dimmer_id].level_steps != level_steps) {
			network_.report(TAG, std::string{"Dimmer "}
				+ std::to_string(dimmer_id) + " level steps: "
				+ std::to_string(current_.dimmers[dimmer_id].level_steps)
				+ " -> " + std::to_string(level_steps));

			current_.dimmers[dimmer_id].level_steps = level_steps;
			dirty_config();
		}
	}
}

DimmerMode Config::get_dimmer_mode(unsigned int dimmer_id) const {
	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		return current_.dimmers[dimmer_id].mode;
	} else {
		return DimmerMode::INDIVIDUAL;
	}
}

void Config::set_dimmer_mode(unsigned int dimmer_id, const std::string &mode) {
	std::lock_guard lock{data_mutex_};

	if (dimmer_id < NUM_DIMMERS) {
		DimmerMode new_dimmer_mode;

		if (Dimmers::mode_value(mode, new_dimmer_mode)
				&& current_.dimmers[dimmer_id].mode != new_dimmer_mode) {
			network_.report(TAG, std::string{"Dimmer "}
				+ std::to_string(dimmer_id) + " mode: "
				+ quoted_string(Dimmers::mode_text(current_.dimmers[dimmer_id].mode))
				+ " -> " + quoted_string(Dimmers::mode_text(new_dimmer_mode)));

			current_.dimmers[dimmer_id].mode = new_dimmer_mode;
			dirty_config();
		}
	}
}

std::vector<std::string> Config::get_selector_groups(unsigned int option_id) const {
	std::lock_guard lock{data_mutex_};

	if (option_id < NUM_OPTIONS) {
		return current_.selector_groups[option_id];
	} else {
		return {};
	}
}

void Config::set_selector_groups(unsigned int option_id, const std::string &groups) {
	if (option_id >= NUM_OPTIONS) {
		return;
	}

	std::lock_guard lock{data_mutex_};
	std::istringstream input{groups};
	std::string item;
	std::vector<std::string> new_groups;

	auto before = vector_text(current_.selector_groups[option_id]);

	while (std::getline(input, item, ',')) {
		if (valid_group_name(item, true)) {
			new_groups.push_back(std::move(item));
		}
	}

	current_.selector_groups[option_id] = std::move(new_groups);

	auto after = vector_text(current_.selector_groups[option_id]);

	if (before != after) {
		network_.report(TAG, std::string{"Selector option "}
			+ std::to_string(option_id) + " groups: "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}

	dirty_config();
}

std::vector<std::string> Config::preset_names() const {
	std::vector<std::string> presets;

	presets.reserve(MAX_PRESETS + 2);
	presets.emplace_back(BUILTIN_PRESET_OFF);
	presets.emplace_back(RESERVED_PRESET_CUSTOM);

	std::unique_lock lock{data_mutex_};

	for (const auto &preset : current_.presets) {
		presets.emplace_back(preset.first);
	}

	lock.unlock();

	std::sort(presets.begin(), presets.end());

	return presets;
}

bool Config::get_preset(const std::string &name,
		std::array<Dali::level_fast_t,Dali::num_addresses> &levels) const {
	std::lock_guard lock{data_mutex_};

	if (name == BUILTIN_PRESET_OFF) {
		levels.fill(0);
	} else {
		const auto it = current_.presets.find(name);

		if (it == current_.presets.cend()) {
			return false;
		}

		levels = it->second;
	}

	return true;
}

bool Config::get_ordered_preset(unsigned long long idx, std::string &name) const {
	std::lock_guard lock{data_mutex_};

	if (current_.ordered.empty()) {
		return false;
	}

	name = current_.ordered[idx % current_.ordered.size()];
	return true;
}

void Config::set_preset(const std::string &name, const std::string &light_ids, long level) {
	if (level == Config::LEVEL_NO_CHANGE) {
		level = Dali::LEVEL_NO_CHANGE;
	} else if (!(level >= 0 && level <= MAX_LEVEL)) {
		return;
	}

	if (!valid_preset_name(name)) {
		return;
	}

	std::lock_guard lock{data_mutex_};
	bool idle_only;
	auto lights = parse_light_ids(light_ids, idle_only);
	auto it = current_.presets.find(name);

	if (it == current_.presets.cend()) {
		if (current_.presets.size() == MAX_PRESETS) {
			return;
		}

		std::array<Dali::level_fast_t,Dali::num_addresses> levels;

		levels.fill(Dali::LEVEL_NO_CHANGE);
		it = current_.presets.emplace(name, std::move(levels)).first;
	}

	auto before = preset_levels_text(it->second, &current_.lights);

	for (unsigned int i = 0; i < current_.lights.size(); i++) {
		if (current_.lights[i]) {
			if (lights[i]) {
				it->second[i] = level;
			}
		} else {
			it->second[i] = Dali::LEVEL_NO_CHANGE;
		}
	}

	auto after = preset_levels_text(it->second, &current_.lights);

	if (before != after) {
		publish_preset(it->first, it->second);
	}

	network_.report(TAG, std::string{"Preset "} + name + ": "
		+ lights_text(lights) + " = " + std::to_string(level));

	if (before != after) {
		network_.report(TAG, std::string{"Preset "} + name + ": "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}

	dirty_config();
}

void Config::set_ordered_presets(const std::string &names) {
	std::lock_guard lock{data_mutex_};
	std::istringstream input{names};
	std::string item;
	std::vector<std::string> new_ordered;

	auto before = vector_text(current_.ordered);

	while (std::getline(input, item, ',')) {
		if (valid_preset_name(item, true)) {
			new_ordered.push_back(std::move(item));
		}
	}

	current_.ordered = std::move(new_ordered);

	auto after = vector_text(current_.ordered);

	if (before != after) {
		network_.report(TAG, std::string{"Preset order: "}
			+ quoted_string(before) + " -> " + quoted_string(after));
	}

	dirty_config();
}

void Config::set_preset(const std::string &name, std::string levels) {
	if (!valid_preset_name(name)) {
		return;
	}

	std::lock_guard lock{data_mutex_};
	auto it = current_.presets.find(name);
	std::string before;

	if (it == current_.presets.cend()) {
		if (current_.presets.size() == MAX_PRESETS) {
			return;
		}

		std::array<Dali::level_fast_t,Dali::num_addresses> empty_levels;

		empty_levels.fill(Dali::LEVEL_NO_CHANGE);
		it = current_.presets.emplace(name, std::move(empty_levels)).first;
	} else {
		before = preset_levels_text(it->second, &current_.lights);
	}

	unsigned int light_id = 0;

	it->second.fill(Dali::LEVEL_NO_CHANGE);

	while (levels.length() >= 2 && light_id < it->second.size()) {
		Dali::level_t level = 0;

		if (levels[0] >= '0' && levels[0] <= '9') {
			level |= (levels[0] - '0') << 4;
		} else if (levels[0] >= 'A' && levels[0] <= 'F') {
			level |= (levels[0] - 'A' + 10) << 4;
		} else {
			break;
		}

		if (levels[1] >= '0' && levels[1] <= '9') {
			level |= levels[1] - '0';
		} else if (levels[1] >= 'A' && levels[1] <= 'F') {
			level |= levels[1] - 'A' + 10;
		} else {
			break;
		}

		levels = levels.substr(2);

		it->second[light_id++] = level;
	}

	auto after = preset_levels_text(it->second, &current_.lights);

	if (before != after) {
		network_.report(TAG, std::string{"Preset "} + name + ": "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}

	dirty_config();
}

void Config::delete_preset(const std::string &name) {
	std::lock_guard lock{data_mutex_};
	const auto it = current_.presets.find(name);

	if (it == current_.presets.cend()) {
		return;
	}

	network_.report(TAG, std::string{"Preset "} + name + ": "
		+ quoted_string(preset_levels_text(it->second, &current_.lights))
		+ " (deleted)");

	current_.presets.erase(it);

	network_.publish(FixedConfig::mqttTopic("/preset/") + name + "/levels", "", true);
	for (const auto &group : group_names()) {
		network_.publish(FixedConfig::mqttTopic("/active/") + group + "/" + name, "", true);
	}

	dirty_config();
}

Dali::addresses_t Config::parse_light_ids(const std::string &light_ids,
		bool &idle_only) const {
	std::lock_guard lock{data_mutex_};
	std::istringstream input{light_ids};
	std::string item;
	Dali::addresses_t lights;

	idle_only = false;

	while (std::getline(input, item, ',')) {
		auto group = current_.groups_by_name.find(item);
		auto dash_idx = item.find('-');
		unsigned long begin, end;

		if (item == BUILTIN_GROUP_ALL) {
			lights.set();
			continue;
		} else if (item == BUILTIN_GROUP_IDLE) {
			idle_only = true;
			continue;
		} else if (group != current_.groups_by_name.end()) {
			for (unsigned int i = 0; i < group->second.addresses.size(); i++) {
				if (group->second.addresses[i]) {
					lights[i] = true;
				}
			}

			continue;
		} else if (dash_idx == std::string::npos) {
			if (ulong_from_string(item, begin)) {
				end = begin;
			} else {
				continue;
			}
		} else {
			std::string second = item.substr(dash_idx + 1);

			item.resize(dash_idx);

			if (!ulong_from_string(item, begin)) {
				continue;
			}

			if (!ulong_from_string(second, end)) {
				continue;
			}
		}

		if (begin > end) {
			continue;
		}

		if (begin >= Dali::num_addresses) {
			continue;
		}

		if (end >= Dali::num_addresses) {
			continue;
		}

		for (unsigned long i = begin; i <= end; i++) {
			lights[i] = true;
		}
	}

	return lights;
}

Dali::addresses_t Config::parse_groups(const std::vector<std::string> &groups) const {
	std::lock_guard lock{data_mutex_};
	Dali::addresses_t lights;

	for (const auto &item : groups) {
		auto group = current_.groups_by_name.find(item);

		if (item == BUILTIN_GROUP_ALL) {
			lights.set();
		} else if (group != current_.groups_by_name.end()) {
			for (unsigned int i = 0; i < group->second.addresses.size(); i++) {
				if (group->second.addresses[i]) {
					lights[i] = true;
				}
			}
		}
	}

	return lights;
}

std::string Config::lights_text(const Dali::addresses_t &lights) const {
	std::lock_guard lock{data_mutex_};
	std::vector<std::string> light_texts;
	std::string list = "";
	unsigned int total = 0;
	unsigned int found = 0;
	unsigned int begin = INT_MAX;
	unsigned int previous = INT_MAX;

	for (unsigned int i = 0; i < current_.lights.size(); i++) {
		if (current_.lights[i]) {
			total++;
		} else {
			continue;
		}

		if (!lights[i]) {
			continue;
		}

		if (previous != INT_MAX && previous == i - 1) {
			light_texts.pop_back();
			light_texts.push_back(std::to_string(begin) + "-" + std::to_string(i));
		} else {
			begin = i;
			light_texts.push_back(std::move(std::to_string(i)));
		}

		previous = i;
		found++;
	}

	for (const auto &light_text : light_texts) {
		if (!list.empty()) {
			list += ",";
		}

		list += light_text;
	}

	if (found == 0) {
		return "None";
	} else if (total == found) {
		return "All";
	} else if (found == 1) {
		return std::string{"Light "} + list;
	} else {
		return std::string{"Lights "} + list;
	}
}
