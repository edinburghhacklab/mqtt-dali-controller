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

#include "api.h"

#include <Arduino.h>
#include <esp_timer.h>

#include <mutex>
#include <string>

#include "config.h"
#include "dali.h"
#include "dimmers.h"
#include "lights.h"
#include "network.h"
#include "ui.h"
#include "util.h"

API::API(std::mutex &file_mutex, Network &network, Config &config, Dali &dali,
		Dimmers &dimmers, Lights &lights, UI &ui) : file_mutex_(file_mutex),
		network_(network), config_(config), dali_(dali), dimmers_(dimmers),
		lights_(lights), ui_(ui) {
}

void API::connected() {
	std::string topic{FixedConfig::mqttTopic()};

	startup_complete(false);

	network_.subscribe(topic + "/startup_complete");
	network_.subscribe("meta/mqtt-agents/poll");
	network_.subscribe(topic + "/reboot");
	network_.subscribe(topic + "/reload");
	network_.subscribe(topic + "/status");
	network_.subscribe(topic + "/idle/");
	network_.subscribe(topic + "/ota/+");
	network_.subscribe(topic + "/addresses");
	network_.subscribe(topic + "/group/+");
	network_.subscribe(topic + "/switch/+/group");
	network_.subscribe(topic + "/switch/+/name");
	network_.subscribe(topic + "/switch/+/preset");
	network_.subscribe(topic + "/dimmer/+/group");
	network_.subscribe(topic + "/dimmer/+/encoder_steps");
	network_.subscribe(topic + "/dimmer/+/level_steps");
	network_.subscribe(topic + "/dimmer/+/get_debug");
	network_.subscribe(topic + "/preset/+");
	network_.subscribe(topic + "/preset/+/+");
	network_.subscribe(topic + "/set/+");
	network_.subscribe(topic + "/command/store/power_on_level");
	network_.subscribe(topic + "/command/store/system_failure_level");
	network_.publish("meta/mqtt-agents/announce", network_.device_id());
	network_.publish(topic + "/startup_complete", "");
}

bool API::startup_complete() {
	return startup_complete_;
}

void API::startup_complete(bool state) {
	startup_complete_ = state;
	lights_.startup_complete(state);
	ui_.startup_complete(state);
}

void API::receive(const char *topic, const uint8_t *payload, unsigned int length) {
	static const std::string group_prefix = "/group/";
	static const std::string preset_prefix = "/preset/";
	static const std::string set_prefix = "/set/";
	std::string topic_str = topic;

	if (topic_str == "meta/mqtt-agents/poll") {
		network_.publish("meta/mqtt-agents/reply", network_.device_id());
		topic_str.clear();
	} else if (topic_str.rfind(FixedConfig::mqttTopic(), 0) == 0) {
		topic_str = topic_str.substr(FixedConfig::mqttTopic().size());
	} else {
		topic_str.clear();
	}

	if (topic_str == "") {
		/* Do nothing */
	} else if (topic_str == "/startup_complete") {
		if (!startup_complete_) {
			ESP_LOGE(TAG, "Startup complete");
			startup_complete(true);
			config_.save_config();
			config_.publish_config();
		}
	} else if (topic_str == "/reboot") {
		config_.save_config();

		std::lock_guard lock{file_mutex_};

		esp_restart();
	} else if (topic_str == "/reload") {
		config_.load_config();
		config_.save_config();
		config_.publish_config();
		lights_.address_config_changed();
		dali_.wake_up();
	} else if (topic_str == "/status") {
		ui_.status_report();
	} else if (topic_str == "/ota/update") {
		ui_.ota_update();
	} else if (topic_str == "/ota/good") {
		ui_.ota_good();
	} else if (topic_str == "/ota/bad") {
		ui_.ota_bad();
	} else if (topic_str == "/addresses") {
		config_.set_addresses(std::string{(const char*)payload, length});
		lights_.address_config_changed(BUILTIN_GROUP_ALL);
		dali_.wake_up();
	} else if (topic_str.rfind("/switch/", 0) == 0) {
		if (topic_str == "/switch/0/group") {
			config_.set_switch_group(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/group") {
			config_.set_switch_group(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/2/group") {
			config_.set_switch_group(2, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/3/group") {
			config_.set_switch_group(3, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/4/group") {
			config_.set_switch_group(4, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/name") {
			config_.set_switch_name(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/name") {
			config_.set_switch_name(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/2/name") {
			config_.set_switch_name(2, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/3/name") {
			config_.set_switch_name(3, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/4/name") {
			config_.set_switch_name(4, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/0/preset") {
			config_.set_switch_preset(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/1/preset") {
			config_.set_switch_preset(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/2/preset") {
			config_.set_switch_preset(2, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/3/preset") {
			config_.set_switch_preset(3, std::string{(const char*)payload, length});
		} else if (topic_str == "/switch/4/preset") {
			config_.set_switch_preset(4, std::string{(const char*)payload, length});
		}
	} else if (topic_str.rfind("/dimmer/", 0) == 0) {
		long value;

		if (!long_from_string(std::string{(const char *)payload, length}, value)) {
			value = 0;
		}

		if (topic_str == "/dimmer/0/group") {
			config_.set_dimmer_group(0, std::string{(const char*)payload, length});
		} else if (topic_str == "/dimmer/1/group") {
			config_.set_dimmer_group(1, std::string{(const char*)payload, length});
		} else if (topic_str == "/dimmer/2/group") {
			config_.set_dimmer_group(2, std::string{(const char*)payload, length});
		} else if (topic_str == "/dimmer/3/group") {
			config_.set_dimmer_group(3, std::string{(const char*)payload, length});
		} else if (topic_str == "/dimmer/4/group") {
			config_.set_dimmer_group(4, std::string{(const char*)payload, length});
		} else if (topic_str == "/dimmer/0/encoder_steps") {
			config_.set_dimmer_encoder_steps(0, value);
		} else if (topic_str == "/dimmer/1/encoder_steps") {
			config_.set_dimmer_encoder_steps(1, value);
		} else if (topic_str == "/dimmer/2/encoder_steps") {
			config_.set_dimmer_encoder_steps(2, value);
		} else if (topic_str == "/dimmer/3/encoder_steps") {
			config_.set_dimmer_encoder_steps(3, value);
		} else if (topic_str == "/dimmer/4/encoder_steps") {
			config_.set_dimmer_encoder_steps(4, value);
		} else if (topic_str == "/dimmer/0/level_steps") {
			config_.set_dimmer_level_steps(0, value);
		} else if (topic_str == "/dimmer/1/level_steps") {
			config_.set_dimmer_level_steps(1, value);
		} else if (topic_str == "/dimmer/2/level_steps") {
			config_.set_dimmer_level_steps(2, value);
		} else if (topic_str == "/dimmer/3/level_steps") {
			config_.set_dimmer_level_steps(3, value);
		} else if (topic_str == "/dimmer/4/level_steps") {
			config_.set_dimmer_level_steps(4, value);
		} else if (topic_str == "/dimmer/0/get_debug") {
			dimmers_.publish_debug(0);
		} else if (topic_str == "/dimmer/1/get_debug") {
			dimmers_.publish_debug(1);
		} else if (topic_str == "/dimmer/2/get_debug") {
			dimmers_.publish_debug(2);
		} else if (topic_str == "/dimmer/3/get_debug") {
			dimmers_.publish_debug(3);
		} else if (topic_str == "/dimmer/4/get_debug") {
			dimmers_.publish_debug(4);
		}
	} else if (topic_str.rfind(group_prefix, 0) == 0) {
		/* "/group/+" */
		std::string group_name = topic_str.substr(group_prefix.length());

		if (length) {
			config_.set_group_addresses(group_name, std::string{(const char *)payload, length});
			lights_.address_config_changed(group_name);
		} else {
			config_.delete_group(group_name);
		}
	} else if (topic_str.rfind(preset_prefix, 0) == 0) {
		std::string preset_name = topic_str.substr(preset_prefix.length());
		auto idx = preset_name.find("/");

		if (idx == std::string::npos) {
			/* "/preset/+" */
			std::string payload_copy = std::string{(const char *)payload, length};

			if (preset_name == RESERVED_PRESET_ORDER) {
				config_.set_ordered_presets(payload_copy);
			} else {
				if (payload_copy.empty()) {
					payload_copy = BUILTIN_GROUP_ALL;
				}

				lights_.select_preset(preset_name, payload_copy);
			}
		} else {
			/* "/preset/+/+" */
			std::string light_id = preset_name.substr(idx + 1);

			preset_name = preset_name.substr(0, idx);

			if (light_id == RESERVED_GROUP_DELETE) {
				config_.delete_preset(preset_name);
			} else if (light_id == RESERVED_GROUP_LEVELS) {
				config_.set_preset(preset_name, std::string{(const char *)payload, length});
			} else {
				long value = -1;

				if (length) {
					if (!long_from_string(std::string{(const char *)payload, length}, value)) {
						return;
					}
				}

				config_.set_preset(preset_name, light_id, value);
			}
		}
	} else if (topic_str.rfind(set_prefix, 0) == 0) {
		/* "/set/+" */
		std::string light_id = topic_str.substr(set_prefix.length());
		long value;

		if (!long_from_string(std::string{(const char *)payload, length}, value)) {
			return;
		}

		lights_.set_level(light_id, value);
	} else if (topic_str == "/command/store/power_on_level") {
		lights_.request_broadcast_power_on_level();
	} else if (topic_str == "/command/store/system_failure_level") {
		lights_.request_broadcast_system_failure_level();
	}

	yield();
	network_.send_queued_messages();
}
