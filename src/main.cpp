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

#include <array>
#include <cstdlib>
#include <string>
#include <unordered_set>

#include "dali.h"
#include "config.h"
#include "lights.h"
#include "network.h"
#include "switches.h"

#if __has_include("fixed_config.h")
# include "fixed_config.h"
#else
# include "fixed_config.h.example"
#endif

static constexpr uint64_t ONE_S = 1000 * 1000ULL;
static constexpr uint64_t ONE_M = 60 * ONE_S;
static constexpr unsigned int LED_GPIO = 38;

struct SwitchState {
	SwitchState() : value(LOW), report_us(0) {}

	int value;
	uint64_t report_us;
};

static bool startup_complete = false;
static std::array<SwitchState,NUM_SWITCHES> switch_state;

static std::array<uint8_t,MAX_ADDR+1> levels{};
static std::array<uint8_t,MAX_ADDR+1> tx_levels{};
static std::array<std::string,MAX_ADDR+1> active_presets{};
static std::unordered_set<std::string> last_active_presets{};
static bool republish_active_presets = true;
static uint64_t last_published_active_presets_us = 0;

static Network network;
static Config config{network};

namespace cbor = qindesign::cbor;

static void select_preset(const std::string &name,
		std::bitset<MAX_ADDR+1> *filter = nullptr) {
	const auto lights = config.get_addresses();
	std::array<int,MAX_ADDR+1> preset_levels;

	if (!config.get_preset(name, preset_levels)) {
		return;
	}

	if (!filter) {
		network.report("lights", std::string{"Preset = "} + name);
	}

	for (int i = 0; i < MAX_ADDR; i++) {
		if (lights[i]) {
			if (preset_levels[i] != -1) {
				if (filter == nullptr || filter->test(i)) {
					levels[i] = preset_levels[i];
					active_presets[i] = name;
					republish_active_presets = true;
				}
			}
		} else {
			levels[i] = 0;
			if (!active_presets[i].empty()) {
				active_presets[i] = "";
			}
		}
	}
}

static void set_level(const std::string &lights, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	const auto addresses = config.get_addresses();
	const auto light_ids = Config::parse_light_ids(lights);
	unsigned int changed = 0;

	for (int light_id : light_ids) {
		if (!addresses[light_id]) {
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

	network.report("lights", config.lights_text(light_ids) + " = " + std::to_string(level));
}

static void publish_active_presets() {
	bool force = !last_published_active_presets_us
			|| esp_timer_get_time() - last_published_active_presets_us >= ONE_M;

	if (!force && !republish_active_presets) {
		return;
	}

	const auto lights = config.get_addresses();
	const std::unordered_set<std::string> all = config.preset_names();
	std::unordered_set<std::string> active;

	for (unsigned int i = 0; i <= MAX_ADDR; i++) {
		if (lights[i]) {
			active.insert(active_presets[i]);
		}
	}

	for (const auto &preset : all) {
		bool is_active = active.find(preset) != active.end();
		bool last_active = last_active_presets.find(preset) != last_active_presets.end();

		if (force || (is_active != last_active)) {
			network.publish(std::string{MQTT_TOPIC} + "/preset/" + preset + "/active", is_active ? "1" : "0", true);
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
		const auto lights = config.get_addresses();

		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			if (lights[i]) {
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

	active_presets.fill(RESERVED_PRESET_UNKNOWN);

	config.setup();

	network.setup([] (const char *topic, const uint8_t *payload, unsigned int length) {
		static const std::string preset_prefix = "/preset/";
		static const std::string set_prefix = "/set/";
		std::string topic_str = topic;

		if (topic_str == "meta/mqtt-agents/poll") {
			network.publish("meta/mqtt-agents/reply", network.device_id());
			return;
		} else if (topic_str.rfind(MQTT_TOPIC, 0) != 0) {
			return;
		}

		topic_str = topic_str.substr(strlen(MQTT_TOPIC));

		if (topic_str == "/startup_complete") {
			if (!startup_complete) {
				ESP_LOGE("main", "Startup complete");
				startup_complete = true;
				config.publish_config();
			}
		} else if (topic_str == "/reboot") {
			esp_restart();
		} else if (topic_str == "/reload") {
			config.load_config();
			config.publish_config();
			republish_active_presets = true;
		} else if (topic_str == "/addresses") {
			config.set_addresses(std::string{(const char*)payload, length});
			republish_active_presets = true;
		} else if (topic_str == "/switch/0/addresses") {
			config.set_switch_addresses(0, std::string{(const char*)payload, length});
			republish_active_presets = true;
		} else if (topic_str == "/switch/1/addresses") {
			config.set_switch_addresses(1, std::string{(const char*)payload, length});
			republish_active_presets = true;
		} else if (topic_str == "/switch/0/name") {
			config.set_switch_name(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/name") {
			config.set_switch_name(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/preset") {
			config.set_switch_preset(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/preset") {
			config.set_switch_preset(1, std::string{(const char*)payload, length});
		} else if (topic_str.rfind(preset_prefix, 0) == 0) {
			std::string preset_name = topic_str.substr(preset_prefix.length());
			auto idx = preset_name.find("/");

			if (idx == std::string::npos) {
				select_preset(preset_name);
			} else {
				std::string light_id = preset_name.substr(idx + 1);

				preset_name = preset_name.substr(0, idx);

				if (light_id == "delete") {
					config.delete_preset(preset_name);
				} else if (light_id == "levels") {
					config.set_preset(preset_name, std::string{(const char *)payload, length});
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

					config.set_preset(preset_name, light_id, value);
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
		auto preset = config.get_switch_preset(i);
		int switch_value = preset.empty() ? LOW : digitalRead(SWITCH_GPIO[i]);

		if (switch_value != switch_state[i].value) {
			switch_state[i].value = switch_value;

			if (network.connected()) {
				std::string name = config.get_switch_name(i);

				if (name.empty()) {
					name = "Light switch ";
					name += std::to_string(i);
				}

				network.publish(std::string{MQTT_TOPIC}
					+ "/switch/" + std::to_string(i) + "/state",
					switch_state[i].value == LOW ? "1" : "0",
					true);
				switch_state[i].report_us = esp_timer_get_time();

				network.report("switch", name + " "
					+ (switch_state[i].value == LOW ? "ON" : "OFF")
					+ " (levels reset to " + preset + ")");
			}

			auto lights = config.get_switch_addresses(i);

			select_preset(preset, &lights);
		} else if (switch_state[i].report_us
				&& esp_timer_get_time() - switch_state[i].report_us >= ONE_M) {
			network.publish(std::string{MQTT_TOPIC} + "/switch/" + std::to_string(i) + "/state",
					switch_state[i].value == LOW ? "1" : "0",
					true);
			switch_state[i].report_us = esp_timer_get_time();
		}
	}

	transmit_dali_all();

	if (startup_complete && network.connected()) {
		publish_active_presets();
	}

	network.loop([] () {
		std::string topic = MQTT_TOPIC;

		startup_complete = false;

		network.subscribe("meta/mqtt-agents/poll");
		network.subscribe(topic + "/reboot");
		network.subscribe(topic + "/reload");
		network.subscribe(topic + "/startup_complete");
		network.subscribe(topic + "/addresses");
		network.subscribe(topic + "/switch/0/addresses");
		network.subscribe(topic + "/switch/1/addresses");
		network.subscribe(topic + "/switch/0/name");
		network.subscribe(topic + "/switch/1/name");
		network.subscribe(topic + "/switch/0/preset");
		network.subscribe(topic + "/switch/1/preset");
		network.subscribe(topic + "/preset/+");
		network.subscribe(topic + "/preset/+/+");
		network.subscribe(topic + "/set/+");
		network.publish("meta/mqtt-agents/announce", network.device_id());
		network.publish(topic + "/startup_complete", "");
	});
}
