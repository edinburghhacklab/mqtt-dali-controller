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
#include "util.h"

#if __has_include("fixed_config.h")
# include "fixed_config.h"
#else
# include "fixed_config.h.example"
#endif

static constexpr unsigned int LED_GPIO = 38;

static uint64_t last_uptime_us = 0;
static bool startup_complete = false;

static Network network;
static Config config{network};
static Lights lights{network, config};
static Dali dali{config, lights};
static Switches switches{network, config, lights};

namespace cbor = qindesign::cbor;

void setup() {
	dali.setup();
	switches.setup();

	pinMode(LED_GPIO, OUTPUT);
	digitalWrite(LED_GPIO, LOW);

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
				lights.startup_complete(true);
				config.publish_config();
			}
		} else if (topic_str == "/reboot") {
			esp_restart();
		} else if (topic_str == "/reload") {
			config.load_config();
			config.publish_config();
			lights.address_config_changed();
		} else if (topic_str == "/addresses") {
			config.set_addresses(std::string{(const char*)payload, length});
			lights.address_config_changed();
		} else if (topic_str == "/switch/0/addresses") {
			config.set_switch_addresses(0, std::string{(const char*)payload, length});
			lights.address_config_changed();
		} else if (topic_str == "/switch/1/addresses") {
			config.set_switch_addresses(1, std::string{(const char*)payload, length});
			lights.address_config_changed();
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
				lights.select_preset(preset_name);
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

			lights.set_level(light_id, value);
		}
	});
}

void loop() {
	switches.loop();
	dali.loop();
	lights.loop();

	if (startup_complete && network.connected()) {
		if (!last_uptime_us || esp_timer_get_time() - last_uptime_us >= ONE_M) {
			network.publish(std::string{MQTT_TOPIC} + "/uptime_us",
				std::to_string(esp_timer_get_time()));
			last_uptime_us = esp_timer_get_time();
		}
	}

	network.loop([] () {
		std::string topic = MQTT_TOPIC;

		startup_complete = false;
		lights.startup_complete(false);

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
