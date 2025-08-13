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
#include <iostream>
#include <sstream>
#include <string>

#include "config.h"
#include "dali.h"
#include "dimmers.h"
#include "lights.h"
#include "network.h"
#include "ui.h"
#include "util.h"

class TopicParser {
public:
	TopicParser(const std::string &topic) {
		std::istringstream input{topic};
		std::string item;

		while (std::getline(input, item, '/')) {
			topic_.push_back(std::move(item));
		}
	}

	bool get_string(std::string &value) {
		if (!topic_.empty()) {
			value = std::move(topic_.front());
			topic_.pop_front();
			return true;
		} else {
			value.clear();
			return false;
		}
	}

	bool get_long(long &value) {
		std::string text;

		if (get_string(text)) {
			return long_from_string(text, value);
		} else {
			return false;
		}
	}

private:
	std::deque<std::string> topic_;
};

API::API(std::mutex &file_mutex, Network &network, Config &config, Dali &dali,
		Dimmers &dimmers, Lights &lights, UI &ui) : file_mutex_(file_mutex),
		network_(network), config_(config), dali_(dali), dimmers_(dimmers),
		lights_(lights), ui_(ui), topic_prefix_(FixedConfig::mqttTopic("/")) {
}

void API::connected() {
	startup_complete(false);

	network_.subscribe(FixedConfig::mqttTopic("/startup_complete"));
	network_.subscribe("meta/mqtt-agents/poll");
	network_.subscribe(FixedConfig::mqttTopic("/reboot"));
	network_.subscribe(FixedConfig::mqttTopic("/reload"));
	network_.subscribe(FixedConfig::mqttTopic("/status"));
	network_.subscribe(FixedConfig::mqttTopic("/idle/"));
	network_.subscribe(FixedConfig::mqttTopic("/ota/+"));
	network_.subscribe(FixedConfig::mqttTopic("/addresses"));
	network_.subscribe(FixedConfig::mqttTopic("/group/+"));
	network_.subscribe(FixedConfig::mqttTopic("/switch/+/group"));
	network_.subscribe(FixedConfig::mqttTopic("/switch/+/name"));
	network_.subscribe(FixedConfig::mqttTopic("/switch/+/preset"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/group"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/encoder_steps"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/level_steps"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/get_debug"));
	network_.subscribe(FixedConfig::mqttTopic("/preset/+"));
	network_.subscribe(FixedConfig::mqttTopic("/preset/+/+"));
	network_.subscribe(FixedConfig::mqttTopic("/set/+"));
	network_.subscribe(FixedConfig::mqttTopic("/command/store/power_on_level"));
	network_.subscribe(FixedConfig::mqttTopic("/command/store/system_failure_level"));
	network_.publish("meta/mqtt-agents/announce", network_.device_id());
	network_.publish(FixedConfig::mqttTopic("/startup_complete"), "");
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
	std::string topic_str{topic};
	std::string payload_str{(const char*)payload, length};

	if (topic_str == "meta/mqtt-agents/poll") {
		network_.publish("meta/mqtt-agents/reply", network_.device_id());
		topic_str.clear();
	} else if (topic_str.rfind(topic_prefix_, 0) == 0) {
		topic_str = topic_str.substr(topic_prefix_.size());
	} else {
		topic_str.clear();
	}

	TopicParser topic_parser{topic_str};

	topic_parser.get_string(topic_str);

	if (topic_str.empty()) {
		/* Do nothing */
	} else if (topic_str == "preset") {
		std::string preset_name;

		if (topic_parser.get_string(preset_name)) {
			std::string light_ids;

			if (topic_parser.get_string(light_ids)) {
				if (light_ids == RESERVED_GROUP_DELETE) {
					config_.delete_preset(preset_name);
				} else if (light_ids == RESERVED_GROUP_LEVELS) {
					config_.set_preset(preset_name, payload_str);
				} else {
					long value = -1;

					if (payload_str.empty()
							|| long_from_string(payload_str, value)) {
						config_.set_preset(preset_name, light_ids, value);
					}
				}
			} else {
				if (preset_name == RESERVED_PRESET_ORDER) {
					config_.set_ordered_presets(payload_str);
				} else {
					if (payload_str.empty()) {
						payload_str = BUILTIN_GROUP_ALL;
					}

					lights_.select_preset(preset_name, payload_str);
				}
			}
		}
	} else if (topic_str == "set") {
		std::string light_ids;
		long value;

		if (topic_parser.get_string(light_ids)
				&& long_from_string(payload_str, value)) {
			lights_.set_level(light_ids, value);
		}
	} else if (topic_str == "startup_complete") {
		if (!startup_complete_) {
			ESP_LOGE(TAG, "Startup complete");
			startup_complete(true);
			config_.save_config();
			config_.publish_config();
		}
	} else if (topic_str == "reboot") {
		config_.save_config();

		std::lock_guard lock{file_mutex_};

		esp_restart();
	} else if (topic_str == "reload") {
		config_.load_config();
		config_.save_config();
		config_.publish_config();
		lights_.address_config_changed();
		dali_.wake_up();
	} else if (topic_str == "status") {
		ui_.status_report();
	} else if (topic_str == "ota") {
		if (topic_parser.get_string(topic_str)) {
			if (topic_str == "update") {
				ui_.ota_update();
			} else if (topic_str == "good") {
				ui_.ota_good();
			} else if (topic_str == "bad") {
				ui_.ota_bad();
			}
		}
	} else if (topic_str == "addresses") {
		config_.set_addresses(payload_str);
		lights_.address_config_changed(BUILTIN_GROUP_ALL);
		dali_.wake_up();
	} else if (topic_str == "switch") {
		long switch_id;

		if (topic_parser.get_long(switch_id)
				&& topic_parser.get_string(topic_str)) {
			if (topic_str == "group") {
				config_.set_switch_group(switch_id, payload_str);
			} else if (topic_str == "name") {
				config_.set_switch_name(switch_id, payload_str);
			} else if (topic_str == "preset") {
				config_.set_switch_preset(switch_id, payload_str);
			}
		}
	} else if (topic_str == "dimmer") {
		long dimmer_id;

		if (topic_parser.get_long(dimmer_id)
				&& topic_parser.get_string(topic_str)) {
			if (topic_str == "group") {
				config_.set_dimmer_group(dimmer_id, payload_str);
			} else if (topic_str == "encoder_steps") {
				long value;

				if (long_from_string(payload_str, value)) {
					config_.set_dimmer_encoder_steps(dimmer_id, value);
				}
			} else if (topic_str == "level_steps") {
				long value;

				if (long_from_string(payload_str, value)) {
					config_.set_dimmer_level_steps(dimmer_id, value);
				}
			} else if (topic_str == "get_debug") {
				dimmers_.publish_debug(dimmer_id);
			}
		}
	} else if (topic_str == "group") {
		std::string group_name;

		if (topic_parser.get_string(group_name)) {
			if (!payload_str.empty()) {
				config_.set_group_addresses(group_name, payload_str);
				lights_.address_config_changed(group_name);
			} else {
				config_.delete_group(group_name);
			}
		}
	} else if (topic_str == "command") {
		if (topic_parser.get_string(topic_str)) {
			if (topic_str == "store") {
				if (topic_parser.get_string(topic_str)) {
					if (topic_str == "power_on_level") {
						lights_.request_broadcast_power_on_level();
					} else if (topic_str == "system_failure_level") {
						lights_.request_broadcast_system_failure_level();
					}
				}
			}
		}
	}

	yield();
	network_.send_queued_messages();
}
