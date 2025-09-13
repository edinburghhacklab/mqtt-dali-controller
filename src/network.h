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

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>
#include <string>

#include "util.h"

class Message {
public:
	Message() = default;
	Message(Message&&) = default;
	Message& operator=(Message&&) = default;

	static constexpr size_t BUFFER_SIZE = 512;

	const char* topic() const;
	std::pair<const uint8_t *,size_t> payload() const;
	bool retain() const;

	bool write(const std::string &topic, const std::string &payload, bool retain);

private:
	Message(const Message&) = delete;
	Message& operator=(const Message&) = delete;

	MemoryAllocation buffer_;
	size_t topic_len_{0};
	size_t payload_len_{0};
	bool retain_{false};
};

class Network {
public:
	Network();

	void setup(std::function<void()> connected,
		std::function<void(std::string &&topic, std::string &&payload)> receive);
	void loop();
	inline std::string device_id() { return device_id_.c_str(); }
	inline bool connected() { return wifi_up_ && mqtt_.connected(); }
	inline bool busy() {
		std::lock_guard lock{messages_mutex_};
		return !immediate_message_queue_.empty();
	}
	void report(const char *tag, const std::string &message);
	void subscribe(const std::string &topic);
	void publish(const std::string &topic, const std::string &payload,
		bool retain = false, bool immediate = false);
	void send_queued_messages();
	size_t received_message_count();
	size_t sent_message_count();
	size_t maximum_queue_size();

private:
	static constexpr const char *TAG = "UI";
	static constexpr size_t MAX_QUEUED_MESSAGES = 1000;
	static constexpr size_t SEND_QUEUE_DIVISOR = 10;

	void receive(char *topic, uint8_t *payload, unsigned int length);

	String device_id_;
	WiFiClient client_;
	PubSubClient mqtt_{client_};
	uint64_t last_wifi_us_{0};
	std::atomic<bool> wifi_up_{false};
	uint64_t last_mqtt_us_{0};

	std::function<void()> connected_;
	std::function<void(std::string &&topic, std::string &&payload)> receive_;

	std::mutex messages_mutex_;
	std::deque<Message> immediate_message_queue_;
	std::deque<Message> message_queue_;
	std::deque<Message> send_messages_;
	size_t dropped_messages_{0};
	size_t oversized_messages_{0};
	size_t received_messages_{0};
	size_t sent_messages_{0};
	size_t maximum_queue_size_{0};
};
