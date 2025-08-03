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

#if __has_include("fixed_config.h")
# include "fixed_config.h"
#else
# include "fixed_config.h.example"
#endif

static constexpr auto &FS = LittleFS;

static const std::string BUILTIN_PRESET_OFF = "off";
static constexpr size_t MAX_TEXT_LEN = 256;
static const std::string FILENAME = "/config.cbor";
static const std::string BACKUP_FILENAME = "/config.cbor~";

namespace cbor = qindesign::cbor;

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
	return addresses_text(-1);
}

std::string Config::switch_addresses_text(unsigned int switch_id) {
	if (switch_id < NUM_SWITCHES) {
		return addresses_text(get_switch_addresses(switch_id));
	} else {
		return "(null)";
	}
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

std::string Config::preset_levels_text(const std::array<int,MAX_ADDR+1> &levels, bool filter) {
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
					network_.publish((std::string{MQTT_TOPIC} + "/loaded_config").c_str(),
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
			if (!read_config_lights(reader)) {
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

bool Config::read_config_lights(cbor::Reader &reader) {
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
			current_.lights[i] = value;
			i++;
		}
	}

	ESP_LOGE("config", "Lights = %s", addresses_text(current_.lights).c_str());

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
		} else if (key == "lights") {
			if (!read_config_switch_lights(reader, switch_id)) {
				return false;
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

bool Config::read_config_switch_lights(cbor::Reader &reader, unsigned int switch_id) {
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
			current_.switches[switch_id].lights[i] = value;
			i++;
		}
	}

	ESP_LOGE("config", "Switch %u lights = %s", switch_id,
		addresses_text(current_.switches[switch_id].lights).c_str());

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
	std::array<int,MAX_ADDR+1> levels;

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

bool Config::read_config_preset_levels(cbor::Reader &reader, std::array<int,MAX_ADDR+1> &levels) {
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
			network_.publish((std::string{MQTT_TOPIC} + "/saved_config").c_str(),
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

	writer.beginMap(3);

	writeText(writer, "lights");
	writer.beginArray(MAX_ADDR+1);
	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		writer.writeBoolean(current_.lights[i]);
	}

	writeText(writer, "switches");
	writer.beginArray(NUM_SWITCHES);
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		writer.beginMap(3);

		writeText(writer, "name");
		writeText(writer, current_.switches[i].name);

		writeText(writer, "lights");
		writer.beginArray(MAX_ADDR+1);
		for (unsigned int j = 0; j <= MAX_ADDR; j++) {
			writer.writeBoolean(current_.switches[i].lights[j]);
		}

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
	network_.publish((std::string{MQTT_TOPIC} + "/addresses").c_str(),
		addresses_text(current_.lights).c_str(), true);

	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		network_.publish((std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i)
			+ "/name").c_str(), current_.switches[i].name.c_str(), true);
		network_.publish((std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i)
			+ "/addresses").c_str(), addresses_text(current_.switches[i].lights).c_str(), true);
		network_.publish((std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i)
			+ "/preset").c_str(), current_.switches[i].preset.c_str(), true);
	}

	for (const auto &preset : current_.presets) {
		publish_preset(preset.first, preset.second);
	}
}

void Config::publish_preset(const std::string &name, const std::array<int,MAX_ADDR+1> &levels) {
	network_.publish((std::string{MQTT_TOPIC} + "/preset/" + name + "/levels").c_str(),
		preset_levels_text(levels, false).c_str(), true);
}

std::bitset<MAX_ADDR+1> Config::get_addresses() {
	return current_.lights;
}

std::bitset<MAX_ADDR+1> Config::get_switch_addresses(unsigned int switch_id) {
	if (switch_id < NUM_SWITCHES) {
		return current_.switches[switch_id].lights;
	} else {
		return {};
	}
}

void Config::set_addresses(std::string addresses) {
	set_addresses(-1, addresses);
}

void Config::set_switch_addresses(unsigned int switch_id, std::string addresses) {
	if (switch_id < NUM_SWITCHES) {
		set_addresses(switch_id, addresses);
	}
}

void Config::set_addresses(int switch_id, std::string addresses) {
	auto before = addresses_text(switch_id);

	if (switch_id == -1) {
		current_.lights.reset();
	} else if (switch_id < NUM_SWITCHES) {
		current_.switches[switch_id].lights.reset();
	} else {
		return;
	}

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
			if (switch_id == -1) {
				current_.lights[address] = true;
			} else {
				current_.switches[switch_id].lights[address] = true;
			}
		}

		addresses = addresses.substr(2);
	}

	auto after = addresses_text(switch_id);

	save_config();

	if (before != after) {
		if (switch_id == -1) {
			ESP_LOGE("lights", "Configure light addresses: %s", addresses.c_str());
			network_.report("lights", std::string{"Addresses: "} + before + " -> " + after);
		} else {
			ESP_LOGE("lights", "Configure light switch %d addresses: %s", switch_id, addresses.c_str());
			network_.report("lights", std::string{"Switch "} + std::to_string(switch_id) + " addresses: " + before + " -> " + after);
		}
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
				+ current_.switches[switch_id].name + " -> " + new_name);

			current_.switches[switch_id].name = new_name;
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
		auto new_preset = preset.substr(0, MAX_SWITCH_NAME_LEN);

		if (current_.switches[switch_id].preset != new_preset) {
			network_.report("switch", std::string{"Switch "}
				+ std::to_string(switch_id) + " preset: "
				+ current_.switches[switch_id].preset + " -> " + new_preset);

			current_.switches[switch_id].preset = new_preset;
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

bool Config::get_preset(const std::string &name, std::array<int,MAX_ADDR+1> &levels) {
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

		std::array<int,MAX_ADDR+1> levels;

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

	network_.report("presets", std::string{"Preset "} + name + ": " + lights_text(light_ids) + " = " + std::to_string(level));

	if (before != after) {
		network_.report("presets", std::string{"Preset "} + name + ": " + before + " -> " + after);
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

		std::array<int,MAX_ADDR+1> empty_levels;

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
		network_.report("presets", std::string{"Preset "} + name + ": " + before + " -> " + after);
	}
}

void Config::delete_preset(const std::string &name) {
	const auto it = current_.presets.find(name);

	if (it == current_.presets.cend()) {
		return;
	}

	network_.report("presets", std::string{"Preset "} + name + ": " + preset_levels_text(it->second, true) + " (deleted)");

	current_.presets.erase(it);
	network_.publish((std::string{MQTT_TOPIC} + "/preset/" + name + "/active").c_str(), "", true);
	network_.publish((std::string{MQTT_TOPIC} + "/preset/" + name + "/levels").c_str(), "", true);
}


std::set<unsigned int> Config::parse_light_ids(const std::string &light_id) {
	std::istringstream input{light_id};
	std::string item;
	std::set<unsigned int> light_ids;

	while (std::getline(input, item, ',')) {
		auto dash_idx = item.find('-');
		unsigned long begin, end;

		if (item == "all") {
			begin = 0;
			end = MAX_ADDR;
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

	return prefix + list;
}
