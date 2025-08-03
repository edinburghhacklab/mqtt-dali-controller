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

#include <Arduino.h>
#include <esp_timer.h>
#include <CBOR.h>
#include <CBOR_parsing.h>
#include <CBOR_streams.h>
#include <FS.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if __has_include("config.h")
# include "config.h"
#else
# include "config.h.example"
#endif

static constexpr auto &FS = LittleFS;

static constexpr uint8_t MAX_ADDR = 63;
static constexpr uint8_t MAX_LEVEL = 254;
static constexpr uint64_t ONE_S = 1000 * 1000ULL;
static constexpr uint64_t ONE_M = 60 * ONE_S;
static constexpr unsigned int NUM_SWITCHES = 2;
static constexpr size_t MAX_PRESETS = 50;
static constexpr size_t MAX_PRESET_NAME_LEN = 50;
static constexpr size_t MAX_SWITCH_NAME_LEN = 50;
static constexpr size_t MAX_TEXT_LEN = 256;
static constexpr std::array<unsigned int,NUM_SWITCHES> SWITCH_GPIO = {11, 12};
static constexpr unsigned int LED_GPIO = 38;
static constexpr unsigned int RX_GPIO = 40;
static constexpr unsigned int TX_GPIO = 21;
static const std::string BUILTIN_PRESET_OFF = "off";
static const std::string RESERVED_PRESET_CUSTOM = "custom";
static const std::string RESERVED_PRESET_UNKNOWN = "unknown";
static const std::string filename = "/config.cbor";
static const std::string backup_filename = "/config.cbor~";

struct SwitchState {
	SwitchState() : value(LOW), report_us(0) {}

	int value;
	uint64_t report_us;
};

static bool startup_complete = false;
static std::array<SwitchState,NUM_SWITCHES> switch_state;

static uint64_t last_wifi_us = 0;
static bool wifi_up = false;

static uint64_t last_mqtt_us = 0;

static WiFiClient client;
static PubSubClient mqtt(client);
static String device_id;

struct SwitchConfig {
	std::string name;
	std::array<bool,MAX_ADDR+1> lights;
	std::string preset;

	bool operator==(const SwitchConfig &other) const {
		return this->name == other.name
			&& this->lights == other.lights
			&& this->preset == other.preset;
	}

	inline bool operator!=(const SwitchConfig &other) const { return !(*this == other); }
};

struct Config {
	std::array<bool,MAX_ADDR+1> lights;
	std::array<SwitchConfig,NUM_SWITCHES> switches;
	std::unordered_map<std::string, std::array<int,MAX_ADDR+1>> presets;

	bool operator==(const Config &other) const {
		return this->lights == other.lights
			&& this->switches == other.switches
			&& this->presets == other.presets;
	}

	inline bool operator!=(const Config &other) const { return !(*this == other); }
};

static Config current_config{};
static Config saved_config{};
static bool config_saved = false;

static std::array<uint8_t,MAX_ADDR+1> levels{};
static std::array<uint8_t,MAX_ADDR+1> tx_levels{};
static std::array<std::string,MAX_ADDR+1> active_presets{};
static std::unordered_set<std::string> last_active_presets{};
static bool republish_active_presets = true;
static uint64_t last_published_active_presets_us = 0;

namespace cbor = qindesign::cbor;

static bool valid_preset_name(const std::string &name) {
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

static std::string addresses_text(int switch_id) {
	std::vector<char> buffer(2 * (MAX_ADDR + 1) + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (switch_id == -1
				? current_config.lights[i]
					: current_config.switches[switch_id].lights[i]) {
			snprintf(&buffer[offset], 3, "%02X", (unsigned int)(i & 0xFF));
			offset += 2;
		}
	}

	if (!offset) {
		return "(null)";
	}

	return {buffer.data(), offset};
}

static std::string preset_levels_text(const std::array<int,MAX_ADDR+1> &levels, bool filter) {
	std::vector<char> buffer(2 * (MAX_ADDR + 1) + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (!filter || current_config.lights[i]) {
			snprintf(&buffer[offset], 3, "%02X", (unsigned int)(levels[i] & 0xFF));
			offset += 2;
		}
	}

	if (!offset) {
		return "(null)";
	}

	return {buffer.data(), offset};
}

static void json_append_escape(std::string &output, const std::string_view value) {
	for (size_t i = 0; i < value.length(); i++) {
		if (value[i] == '"' || value[i] == '\\') {
			output += '\\';
		}
		output += value[i];
	}
}

static void report(const char *tag, const std::string &message) {
	ESP_LOGE(tag, "%s", message.c_str());

	if (wifi_up && mqtt.connected() && IRC_CHANNEL[0]) {
		std::string payload;

		payload.reserve(MQTT_MAX_PACKET_SIZE);
		payload += "{\"to\": \"";
		json_append_escape(payload, IRC_CHANNEL);
		payload += "\", \"message\": \"";
		json_append_escape(payload, MQTT_TOPIC);
		json_append_escape(payload, ": ");
		json_append_escape(payload, message);
		payload += + "\"}";

		mqtt.publish("irc/send", payload.c_str());
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

static bool read_config_lights(cbor::Reader &reader) {
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
			current_config.lights[i] = value;
			i++;
		}
	}

	ESP_LOGE("config", "Lights = %s", addresses_text(-1).c_str());

	return true;
}

static bool read_config_switch_lights(cbor::Reader &reader, unsigned int switch_id) {
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
			current_config.switches[switch_id].lights[i] = value;
			i++;
		}
	}

	ESP_LOGE("config", "Switch %u lights = %s", switch_id, addresses_text(switch_id).c_str());

	return true;
}

static bool read_config_switch(cbor::Reader &reader, unsigned int switch_id) {
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
			current_config.switches[switch_id].name = name;
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
				current_config.switches[switch_id].preset = preset;
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

static bool read_config_switches(cbor::Reader &reader) {
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

static bool read_config_preset_levels(cbor::Reader &reader, std::array<int,MAX_ADDR+1> &levels) {
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

static bool read_config_preset(cbor::Reader &reader) {
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
		auto result = current_config.presets.emplace(name, std::move(levels));

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

static bool read_config_presets(cbor::Reader &reader) {
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

static bool read_config(cbor::Reader &reader) {
	uint64_t length;
	bool indefinite;

	current_config = {};

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

static bool read_config(const std::string &filename, bool load) {
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

static void load_config() {
	if (!read_config(filename, true)) {
		read_config(backup_filename, true);
	} else {
		saved_config = current_config;
		config_saved = true;
	}
}

static void writeText(cbor::Writer &writer, const std::string &value) {
	size_t length = std::min(value.length(), MAX_TEXT_LEN);

	writer.beginText(length);
	writer.writeBytes(reinterpret_cast<const uint8_t*>(value.c_str()), length);
}

static void write_config(cbor::Writer &writer) {
	std::string key;

	writer.beginMap(3);

	writeText(writer, "lights");
	writer.beginArray(MAX_ADDR+1);
	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		writer.writeBoolean(current_config.lights[i]);
	}

	writeText(writer, "switches");
	writer.beginArray(NUM_SWITCHES);
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		writer.beginMap(3);

		writeText(writer, "name");
		writeText(writer, current_config.switches[i].name);

		writeText(writer, "lights");
		writer.beginArray(MAX_ADDR+1);
		for (unsigned int j = 0; j <= MAX_ADDR; j++) {
			writer.writeBoolean(current_config.switches[i].lights[j]);
		}

		writeText(writer, "preset");
		writeText(writer, current_config.switches[i].preset);
	}

	writeText(writer, "presets");
	writer.beginArray(current_config.presets.size());
	for (const auto &preset : current_config.presets) {
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

static bool write_config(const std::string &filename) {
	ESP_LOGE("config", "Writing config file %s", filename.c_str());
	const char mode[2] = {'w', '\0'};
	auto file = FS.open(filename.c_str(), mode);
	if (file) {
		cbor::Writer writer{file};

		writer.writeTag(cbor::kSelfDescribeTag);
		write_config(writer);

		if (file.getWriteError()) {
			report("config", std::string{"Failed to write config file "} + filename
					+ ": " + std::to_string(file.getWriteError()));
			return false;
		} else {
			return true;
		}
	} else {
		report("config", std::string{"Unable to open config file "} + filename + " for writing");
		return false;
	}
}

static void save_config() {
	if (!startup_complete) {
		return;
	}

	if (config_saved && saved_config == current_config) {
		return;
	}

	if (write_config(filename)) {
		if (read_config(filename, false)) {
			if (write_config(backup_filename)) {
				saved_config = current_config;
				config_saved = true;
			}
		}
	}
}

static void publish_preset(const std::string &name, const std::array<int,MAX_ADDR+1> &levels) {
	mqtt.publish((std::string{MQTT_TOPIC} + "/preset/" + name + "/levels").c_str(),
		preset_levels_text(levels, false).c_str(), true);
}

static void publish_config() {
	mqtt.publish((std::string{MQTT_TOPIC} + "/addresses").c_str(),
		addresses_text(-1).c_str(), true);

	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		mqtt.publish((std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i)
			+ "/name").c_str(), current_config.switches[i].name.c_str(), true);
		mqtt.publish((std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i)
			+ "/addresses").c_str(), addresses_text(i).c_str(), true);
		mqtt.publish((std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i)
			+ "/preset").c_str(), current_config.switches[i].preset.c_str(), true);
	}

	for (const auto &preset : current_config.presets) {
		publish_preset(preset.first, preset.second);
	}
}

static void configure_addresses(int switch_id, std::string addresses) {
	auto before = addresses_text(switch_id);

	if (switch_id == -1) {
		current_config.lights.fill(false);
	} else {
		current_config.switches[switch_id].lights.fill(false);
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
				current_config.lights[address] = true;
			} else {
				current_config.switches[switch_id].lights[address] = true;
			}
		}

		addresses = addresses.substr(2);
	}

	auto after = addresses_text(switch_id);

	save_config();
	republish_active_presets = true;

	if (before != after) {
		if (switch_id == -1) {
			ESP_LOGE("lights", "Configure light addresses: %s", addresses.c_str());
			report("lights", std::string{"Addresses: "} + before + " -> " + after);
		} else {
			ESP_LOGE("lights", "Configure light switch %d addresses: %s", switch_id, addresses.c_str());
			report("lights", std::string{"Switch "} + std::to_string(switch_id) + " addresses: " + before + " -> " + after);
		}
	}
}

static std::set<unsigned int> parse_light_ids(const std::string &light_id) {
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

static std::string list_lights(const std::set<unsigned int> &light_ids) {
	std::string prefix = "Light ";
	std::string list = "";
	unsigned int total = 0;
	unsigned int found = 0;

	for (unsigned int i = 0; i < MAX_ADDR; i++) {
		if (current_config.lights[i]) {
			total++;
		}
	}

	for (int light_id : light_ids) {
		if (!current_config.lights[light_id]) {
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

static void configure_preset(const std::string &name, const std::string &lights, long level) {
	if (level < -1 || level > MAX_LEVEL) {
		return;
	}

	if (!valid_preset_name(name)) {
		return;
	}

	auto light_ids = parse_light_ids(lights);
	auto it = current_config.presets.find(name);

	if (it == current_config.presets.cend()) {
		if (current_config.presets.size() == MAX_PRESETS) {
			return;
		}

		std::array<int,MAX_ADDR+1> levels;

		levels.fill(-1);
		it = current_config.presets.emplace(name, std::move(levels)).first;
	}

	auto before = preset_levels_text(it->second, true);

	for (unsigned int light_id : light_ids) {
		if (current_config.lights[light_id]) {
			it->second[light_id] = level;
		}
	}

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (!current_config.lights[i]) {
			it->second[i] = -1;
		}
	}

	auto after = preset_levels_text(it->second, true);

	save_config();

	if (before != after) {
		publish_preset(it->first, it->second);
	}

	report("presets", std::string{"Preset "} + name + ": " + list_lights(light_ids) + " = " + std::to_string(level));

	if (before != after) {
		report("presets", std::string{"Preset "} + name + ": " + before + " -> " + after);
	}
}

static void configure_preset(const std::string &name, std::string levels) {
	if (!valid_preset_name(name)) {
		return;
	}

	auto it = current_config.presets.find(name);

	if (it == current_config.presets.cend()) {
		if (current_config.presets.size() == MAX_PRESETS) {
			return;
		}

		std::array<int,MAX_ADDR+1> empty_levels;

		empty_levels.fill(-1);
		it = current_config.presets.emplace(name, std::move(empty_levels)).first;
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
		report("presets", std::string{"Preset "} + name + ": " + before + " -> " + after);
	}
}

static void select_preset(const std::string &name,
		std::array<bool,MAX_ADDR+1> *filter = nullptr) {
	const auto it = current_config.presets.find(name);

	if (it == current_config.presets.cend() && name != BUILTIN_PRESET_OFF) {
		return;
	}

	report("lights", std::string{"Preset = "} + name);

	if (name == BUILTIN_PRESET_OFF) {
		for (int i = 0; i < MAX_ADDR; i++) {
			if (filter == nullptr || filter->at(i)) {
				levels[i] = 0;
				active_presets[i] = BUILTIN_PRESET_OFF;
				republish_active_presets = true;
			}
		}
	} else {
		for (int i = 0; i < MAX_ADDR; i++) {
			if (it->second[i] != -1) {
				if (filter == nullptr || filter->at(i)) {
					levels[i] = it->second[i];
					active_presets[i] = name;
					republish_active_presets = true;
				}
			}
		}
	}
}

static void delete_preset(const std::string &name) {
	const auto it = current_config.presets.find(name);

	if (it == current_config.presets.cend()) {
		return;
	}

	current_config.presets.erase(it);
	mqtt.publish((std::string{MQTT_TOPIC} + "/preset/" + name + "/active").c_str(), "", true);
	mqtt.publish((std::string{MQTT_TOPIC} + "/preset/" + name + "/levels").c_str(), "", true);
}

static void set_level(const std::string &lights, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	auto light_ids = parse_light_ids(lights);
	unsigned int changed = 0;

	for (int light_id : light_ids) {
		if (!current_config.lights[light_id]) {
			continue;
		}

		levels[light_id] = level;
		active_presets[light_id] = RESERVED_PRESET_CUSTOM;
		republish_active_presets = true;
		changed++;
	}

	if (!changed) {
		return;
	}

	report("lights", list_lights(light_ids) + " = " + std::to_string(level));
}

static void publish_active_presets() {
	bool force = !last_published_active_presets_us
			|| esp_timer_get_time() - last_published_active_presets_us >= ONE_M;

	if (!force && !republish_active_presets) {
		return;
	}

	std::unordered_set<std::string> active;
	std::unordered_set<std::string> all(MAX_PRESETS + 3);

	all.insert(BUILTIN_PRESET_OFF);
	all.insert(RESERVED_PRESET_CUSTOM);
	all.insert(RESERVED_PRESET_UNKNOWN);

	for (const auto &preset : current_config.presets) {
		all.insert(preset.first);
	}

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (current_config.lights[i]) {
			active.insert(active_presets[i]);
		}
	}

	for (const auto &preset : all) {
		bool is_active = active.find(preset) != active.end();
		bool last_active = last_active_presets.find(preset) != last_active_presets.end();

		if (force || (is_active != last_active)) {
			mqtt.publish((std::string{MQTT_TOPIC} + "/preset/" + preset + "/active").c_str(), is_active ? "1" : "0", true);
		}

		if (is_active) {
			last_active_presets.insert(preset);
		} else {
			last_active_presets.erase(preset);
		}
	}

	if (force) {
		last_published_active_presets_us = esp_timer_get_time();
	}
}

static void transmit_dali_one(unsigned int address, unsigned int level) {
	if (address > MAX_ADDR || level > MAX_LEVEL) {
		return;
	}

	// TODO
}

static void transmit_dali_all() {
	static uint64_t last_dali_us = 0;
	bool repeat = !last_dali_us || esp_timer_get_time() - last_dali_us >= ONE_S;

	if (repeat || levels != tx_levels) {
		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			if (current_config.lights[i]) {
				if (repeat || levels[i] != tx_levels[i]) {
					transmit_dali_one(i, levels[i]);
				}
			}
		}

		tx_levels = levels;
		if (repeat) {
			last_dali_us = esp_timer_get_time();
		}
	}
}

void setup() {
	pinMode(TX_GPIO, OUTPUT);
	digitalWrite(TX_GPIO, HIGH);
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		pinMode(SWITCH_GPIO[i], INPUT_PULLUP);
	}
	pinMode(LED_GPIO, OUTPUT);
	digitalWrite(LED_GPIO, LOW);

	device_id = String("mqtt-dali-controller_") + String(ESP.getEfuseMac(), HEX);
	active_presets.fill(RESERVED_PRESET_UNKNOWN);

	FS.begin(true);
	load_config();

	WiFi.persistent(false);
	WiFi.setAutoReconnect(false);
	WiFi.setSleep(false);
	WiFi.mode(WIFI_STA);

	mqtt.setServer(MQTT_HOSTNAME, MQTT_PORT);
	mqtt.setBufferSize(512);
	mqtt.setCallback([] (const char *topic, const uint8_t *payload, unsigned int length) {
		static const std::string preset_prefix = "/preset/";
		static const std::string set_prefix = "/set/";
		std::string topic_str = topic;

		if (topic_str == "meta/mqtt-agents/poll") {
			mqtt.publish("meta/mqtt-agents/reply", device_id.c_str());
			return;
		} else if (topic_str.rfind(MQTT_TOPIC, 0) != 0) {
			return;
		}

		topic_str = topic_str.substr(strlen(MQTT_TOPIC));

		if (topic_str == "/startup_complete") {
			if (!startup_complete) {
				ESP_LOGE("main", "Startup complete");
				startup_complete = true;
				save_config();
				publish_config();
			}
		} else if (topic_str == "/reboot") {
			esp_restart();
		} else if (topic_str == "/reload") {
			load_config();
			save_config();
			publish_config();
			republish_active_presets = true;
		} else if (topic_str == "/addresses") {
			configure_addresses(-1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/addresses") {
			configure_addresses(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/addresses") {
			configure_addresses(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/name") {
			auto name = std::string{(const char*)payload, length}.substr(0, MAX_SWITCH_NAME_LEN);

			if (current_config.switches[0].name != name) {
				report("switch", std::string{"Switch 0 name: "} + current_config.switches[0].name + " -> " + name);

				current_config.switches[0].name = name;
				save_config();
			}
		} else if (topic_str == "/switch/1/name") {
			auto name = std::string{(const char*)payload, length}.substr(0, MAX_SWITCH_NAME_LEN);

			if (current_config.switches[1].name != name) {
				report("switch", std::string{"Switch 1 name: "} + current_config.switches[1].name + " -> " + name);

				current_config.switches[1].name = name;
				save_config();
			}
		} else if (topic_str == "/switch/0/preset") {
			auto preset = std::string{(const char*)payload, length};

			if (valid_preset_name(preset) && current_config.switches[0].preset != preset) {
				report("switch", std::string{"Switch 0 preset: "} + current_config.switches[0].preset + " -> " + preset);

				current_config.switches[0].preset = preset;
				save_config();
			}
		} else if (topic_str == "/switch/1/preset") {
			auto preset = std::string{(const char*)payload, length};

			if (valid_preset_name(preset) && current_config.switches[1].preset != preset) {
				report("switch", std::string{"Switch 1 preset: "} + current_config.switches[1].preset + " -> " + preset);

				current_config.switches[1].preset = preset;
				save_config();
			}
		} else if (topic_str.rfind(preset_prefix, 0) == 0) {
			std::string preset_name = topic_str.substr(preset_prefix.length());
			auto idx = preset_name.find("/");

			if (idx == std::string::npos) {
				select_preset(preset_name);
			} else {
				std::string light_id = preset_name.substr(idx + 1);

				preset_name = preset_name.substr(0, idx);

				if (light_id == "delete") {
					delete_preset(preset_name);
				} else if (light_id == "levels") {
					configure_preset(preset_name, std::string{(const char *)payload, length});
				} else if (light_id == "all" || (light_id[0] >= '0' && light_id[0] <= '9')) {
					long value = -1;

					if (length) {
						std::string payload_copy = std::string{(const char *)payload, length};
						char *endptr = nullptr;

						value = std::strtol(payload_copy.c_str(), &endptr, 10);
						if (!endptr || endptr[0]) {
							return;
						}
					}

					configure_preset(preset_name, light_id, value);
				}
			}
		} else if (topic_str.rfind(set_prefix, 0) == 0) {
			std::string light_id = topic_str.substr(set_prefix.length());
			std::string payload_copy = std::string{(const char *)payload, length};
			char *endptr = nullptr;

			long value = std::strtol(payload_copy.c_str(), &endptr, 10);
			if (!length || !endptr || endptr[0]) {
				return;
			}

			set_level(light_id, value);
		}
	});
}

void loop() {
	for (unsigned int i = 0; i < NUM_SWITCHES; i++) {
		int switch_value = current_config.switches[i].preset.empty() ? LOW : digitalRead(SWITCH_GPIO[i]);

		if (switch_value != switch_state[i].value) {
			switch_state[i].value = switch_value;

			if (wifi_up && mqtt.connected()) {
				std::string name = current_config.switches[i].name;

				if (name.empty()) {
					name = "Light switch ";
					name += std::to_string(i);
				}

				mqtt.publish((std::string{MQTT_TOPIC}
					+ "/switch/" + std::to_string(i) + "/state").c_str(),
					switch_state[i].value == LOW ? "1" : "0",
					true);
				switch_state[i].report_us = esp_timer_get_time();

				report("switch", name + " "
					+ (switch_state[i].value == LOW ? "ON" : "OFF")
					+ " (levels reset to " + current_config.switches[i].preset + ")");
			}
			select_preset(current_config.switches[i].preset, &current_config.switches[i].lights);
		} else if (switch_state[i].report_us
				&& esp_timer_get_time() - switch_state[i].report_us >= ONE_M) {
			mqtt.publish((std::string{MQTT_TOPIC}
					+ "/switch/" + std::to_string(i) + "/state").c_str(),
					switch_state[i].value == LOW ? "1" : "0",
					true);
			switch_state[i].report_us = esp_timer_get_time();
		}
	}

	transmit_dali_all();

	if (startup_complete && wifi_up && mqtt.connected()) {
		publish_active_presets();
	}

	switch (WiFi.status()) {
	case WL_IDLE_STATUS:
	case WL_NO_SSID_AVAIL:
	case WL_CONNECT_FAILED:
	case WL_CONNECTION_LOST:
	case WL_DISCONNECTED:
		if (!last_wifi_us || wifi_up || esp_timer_get_time() - last_wifi_us > 30 * ONE_S) {
			ESP_LOGE("network", "WiFi reconnect");
			WiFi.disconnect();
			WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
			last_wifi_us = esp_timer_get_time();
			wifi_up = false;
		}
		break;

	case WL_CONNECTED:
		if (!wifi_up) {
			ESP_LOGE("network", "WiFi connected");
			wifi_up = true;
		}
		break;

	case WL_NO_SHIELD:
	case WL_SCAN_COMPLETED:
		break;
	}

	mqtt.loop();

	if (wifi_up) {
		if (!mqtt.connected() && (!last_mqtt_us || esp_timer_get_time() - last_mqtt_us > ONE_S)) {
			ESP_LOGE("network", "MQTT connecting");
			mqtt.connect(device_id.c_str());

			if (mqtt.connected()) {
				std::string topic = MQTT_TOPIC;

				ESP_LOGE("network", "MQTT connected");
				mqtt.subscribe((topic + "/reboot").c_str());
				mqtt.subscribe((topic + "/reload").c_str());
				mqtt.subscribe((topic + "/startup_complete").c_str());
				mqtt.subscribe("meta/mqtt-agents/poll");
				mqtt.subscribe((topic + "/addresses").c_str());
				mqtt.subscribe((topic + "/switch/0/addresses").c_str());
				mqtt.subscribe((topic + "/switch/1/addresses").c_str());
				mqtt.subscribe((topic + "/switch/0/name").c_str());
				mqtt.subscribe((topic + "/switch/1/name").c_str());
				mqtt.subscribe((topic + "/switch/0/preset").c_str());
				mqtt.subscribe((topic + "/switch/1/preset").c_str());
				mqtt.subscribe((topic + "/preset/+").c_str());
				mqtt.subscribe((topic + "/preset/+/+").c_str());
				mqtt.subscribe((topic + "/set/+").c_str());
				mqtt.publish("meta/mqtt-agents/announce", device_id.c_str());
				mqtt.publish((topic + "/startup_complete").c_str(), "");
			} else {
				ESP_LOGE("network", "MQTT connection failed");
			}
		}
	}
}
