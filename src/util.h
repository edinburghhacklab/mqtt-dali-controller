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

#include <cstring>
#include <memory>
#include <string>
#include <vector>

static constexpr uint64_t ONE_S = 1000 * 1000ULL;
static constexpr uint64_t ONE_M = 60 * ONE_S;
static constexpr uint64_t FIVE_M = 5 * ONE_M;

bool long_from_string(const std::string &text, long &value);
bool ulong_from_string(const std::string &text, unsigned long &value);
bool ulonglong_from_string(const std::string &text, unsigned long long &value);
std::string vector_text(const std::vector<std::string> &vector);

template<typename T, size_t size>
static inline std::string null_terminated_string(T(&data)[size]) {
	T *found = reinterpret_cast<T*>(std::memchr(&data[0], '\0', size));
	return std::string{&data[0], found ? (found - &data[0]) : size};
};

class MemoryDeleter {
public:
	void operator()(uint8_t *data) { ::free(data); }
};

using MemoryAllocation = std::unique_ptr<uint8_t, MemoryDeleter>;

enum BootRTCStatus {
	UNKNOWN,
	POWER_ON_IGNORED,
	CHECKSUM_MISMATCH,
	LOADED_OK,
};

class FixedConfig {
public:
	FixedConfig() = default;

	static inline const char *wifiHostname() { return WIFI_HOSTNAME; }
	static inline const char *wifiSSID() { return WIFI_SSID; }
	static inline const char *wifiPassword() { return WIFI_PASSWORD; }

	static inline const char *mqttHostname() { return MQTT_HOSTNAME; }
	static inline int mqttPort() { return MQTT_PORT; }
	static inline const std::string_view mqttTopic() { return mqtt_topic_str; }
	static std::string mqttTopic(const char *append) { return mqtt_topic_str + append; }
	static inline const std::string& mqttRemoteTopic() { return mqtt_remote_topic_str; }

	static inline bool isLocal() { return MQTT_REMOTE_TOPIC == nullptr; }
	static inline bool isRemote() { return MQTT_REMOTE_TOPIC != nullptr; }

	static inline bool hasChannel() { return IRC_CHANNEL[0]; }
	static inline const char *ircChannel() { return IRC_CHANNEL; }

	static inline const char *otaURL() { return OTA_URL; }

private:
	static std::string mqtt_topic_str;
	static std::string mqtt_remote_topic_str;

#if __has_include("fixed_config.h")
# include "fixed_config.h"
#else
# include "fixed_config.h.example"
#endif
};
