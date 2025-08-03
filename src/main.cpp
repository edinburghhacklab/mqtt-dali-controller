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
#include "ui.h"
#include "util.h"

static Network network;
static UI ui{network};
static Config config{network};
static Lights lights{network, config};
static Dali dali{config, lights};
static Switches switches{network, config, lights};
static bool startup_complete{false};

namespace cbor = qindesign::cbor;

static void set_startup_complete(bool state) {
	startup_complete = state;
	lights.startup_complete(state);
	ui.startup_complete(state);
}

extern "C" {

bool verifyRollbackLater() {
	return true;
}

bool testSPIRAM() {
	return true;
}

}


void setup() {
	dali.setup();
	switches.setup();
	config.setup();
	ui.setup();

	network.setup([] (const char *topic, const uint8_t *payload, unsigned int length) {
		static const std::string group_prefix = "/group/";
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
				set_startup_complete(true);
				config.publish_config();
			}
		} else if (topic_str == "/reboot") {
			esp_restart();
		} else if (topic_str == "/reload") {
			config.load_config();
			config.publish_config();
			lights.address_config_changed();
		} else if (topic_str == "/status") {
			ui.status_report();
		} else if (topic_str == "/addresses") {
			config.set_addresses(std::string{(const char*)payload, length});
			lights.address_config_changed(BUILTIN_GROUP_ALL);
		} else if (topic_str == "/switch/0/group") {
			config.set_switch_group(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/group") {
			config.set_switch_group(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/name") {
			config.set_switch_name(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/name") {
			config.set_switch_name(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/preset") {
			config.set_switch_preset(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/preset") {
			config.set_switch_preset(1, std::string{(const char*)payload, length});
		} else if (topic_str.rfind(group_prefix, 0) == 0) {
			std::string group_name = topic_str.substr(group_prefix.length());

			if (length) {
				config.set_group_addresses(group_name, std::string{(const char *)payload, length});
				lights.address_config_changed(group_name);
			} else {
				config.delete_group(group_name);
			}
		} else if (topic_str.rfind(preset_prefix, 0) == 0) {
			std::string preset_name = topic_str.substr(preset_prefix.length());
			auto idx = preset_name.find("/");

			if (idx == std::string::npos) {
				std::string payload_copy = std::string{(const char *)payload, length};

				if (payload_copy.empty()) {
					payload_copy = BUILTIN_GROUP_ALL;
				}

				lights.select_preset(preset_name, payload_copy);
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
	ui.loop();

	network.loop([] () {
		std::string topic = MQTT_TOPIC;

		set_startup_complete(false);

		network.subscribe("meta/mqtt-agents/poll");
		network.subscribe(topic + "/reboot");
		network.subscribe(topic + "/reload");
		network.subscribe(topic + "/status");
		network.subscribe(topic + "/startup_complete");
		network.subscribe(topic + "/addresses");
		network.subscribe(topic + "/group/+");
		network.subscribe(topic + "/switch/0/group");
		network.subscribe(topic + "/switch/1/group");
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
