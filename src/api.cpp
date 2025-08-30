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
	explicit TopicParser(const std::string &topic) {
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
	network_.subscribe(FixedConfig::mqttTopic("/groups/sync"));
	network_.subscribe(FixedConfig::mqttTopic("/switch/+/group"));
	network_.subscribe(FixedConfig::mqttTopic("/switch/+/name"));
	network_.subscribe(FixedConfig::mqttTopic("/switch/+/preset"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/groups"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/encoder_steps"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/level_steps"));
	network_.subscribe(FixedConfig::mqttTopic("/dimmer/+/mode"));
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

void API::receive(std::string &&topic, std::string &&payload) {
	if (topic == "meta/mqtt-agents/poll") {
		network_.publish("meta/mqtt-agents/reply", network_.device_id());
		topic.clear();
	} else if (topic.rfind(topic_prefix_, 0) == 0) {
		topic = topic.substr(topic_prefix_.size());
	} else {
		topic.clear();
	}

	TopicParser topic_parser{topic};

	topic_parser.get_string(topic);

	if (topic.empty()) {
		/* Do nothing */
	} else if (topic == "preset") {
		std::string preset_name;

		if (topic_parser.get_string(preset_name)) {
			std::string light_ids;

			if (topic_parser.get_string(light_ids)) {
				if (light_ids == RESERVED_GROUP_DELETE) {
					config_.delete_preset(preset_name);
				} else if (light_ids == RESERVED_GROUP_LEVELS) {
					if (!payload.empty()) {
						config_.set_preset(preset_name, payload);
					}
				} else {
					long value = Config::LEVEL_NO_CHANGE;

					if (payload.empty()
							|| long_from_string(payload, value)) {
						config_.set_preset(preset_name, light_ids, value);
					}
				}
			} else {
				if (preset_name == RESERVED_PRESET_ORDER) {
					config_.set_ordered_presets(payload);
				} else {
					if (payload.empty()) {
						payload = BUILTIN_GROUP_ALL;
					}

					lights_.select_preset(preset_name, payload);
				}
			}
		}
	} else if (topic == "set") {
		std::string light_ids;
		long value;

		if (topic_parser.get_string(light_ids)
				&& long_from_string(payload, value)) {
			lights_.set_level(light_ids, value);
		}
	} else if (topic == "startup_complete") {
		if (!startup_complete_) {
			ESP_LOGE(TAG, "Startup complete");
			startup_complete(true);
			config_.save_config();
			config_.publish_config();
		}
	} else if (topic == "reboot") {
		config_.save_config();

		std::lock_guard lock{file_mutex_};

		esp_restart();
	} else if (topic == "reload") {
		config_.load_config();
		config_.save_config();
		config_.publish_config();
		lights_.address_config_changed();
		dali_.wake_up();
	} else if (topic == "status") {
		ui_.status_report();
	} else if (topic == "ota") {
		if (topic_parser.get_string(topic)) {
			if (topic == "update") {
				ui_.ota_update();
			} else if (topic == "good") {
				ui_.ota_good();
			} else if (topic == "bad") {
				ui_.ota_bad();
			}
		}
	} else if (topic == "addresses") {
		config_.set_addresses(payload);
		lights_.address_config_changed(BUILTIN_GROUP_ALL);
		dali_.wake_up();
	} else if (topic == "switch") {
		long switch_id;

		if (topic_parser.get_long(switch_id)
				&& topic_parser.get_string(topic)) {
			if (topic == "group") {
				config_.set_switch_group(switch_id, payload);
			} else if (topic == "name") {
				config_.set_switch_name(switch_id, payload);
			} else if (topic == "preset") {
				config_.set_switch_preset(switch_id, payload);
			}
		}
	} else if (topic == "dimmer") {
		long dimmer_id;

		if (topic_parser.get_long(dimmer_id)
				&& topic_parser.get_string(topic)) {
			if (topic == "groups") {
				config_.set_dimmer_groups(dimmer_id, payload);
			} else if (topic == "encoder_steps") {
				long value;

				if (long_from_string(payload, value)) {
					config_.set_dimmer_encoder_steps(dimmer_id, value);
				}
			} else if (topic == "level_steps") {
				long value;

				if (long_from_string(payload, value)) {
					config_.set_dimmer_level_steps(dimmer_id, value);
				}
			} else if (topic == "mode") {
				config_.set_dimmer_mode(dimmer_id, payload);
			} else if (topic == "get_debug") {
				dimmers_.publish_debug(dimmer_id);
			}
		}
	} else if (topic == "group") {
		std::string group_name;

		if (topic_parser.get_string(group_name)) {
			if (group_name == RESERVED_GROUP_SYNC) {
				lights_.request_group_sync();
			} else if (payload.empty()) {
				config_.delete_group(group_name);
			} else if (payload == "sync") {
				lights_.request_group_sync(group_name);
			} else {
				if (config_.set_group_addresses(group_name, payload)) {
					lights_.address_config_changed(group_name);
					lights_.request_group_sync(group_name);
				}
			}
		}
	} else if (topic == "command") {
		if (topic_parser.get_string(topic)) {
			if (topic == "store") {
				if (topic_parser.get_string(topic)) {
					if (topic == "power_on_level") {
						lights_.request_broadcast_power_on_level();
					} else if (topic == "system_failure_level") {
						lights_.request_broadcast_system_failure_level();
					}
				}
			}
		}
	}

	yield();
	network_.send_queued_messages();
}
