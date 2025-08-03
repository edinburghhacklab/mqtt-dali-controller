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

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <functional>
#include <string>

class Network {
public:
	Network();

	void setup(std::function<void(char*, uint8_t*, unsigned int)> mqtt_callback);
	void loop(std::function<void()> connected);
	inline std::string device_id() { return device_id_.c_str(); }
	inline bool connected() { return wifi_up_ && mqtt_.connected(); }
	void report(const char *tag, const std::string &message);
	void subscribe(const std::string &topic);
	void publish(const std::string &topic, const std::string &payload, bool retain = false);

private:
	String device_id_;
	WiFiClient client_;
	PubSubClient mqtt_{client_};
	uint64_t last_wifi_us_{0};
	bool wifi_up_{false};
	uint64_t last_mqtt_us_{0};
};
