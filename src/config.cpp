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
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "dali.h"
#include "lights.h"
#include "network.h"
#include "switches.h"
#include "util.h"

static constexpr auto &FS = LittleFS;

static const std::string RESERVED_GROUP_LEVELS = "levels";
static const std::string RESERVED_GROUP_DELETE = "delete";
static constexpr size_t MAX_TEXT_LEN = 256;
static const std::string FILENAME = "/config.cbor";
static const std::string BACKUP_FILENAME = "/config.cbor~";

namespace cbor = qindesign::cbor;

static std::string quoted_string(const std::string &text) {
	if (text.empty()) {
		return "`(null)`";
	} else {
		return std::string{"`"} + text + "`";
	}
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

Config::Config(Network &network) : network_(network) {
}

void Config::setup() {
	FS.begin(true);
	load_config();
}

bool Config::valid_group_name(const std::string &name) {
	if (name == BUILTIN_GROUP_ALL
			|| name == RESERVED_GROUP_LEVELS
			|| name == RESERVED_GROUP_DELETE
			|| name.empty()
			|| name.length() > MAX_GROUP_NAME_LEN) {
		return false;
	}

	for (size_t i = 0; i < name.length(); i++) {
		if (name[i] >= '0' && name[i] <= '9') {
			if (i == 0) {
				return false;
			}

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

bool Config::valid_preset_name(const std::string &name) {
	if (name == BUILTIN_PRESET_OFF
			|| name == RESERVED_PRESET_CUSTOM
			|| name == RESERVED_PRESET_UNKNOWN
			|| name.empty()
			|| name.length() > MAX_PRESET_NAME_LEN) {
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

std::string Config::addresses_text() {
	return addresses_text(get_addresses());
}

std::string Config::group_addresses_text(const std::string &group) {
	return addresses_text(get_group_addresses(group));
}

std::string Config::addresses_text(const std::bitset<MAX_ADDR+1> &addresses) {
	std::vector<char> buffer(2 * (MAX_ADDR + 1) + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (addresses[i]) {
			snprintf(&buffer[offset], 3, "%02X", (unsigned int)(i & 0xFF));
			offset += 2;
		}
	}

	if (!offset) {
		return "(null)";
	}

	return {buffer.data(), offset};
}

std::string Config::preset_levels_text(const std::array<int16_t,MAX_ADDR+1> &levels, bool filter) {
	std::vector<char> buffer(2 * (MAX_ADDR + 1) + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (!filter || current_.lights[i]) {
			snprintf(&buffer[offset], 3, "%02X", (unsigned int)(levels[i] & 0xFF));
			offset += 2;
		}
	}

	if (!offset) {
		return "(null)";
	}

	return {buffer.data(), offset};
}

void Config::load_config() {
	if (!read_config(FILENAME, true)) {
		read_config(BACKUP_FILENAME, true);
		save_config();
	} else {
		last_saved_ = current_;
		saved_ = true;
	}
}

bool Config::read_config(const std::string &filename, bool load) {
	ESP_LOGE("config", "Reading config file %s", filename.c_str());
	const char mode[2] = {'r', '\0'};
	auto file = FS.open(filename.c_str(), mode);
	if (file) {
		cbor::Reader reader{file};

		if (!cbor::expectValue(reader, cbor::DataType::kTag, cbor::kSelfDescribeTag)
				|| !reader.isWellFormed()) {
			ESP_LOGE("config", "Failed to parse config file %s", filename.c_str());
			return false;
		} else {
			if (load) {
				ESP_LOGE("config", "Loading config from file %s", filename.c_str());
				file.seek(0);

				if (!cbor::expectValue(reader, cbor::DataType::kTag, cbor::kSelfDescribeTag))
					return false;

				if (read_config(reader)) {
					ESP_LOGE("config", "Loaded config from file %s", filename.c_str());
					network_.publish(std::string{MQTT_TOPIC} + "/loaded_config",
						filename + " " + std::to_string(file.size()));
				} else {
					ESP_LOGE("config", "Invalid config file %s", filename.c_str());
				}

			}
			return true;
		}
	} else {
		ESP_LOGE("config", "Config file %s does not exist", filename.c_str());
		return false;
	}
}

bool Config::read_config(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;

	current_ = {};

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	while (length-- > 0) {
		std::string key;

		if (!readText(reader, key, UINT8_MAX)) {
			return false;
		}

		if (key == "lights") {
			if (!read_config_lights(reader, current_.lights)) {
				return false;
			}

			ESP_LOGE("config", "Lights = %s", addresses_text(current_.lights).c_str());
		} else if (key == "groups") {
			if (!read_config_groups(reader)) {
				return false;
			}
		} else if (key == "switches") {
			if (!read_config_switches(reader)) {
				return false;
			}
		} else if (key == "presets") {
			if (!read_config_presets(reader)) {
				return false;
			}
		} else {
			ESP_LOGE("config", "Unknown key: %s", key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool Config::read_config_lights(cbor::Reader &reader, std::bitset<MAX_ADDR+1> &lights) {
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

		if (i <= MAX_ADDR) {
			lights[i] = value;
			i++;
		}
	}

	return true;
}

bool Config::read_config_groups(cbor::Reader &reader) {
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

	return true;
}

bool Config::read_config_group(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	std::string name;
	std::bitset<MAX_ADDR+1> lights;

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
		} else if (key == "lights") {
			if (!read_config_lights(reader, lights)) {
				return false;
			}
		} else {
			ESP_LOGE("config", "Unknown group key: %s", key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	if (valid_group_name(name)) {
		auto result = current_.groups.emplace(name, std::move(lights));

		if (result.second) {
			ESP_LOGE("config", "Group %s = %s", name.c_str(), group_addresses_text(name).c_str());
		} else {
			ESP_LOGE("config", "Ignoring duplicate group: %s", name.c_str());
		}
	} else {
		ESP_LOGE("config", "Ignoring invalid group: %s", name.c_str());
	}

	return true;
}

bool Config::read_config_switches(cbor::Reader &reader) {
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

bool Config::read_config_switch(cbor::Reader &reader, unsigned int switch_id) {
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

			ESP_LOGE("config", "Switch %u name = %s", switch_id, name.c_str());
			current_.switches[switch_id].name = name;
		} else if (key == "group") {
			std::string group;

			if (!readText(reader, group, UINT8_MAX)) {
				return false;
			}

			if (valid_group_name(group)) {
				ESP_LOGE("config", "Switch %u group = %s", switch_id, group.c_str());
				current_.switches[switch_id].group = group;
			} else {
				ESP_LOGE("config", "Switch %u invalid group ignored: %s", switch_id, group.c_str());
			}
		} else if (key == "preset") {
			std::string preset;

			if (!readText(reader, preset, UINT8_MAX)) {
				return false;
			}

			if (valid_preset_name(preset)) {
				ESP_LOGE("config", "Switch %u preset = %s", switch_id, preset.c_str());
				current_.switches[switch_id].preset = preset;
			} else {
				ESP_LOGE("config", "Switch %u invalid preset ignored: %s", switch_id, preset.c_str());
			}
		} else {
			ESP_LOGE("config", "Unknown switch %u key: %s", switch_id, key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	return true;
}

bool Config::read_config_presets(cbor::Reader &reader) {
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

bool Config::read_config_preset(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;
	std::string name;
	std::array<int16_t,MAX_ADDR+1> levels;

	if (!cbor::expectMap(reader, &length, &indefinite) || indefinite) {
		return false;
	}

	levels.fill(-1);

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
			ESP_LOGE("config", "Unknown preset key: %s", key.c_str());

			if (!reader.isWellFormed()) {
				return false;
			}
		}
	}

	if (valid_preset_name(name)) {
		auto result = current_.presets.emplace(name, std::move(levels));

		if (result.second) {
			ESP_LOGE("config", "Preset %s = %s", name.c_str(), preset_levels_text(result.first->second, false).c_str());
		} else {
			ESP_LOGE("config", "Ignoring duplicate preset: %s", name.c_str());
		}
	} else {
		ESP_LOGE("config", "Ignoring invalid preset: %s", name.c_str());
	}

	return true;
}

bool Config::read_config_preset_levels(cbor::Reader &reader, std::array<int16_t,MAX_ADDR+1> &levels) {
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

		if (i <= MAX_ADDR) {
			if (value >= -1 && value <= MAX_LEVEL) {
				levels[i] = value;
			}
			i++;
		}
	}

	return true;
}

void Config::save_config() {
	if (saved_ && current_ == last_saved_) {
		return;
	}

	if (write_config(FILENAME)) {
		if (read_config(FILENAME, false)) {
			if (write_config(BACKUP_FILENAME)) {
				last_saved_ = current_;
				saved_ = true;
			}
		}
	}
}

bool Config::write_config(const std::string &filename) {
	ESP_LOGE("config", "Writing config file %s", filename.c_str());
	{
		const char mode[2] = {'w', '\0'};
		auto file = FS.open(filename.c_str(), mode);
		if (file) {
			cbor::Writer writer{file};

			writer.writeTag(cbor::kSelfDescribeTag);
			write_config(writer);

			if (file.getWriteError()) {
				network_.report("config", std::string{"Failed to write config file "} + filename
						+ ": " + std::to_string(file.getWriteError()));
				return false;
			}
		} else {
			network_.report("config", std::string{"Unable to open config file "} + filename + " for writing");
			return false;
		}
	}
	{
		const char mode[2] = {'r', '\0'};
		auto file = FS.open(filename.c_str(), mode);
		if (file) {
			ESP_LOGE("config", "Saved config to file %s", filename.c_str());
			network_.publish(std::string{MQTT_TOPIC} + "/saved_config",
				filename + " " + std::to_string(file.size()));
			return true;
		} else {
			network_.report("config", std::string{"Unable to open config file "} + filename + " for reading");
			return false;
		}
	}
}

void Config::write_config(cbor::Writer &writer) {
	std::string key;

	writer.beginMap(4);

	writeText(writer, "lights");
	writer.beginArray(MAX_ADDR+1);
	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		writer.writeBoolean(current_.lights[i]);
	}

	writeText(writer, "groups");
	writer.beginArray(current_.groups.size());
	for (const auto &group : current_.groups) {
		writer.beginMap(2);

		writeText(writer, "name");
		writeText(writer, group.first);

		writeText(writer, "lights");
		writer.beginArray(MAX_ADDR+1);
		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			writer.writeBoolean(group.second[i]);
		}
	}

	writeText(writer, "switches");
	writer.beginArray(NUM_SWITCHES);
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		writer.beginMap(3);

		writeText(writer, "name");
		writeText(writer, current_.switches[i].name);

		writeText(writer, "group");
		writeText(writer, current_.switches[i].group);

		writeText(writer, "preset");
		writeText(writer, current_.switches[i].preset);
	}

	writeText(writer, "presets");
	writer.beginArray(current_.presets.size());
	for (const auto &preset : current_.presets) {
		writer.beginMap(2);

		writeText(writer, "name");
		writeText(writer, preset.first);

		writeText(writer, "levels");
		writer.beginArray(MAX_ADDR+1);
		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			writer.writeInt(preset.second[i]);
		}
	}
}

void Config::publish_config() {
	network_.publish(std::string{MQTT_TOPIC} + "/addresses",
		addresses_text(current_.lights), true);

	for (const auto &group : current_.groups) {
		network_.publish(std::string{MQTT_TOPIC} + "/group/" + group.first,
			addresses_text(group.second), true);
	}

	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		auto switch_prefix = std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i);

		network_.publish(switch_prefix + "/name", current_.switches[i].name, true);
		network_.publish(switch_prefix + "/group", current_.switches[i].group, true);
		network_.publish(switch_prefix + "/preset", current_.switches[i].preset, true);
	}

	for (const auto &preset : current_.presets) {
		publish_preset(preset.first, preset.second);
	}
}

void Config::publish_preset(const std::string &name, const std::array<int16_t,MAX_ADDR+1> &levels) {
	network_.publish(std::string{MQTT_TOPIC} + "/preset/" + name + "/levels",
		preset_levels_text(levels, false), true);
}

std::bitset<MAX_ADDR+1> Config::get_addresses() {
	return get_group_addresses(BUILTIN_GROUP_ALL);
}

std::unordered_set<std::string> Config::group_names() {
	std::unordered_set<std::string> all(MAX_GROUPS + 1);

	all.insert(BUILTIN_GROUP_ALL);

	for (const auto &groups : current_.groups) {
		all.insert(groups.first);
	}

	return all;
}

std::bitset<MAX_ADDR+1> Config::get_group_addresses(const std::string &group) {
	if (group == BUILTIN_GROUP_ALL) {
		return current_.lights;
	} else {
		auto it = current_.groups.find(group);

		if (it == current_.groups.end()) {
			return {};
		}

		return it->second;
	}
}

void Config::set_addresses(std::string addresses) {
	set_addresses(BUILTIN_GROUP_ALL, addresses);
}

void Config::set_group_addresses(const std::string &name, std::string addresses) {
	if (!valid_group_name(name)) {
		return;
	}

	set_addresses(name, addresses);
}

void Config::set_addresses(const std::string &group, std::string addresses) {
	std::bitset<MAX_ADDR+1> lights;

	auto before = group_addresses_text(group);

	while (addresses.length() >= 2) {
		unsigned int address = 0;

		if (addresses[0] >= '0' && addresses[0] <= '9') {
			address |= (addresses[0] - '0') << 4;
		} else if (addresses[0] >= 'A' && addresses[0] <= 'F') {
			address |= (addresses[0] - 'A' + 10) << 4;
		}

		if (addresses[1] >= '0' && addresses[1] <= '9') {
			address |= addresses[1] - '0';
		} else if (addresses[1] >= 'A' && addresses[1] <= 'F') {
			address |= addresses[1] - 'A' + 10;
		}

		if (address <= MAX_ADDR) {
			lights[address] = true;
		}

		addresses = addresses.substr(2);
	}

	if (group == BUILTIN_GROUP_ALL) {
		current_.lights = lights;
	} else {
		auto it = current_.groups.find(group);

		if (it == current_.groups.end()) {
			if (current_.groups.size() == MAX_GROUPS) {
				return;
			}

			current_.groups.emplace(group, std::move(lights));
		} else {
			it->second = lights;
		}
	}

	auto after = group_addresses_text(group);

	save_config();

	if (before != after) {
		if (group == BUILTIN_GROUP_ALL) {
			ESP_LOGE("lights", "Configure light addresses: %s", addresses.c_str());
			network_.publish(std::string{MQTT_TOPIC} + "/addresses", after, true);
			network_.report("lights", std::string{"Addresses: "}
				+ quoted_string(before) + " -> " + quoted_string(after));
		} else {
			ESP_LOGE("lights", "Configure group %s addresses: %s", group.c_str(), addresses.c_str());
			network_.publish(std::string{MQTT_TOPIC} + "/group/" + group, after, true);
			network_.report("lights", std::string{"Group "} + group + " addresses: "
				+ quoted_string(before) + " -> " + quoted_string(after));
		}
	}
}

void Config::delete_group(const std::string &name) {
	const auto it = current_.groups.find(name);

	if (it == current_.groups.cend()) {
		return;
	}

	network_.report("groups", std::string{"Group "} + name + ": "
		+ quoted_string(group_addresses_text(name)) + " (deleted)");

	current_.groups.erase(it);
	network_.publish(std::string{MQTT_TOPIC} + "/group/" + name, "", true);
	for (const auto &preset : current_.presets) {
		network_.publish(std::string{MQTT_TOPIC} + "/preset/" + name + "/" + preset.first + "/active", "", true);
	}
}

std::string Config::get_switch_name(unsigned int switch_id) {
	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].name;
	} else {
		return "";
	}
}

void Config::set_switch_name(unsigned int switch_id, const std::string &name) {
	if (switch_id < NUM_SWITCHES) {
		auto new_name = name.substr(0, MAX_SWITCH_NAME_LEN);

		if (current_.switches[switch_id].name != new_name) {
			network_.report("switch", std::string{"Switch "}
				+ std::to_string(switch_id) + " name: "
				+ quoted_string(current_.switches[switch_id].name)
				+ " -> " + quoted_string(new_name));

			current_.switches[switch_id].name = new_name;
			save_config();
		}
	}
}

std::string Config::get_switch_group(unsigned int switch_id) {
	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].group;
	} else {
		return "";
	}
}

void Config::set_switch_group(unsigned int switch_id, const std::string &group) {
	if (switch_id < NUM_SWITCHES) {
		if (!valid_group_name(group)) {
			return;
		}

		if (current_.switches[switch_id].group != group) {
			network_.report("switch", std::string{"Switch "}
				+ std::to_string(switch_id) + " group: "
				+ quoted_string(current_.switches[switch_id].group)
				+ " -> " + quoted_string(group));

			current_.switches[switch_id].group = group;
			save_config();
		}
	}
}

std::string Config::get_switch_preset(unsigned int switch_id) {
	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].preset;
	} else {
		return "";
	}
}

void Config::set_switch_preset(unsigned int switch_id, const std::string &preset) {
	if (switch_id < NUM_SWITCHES) {
		if (!valid_preset_name(preset)) {
			return;
		}

		if (current_.switches[switch_id].preset != preset) {
			network_.report("switch", std::string{"Switch "}
				+ std::to_string(switch_id) + " preset: "
				+ quoted_string(current_.switches[switch_id].preset)
				+ " -> " + quoted_string(preset));

			current_.switches[switch_id].preset = preset;
			save_config();
		}
	}
}

std::unordered_set<std::string> Config::preset_names() {
	std::unordered_set<std::string> all(MAX_PRESETS + 3);

	all.insert(BUILTIN_PRESET_OFF);
	all.insert(RESERVED_PRESET_CUSTOM);
	all.insert(RESERVED_PRESET_UNKNOWN);

	for (const auto &preset : current_.presets) {
		all.insert(preset.first);
	}

	return all;
}

bool Config::get_preset(const std::string &name, std::array<int16_t,MAX_ADDR+1> &levels) {
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

void Config::set_preset(const std::string &name, const std::string &lights, long level) {
	if (level < -1 || level > MAX_LEVEL) {
		return;
	}

	if (!valid_preset_name(name)) {
		return;
	}

	auto light_ids = parse_light_ids(lights);
	auto it = current_.presets.find(name);

	if (it == current_.presets.cend()) {
		if (current_.presets.size() == MAX_PRESETS) {
			return;
		}

		std::array<int16_t,MAX_ADDR+1> levels;

		levels.fill(-1);
		it = current_.presets.emplace(name, std::move(levels)).first;
	}

	auto before = preset_levels_text(it->second, true);

	for (unsigned int light_id : light_ids) {
		if (current_.lights[light_id]) {
			it->second[light_id] = level;
		}
	}

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (!current_.lights[i]) {
			it->second[i] = -1;
		}
	}

	auto after = preset_levels_text(it->second, true);

	save_config();

	if (before != after) {
		publish_preset(it->first, it->second);
	}

	network_.report("presets", std::string{"Preset "} + name + ": "
		+ lights_text(light_ids) + " = " + std::to_string(level));

	if (before != after) {
		network_.report("presets", std::string{"Preset "} + name + ": "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}
}

void Config::set_preset(const std::string &name, std::string levels) {
	if (!valid_preset_name(name)) {
		return;
	}

	auto it = current_.presets.find(name);

	if (it == current_.presets.cend()) {
		if (current_.presets.size() == MAX_PRESETS) {
			return;
		}

		std::array<int16_t,MAX_ADDR+1> empty_levels;

		empty_levels.fill(-1);
		it = current_.presets.emplace(name, std::move(empty_levels)).first;
	}

	auto before = preset_levels_text(it->second, true);
	unsigned int light_id = 0;

	it->second.fill(-1);

	while (levels.length() >= 2 && light_id <= MAX_ADDR) {
		uint8_t level = 0;

		if (levels[0] >= '0' && levels[0] <= '9') {
			level |= (levels[0] - '0') << 4;
		} else if (levels[0] >= 'A' && levels[0] <= 'F') {
			level |= (levels[0] - 'A' + 10) << 4;
		}

		if (levels[1] >= '0' && levels[1] <= '9') {
			level |= levels[1] - '0';
		} else if (levels[1] >= 'A' && levels[1] <= 'F') {
			level |= levels[1] - 'A' + 10;
		}

		levels = levels.substr(2);

		it->second[light_id++] = (level == 0xFF ? -1 : level);
	}

	auto after = preset_levels_text(it->second, true);

	save_config();

	if (before != after) {
		publish_preset(it->first, it->second);
		network_.report("presets", std::string{"Preset "} + name + ": "
			+ quoted_string(before) + " -> " + quoted_string(after));
	}
}

void Config::delete_preset(const std::string &name) {
	const auto it = current_.presets.find(name);

	if (it == current_.presets.cend()) {
		return;
	}

	network_.report("presets", std::string{"Preset "} + name + ": "
		+ quoted_string(preset_levels_text(it->second, true))
		+ " (deleted)");

	current_.presets.erase(it);
	network_.publish(std::string{MQTT_TOPIC} + "/preset/" + name + "/active", "", true);
	network_.publish(std::string{MQTT_TOPIC} + "/preset/" + name + "/levels", "", true);
}

std::set<unsigned int> Config::parse_light_ids(const std::string &light_id) {
	std::istringstream input{light_id};
	std::string item;
	std::set<unsigned int> light_ids;

	while (std::getline(input, item, ',')) {
		auto group = current_.groups.find(item);
		auto dash_idx = item.find('-');
		unsigned long begin, end;

		if (item == BUILTIN_GROUP_ALL) {
			begin = 0;
			end = MAX_ADDR;
		} else if (group != current_.groups.end()) {
			for (unsigned int i = 0; i <= MAX_ADDR; i++) {
				if (group->second[i]) {
					light_ids.insert(i);
				}
			}

			continue;
		} else if (dash_idx == std::string::npos) {
			char *endptr = nullptr;

			begin = end = std::strtoul(item.c_str(), &endptr, 10);
			if (item.empty() || !endptr || endptr[0]) {
				continue;
			}
		} else {
			std::string second = item.substr(dash_idx + 1);

			item.resize(dash_idx);

			{
				char *endptr = nullptr;

				begin = std::strtoul(item.c_str(), &endptr, 10);
				if (item.empty() || !endptr || endptr[0]) {
					continue;
				}
			}

			{
				char *endptr = nullptr;

				end = std::strtoul(second.c_str(), &endptr, 10);
				if (second.empty() || !endptr || endptr[0]) {
					continue;
				}
			}
		}

		if (begin > end) {
			continue;
		}

		if (begin > MAX_ADDR) {
			continue;
		}

		if (end > MAX_ADDR) {
			continue;
		}

		for (unsigned long i = begin; i <= end; i++) {
			light_ids.insert(i);
		}
	}

	return light_ids;
}

std::string Config::lights_text(const std::set<unsigned int> &light_ids) {
	std::string prefix = "Light ";
	std::string list = "";
	unsigned int total = 0;
	unsigned int found = 0;

	for (unsigned int i = 0; i < MAX_ADDR; i++) {
		if (current_.lights[i]) {
			total++;
		}
	}

	for (int light_id : light_ids) {
		if (!current_.lights[light_id]) {
			continue;
		}

		if (!list.empty()) {
			prefix = "Lights ";
			list += ",";
		}

		list += std::to_string(light_id);
		found++;
	}

	if (total == found) {
		prefix = "All";
		list = "";
	}

	if (found == 0) {
		return "None";
	}

	return prefix + list;
}
