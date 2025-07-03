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
#include <PubSubClient.h>
#include <WiFi.h>

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

static bool lights[256]{};

#if __has_include("config.h")
# include "config.h"
#else
# include "config.h.example"
#endif

static constexpr uint64_t ONE_S = 1000 * 1000ULL;
static constexpr unsigned int SWITCH_GPIO = 17;
static constexpr unsigned int LED_GPIO = 15;
static constexpr unsigned int RX_GPIO = 16;
static constexpr unsigned int TX_GPIO = 18;
static bool startup_complete = false;
static int last_switch_state = LOW;

static uint64_t last_wifi_us = 0;
static bool wifi_up = false;

static uint64_t last_mqtt_us = 0;

static WiFiClient client;
static PubSubClient mqtt(client);
static String device_id;

static std::array<uint8_t,256> levels;
static std::array<uint8_t,256> tx_levels;
static std::deque<std::string> presets_order;
static std::unordered_map<std::string, std::array<int,256>> presets;

static void configure_preset(const std::string &name, int light_id, int level) {
	static constexpr size_t MAX_PRESETS = 50;
	const auto it = presets.find(name);

	if (light_id < 0 || light_id > 255) {
		return;
	}

	if (level < 0 || level > 255) {
		return;
	}

	if (it == presets.cend()) {
		if (presets_order.size() == MAX_PRESETS) {
			presets.erase(presets_order[0]);
			presets_order.pop_front();
		}

		presets_order.push_back(name);

		std::array<int, 256> levels;

		levels[light_id] = level;
		presets.emplace(name, std::move(levels));
		return;
	}

	it->second[light_id] = level;
}

static void select_preset(const std::string &name) {
	const auto it = presets.find(name);

	if (it == presets.cend() && name != "off") {
		return;
	}

	if (wifi_up && mqtt.connected()) {
		mqtt.publish("irc/send", (std::string{"{\"to\": \""}
			+ IRC_CHANNEL + "\", \"message\": \""
			+ MQTT_TOPIC + ": Preset - "
			+ name + "\"}").c_str());
	}

	if (name == "off") {
		for (int i = 0; i < 256; i++) {
			levels[i] = 0;
		}
	} else {
		for (int i = 0; i < 256; i++) {
			levels[i] = it->second[i];
		}
	}
}

static void set_level(int light_id, int level) {
	if (light_id < 0 || light_id > 255) {
		return;
	}

	if (level < 0 || level > 255) {
		return;
	}

	if (!lights[light_id]) {
		return;
	}

	if (wifi_up && mqtt.connected()) {
		mqtt.publish("irc/send", (std::string{"{\"to\": \""}
			+ IRC_CHANNEL + "\", \"message\": \""
			+ MQTT_TOPIC + ": Light #"
			+ std::to_string(light_id) + " - "
			+ std::to_string(level) + "\"}").c_str());
	}

	levels[light_id] = level;
}

static void transmit_dali() {
	static uint64_t last_dali_us = 0;

	if (levels != tx_levels || esp_timer_get_time() - last_dali_us > ONE_S) {
		// TODO Serial1.write()

		tx_levels = levels;
		last_dali_us = esp_timer_get_time();
	}
}

void setup() {
	configure_lights();

	pinMode(SWITCH_GPIO, INPUT_PULLUP);
	pinMode(LED_GPIO, OUTPUT);
	digitalWrite(LED_GPIO, LOW);

	device_id = String("mqtt-dali-controller_") + String(ESP.getEfuseMac(), HEX);

	digitalWrite(LED_GPIO, LOW);
	Serial1.begin(1200, SERIAL_8N1, RX_GPIO, TX_GPIO);

	WiFi.persistent(false);
	WiFi.setAutoReconnect(false);
	WiFi.setSleep(false);
	WiFi.mode(WIFI_STA);

	mqtt.setServer(MQTT_HOSTNAME, MQTT_PORT);
	mqtt.setCallback([] (const char *topic, const uint8_t *payload, unsigned int length) {
		static const std::string startup_topic = std::string{MQTT_TOPIC} + "/startup_complete";
		static const std::string reboot_topic = std::string{MQTT_TOPIC} + "/reboot";
		static const std::string preset_prefix = std::string{MQTT_TOPIC} + "/preset/";
		static const std::string set_prefix = std::string{MQTT_TOPIC} + "/set/";
		std::string topic_str = topic;

		if (topic_str == "meta/mqtt-agents/poll") {
			mqtt.publish("meta/mqtt-agents/reply", device_id.c_str());
		} else if (topic_str == startup_topic) {
			startup_complete = true;
		} else if (topic_str == reboot_topic) {
			esp_restart();
		} else if (topic_str.rfind(preset_prefix, 0) == 0) {
			std::string preset_name = topic_str.substr(preset_prefix.length());
			auto idx = preset_name.find("/");

			if (idx == std::string::npos) {
				select_preset(preset_name);
			} else {
				std::string light_id = preset_name.substr(idx + 1);

				preset_name = preset_name.substr(0, idx);
				configure_preset(preset_name, atoi(light_id.c_str()),
					atoi(std::string{(const char *)payload, length}.c_str()));
			}
		} else if (topic_str.rfind(set_prefix, 0) == 0) {
			std::string light_id = topic_str.substr(set_prefix.length());

			set_level(atoi(light_id.c_str()),
				atoi(std::string{(const char *)payload, length}.c_str()));
		}
	});
}

void loop() {
	if (startup_complete) {
		int switch_state = digitalRead(SWITCH_GPIO);

		digitalWrite(LED_GPIO, switch_state == LOW ? HIGH : LOW);

		if (switch_state != last_switch_state) {
			last_switch_state = switch_state;

			if (wifi_up && mqtt.connected()) {
				mqtt.publish("irc/send", (std::string{"{\"to\": \""}
					+ IRC_CHANNEL + "\", \"message\": \""
					+ MQTT_TOPIC + ": Light switch "
					+ (switch_state == LOW ? "ON" : "OFF")
					+ " (levels reset to comfort)\"}").c_str());
				select_preset("comfort");
			}
		}

		transmit_dali();
	}

	switch (WiFi.status()) {
	case WL_IDLE_STATUS:
	case WL_NO_SSID_AVAIL:
	case WL_CONNECT_FAILED:
	case WL_CONNECTION_LOST:
	case WL_DISCONNECTED:
		if (!last_wifi_us || wifi_up || esp_timer_get_time() - last_wifi_us > 30 * ONE_S) {
			Serial.println("WiFi reconnect");
			WiFi.disconnect();
			WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
			last_wifi_us = esp_timer_get_time();
			wifi_up = false;
		}
		break;

	case WL_CONNECTED:
		if (!wifi_up) {
			Serial.println("WiFi connected");
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
			Serial.println("MQTT connecting");
			mqtt.connect(device_id.c_str());

			if (mqtt.connected()) {
				Serial.println("MQTT connected");
				mqtt.subscribe((std::string{MQTT_TOPIC} + "/reboot").c_str());
				mqtt.subscribe((std::string{MQTT_TOPIC} + "/startup_complete").c_str());
				mqtt.subscribe("meta/mqtt-agents/poll");
				mqtt.subscribe((std::string{MQTT_TOPIC} + "/preset/+").c_str());
				mqtt.subscribe((std::string{MQTT_TOPIC} + "/preset/+/+").c_str());
				mqtt.subscribe((std::string{MQTT_TOPIC} + "/set/+").c_str());
				mqtt.publish("meta/mqtt-agents/announce", device_id.c_str());
				mqtt.publish((std::string{MQTT_TOPIC} + "/startup_complete").c_str(), "");
			} else {
				Serial.println("MQTT connection failed");
			}
		}
	}
}
