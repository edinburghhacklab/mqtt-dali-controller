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

#include "network.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <functional>
#include <string>

#include "util.h"

static void json_append_escape(std::string &output, const std::string_view value) {
	for (size_t i = 0; i < value.length(); i++) {
		if (value[i] == '"' || value[i] == '\\') {
			output += '\\';
		}
		output += value[i];
	}
}

Network::Network() : device_id_(String("mqtt-dali-controller_") + String(ESP.getEfuseMac(), HEX)) {}

void Network::report(const char *tag, const std::string &message) {
	ESP_LOGE(tag, "%s", message.c_str());

	if (connected() && IRC_CHANNEL[0]) {
		std::string payload;

		payload.reserve(512);
		payload += "{\"to\": \"";
		json_append_escape(payload, IRC_CHANNEL);
		payload += "\", \"message\": \"";
		json_append_escape(payload, MQTT_TOPIC);
		json_append_escape(payload, ": ");
		json_append_escape(payload, message);
		payload += + "\"}";

		publish("irc/send", payload.c_str());
	}
}

void Network::subscribe(const std::string &topic) {
    mqtt_.subscribe(topic.c_str());
}

void Network::publish(const std::string &topic, const std::string &payload, bool retain) {
    mqtt_.publish(topic.c_str(), payload.c_str(), retain);
}

void Network::setup(std::function<void(char*, uint8_t*, unsigned int)> callback) {
	WiFi.persistent(false);
	WiFi.setAutoReconnect(false);
	WiFi.setSleep(false);
	WiFi.mode(WIFI_STA);

	mqtt_.setServer(MQTT_HOSTNAME, MQTT_PORT);
	mqtt_.setBufferSize(512);
	mqtt_.setCallback(callback);
}

void Network::loop(std::function<void()> connected) {
	switch (WiFi.status()) {
	case WL_IDLE_STATUS:
	case WL_NO_SSID_AVAIL:
	case WL_CONNECT_FAILED:
	case WL_CONNECTION_LOST:
	case WL_DISCONNECTED:
		if (!last_wifi_us_ || wifi_up_ || esp_timer_get_time() - last_wifi_us_ > 30 * ONE_S) {
			ESP_LOGE("network", "WiFi reconnect");
			WiFi.disconnect();
			WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
			last_wifi_us_ = esp_timer_get_time();
			wifi_up_ = false;
		}
		break;

	case WL_CONNECTED:
		if (!wifi_up_) {
			ESP_LOGE("network", "WiFi connected");
			wifi_up_ = true;
		}
		break;

	case WL_NO_SHIELD:
	case WL_SCAN_COMPLETED:
		break;
	}

	mqtt_.loop();

	if (wifi_up_) {
		if (!mqtt_.connected() && (!last_mqtt_us_ || esp_timer_get_time() - last_mqtt_us_ > ONE_S)) {
			ESP_LOGE("network", "MQTT connecting");
			mqtt_.connect(device_id_.c_str());

			if (mqtt_.connected()) {
				ESP_LOGE("network", "MQTT connected");
                connected();
			} else {
				ESP_LOGE("network", "MQTT connection failed");
			}
		}
	}
}
