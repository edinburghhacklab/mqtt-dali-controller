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

#include "network.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

#include "util.h"

static void json_append_escape(std::string &output, const std::string_view value) {
	for (size_t i = 0; i < value.length(); i++) {
		if (value[i] == '"' || value[i] == '\\') {
			output += '\\';
		}
		output += value[i];
	}
}

Network::Network()
		: device_id_(String("mqtt-dali-controller_") + String(ESP.getEfuseMac(), HEX)) {
}

void Network::report(const char *tag, const std::string &message) {
	ESP_LOGE(tag, "%s", message.c_str());

	if (IRC_CHANNEL[0]) {
		std::string payload;

		payload.reserve(Message::BUFFER_SIZE);
		payload += "{\"to\": \"";
		json_append_escape(payload, IRC_CHANNEL);
		payload += "\", \"message\": \"";
		json_append_escape(payload, MQTT_TOPIC);
		json_append_escape(payload, ": ");
		json_append_escape(payload, message);
		payload += + "\"}";

		publish("irc/send", payload);
	}
}

void Network::subscribe(const std::string &topic) {
	mqtt_.subscribe(topic.c_str());
}

void Network::publish(const std::string &topic, const std::string &payload, bool retain) {
	Message message;
	bool ok = message.write(topic, payload, retain);

	std::lock_guard lock{messages_mutex_};

	if (!ok) {
		oversized_messages_++;
		return;
	}

	while (message_queue_.size() >= MAX_QUEUED_MESSAGES) {
		message_queue_.pop_front();
	}

	message_queue_.emplace_back(std::move(message));
	maximum_queue_size_ = std::max(maximum_queue_size_, message_queue_.size());
}

void Network::send_queued_messages() {
	if (!wifi_up_ || !mqtt_.connected()) {
		return;
	}

	std::unique_lock lock{messages_mutex_};
	size_t count = message_queue_.size() / SEND_QUEUE_DIVISOR + 1;
	size_t dropped = dropped_messages_;
	size_t oversized = oversized_messages_;

	while (!message_queue_.empty() && send_messages_.size() < count) {
		send_messages_.emplace_back(std::move(message_queue_.front()));
		message_queue_.pop_front();
	}

	dropped_messages_ = 0;
	oversized_messages_ = 0;
	lock.unlock();

	if (dropped) {
		mqtt_.publish((std::string{MQTT_TOPIC} + "/stats/dropped_messages").c_str(),
			std::to_string(dropped).c_str());
	}

	if (oversized) {
		mqtt_.publish((std::string{MQTT_TOPIC} + "/stats/oversized_messages").c_str(),
			std::to_string(oversized).c_str());
	}

	while (!send_messages_.empty()) {
		const auto &message = send_messages_.front();
		auto payload = message.payload();

		mqtt_.publish(message.topic(), payload.first, payload.second, message.retain());
		send_messages_.pop_front();
	}
}

size_t Network::maximum_queue_size() {
	std::lock_guard lock{messages_mutex_};
	size_t size = maximum_queue_size_;

	maximum_queue_size_ = 0;
	return size;
}

void Network::setup(std::function<void(char*, uint8_t*, unsigned int)> callback) {
	WiFi.persistent(false);
	WiFi.setAutoReconnect(false);
	WiFi.setSleep(false);
	WiFi.mode(WIFI_STA);

	mqtt_.setServer(MQTT_HOSTNAME, MQTT_PORT);
	mqtt_.setBufferSize(Message::BUFFER_SIZE);
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
			ESP_LOGE(TAG, "WiFi reconnect");
			WiFi.disconnect();
			WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
			last_wifi_us_ = esp_timer_get_time();
			wifi_up_ = false;
		}
		break;

	case WL_CONNECTED:
		if (!wifi_up_) {
			ESP_LOGE(TAG, "WiFi connected");
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
			ESP_LOGE(TAG, "MQTT connecting");
			mqtt_.connect(device_id_.c_str());

			if (mqtt_.connected()) {
				ESP_LOGE(TAG, "MQTT connected");
				connected();
			} else {
				ESP_LOGE(TAG, "MQTT connection failed");
			}
		}
	}

	send_queued_messages();
}

inline const char* Message::topic() const {
	if (topic_len_) {
		return reinterpret_cast<char*>(buffer_.get());
	} else {
		return "";
	}
}

inline std::pair<const uint8_t *,size_t> Message::payload() const {
	if (payload_len_) {
		return {&buffer_.get()[topic_len_], payload_len_};
	} else {
		return {nullptr, 0};
	}
}

inline bool Message::retain() const {
	return retain_;
}

bool Message::write(const std::string &topic, const std::string &payload, bool retain) {
	if (topic.length() + 1 + payload.length() > BUFFER_SIZE) {
		topic_len_ = 0;
		payload_len_ = 0;
		retain_ = false;
		return false;
	}

	buffer_ = MemoryAllocation{reinterpret_cast<uint8_t*>(
		::heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))};
	topic_len_ = topic.length() + 1;
	payload_len_ = payload.length();
	retain_ = retain;
	std::memcpy(buffer_.get(), topic.c_str(), topic_len_);
	std::memcpy(&buffer_.get()[topic_len_], payload.c_str(), payload_len_);
	return true;
}
