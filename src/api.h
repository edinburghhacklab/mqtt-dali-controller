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

#include <mutex>
#include <string>

class Config;
class Dali;
class Dimmers;
class Lights;
class Network;
class UI;

class API {
public:
	API(std::mutex &file_mutex, Network &network, Config &config, Dali &dali,
		Dimmers &dimmers, Lights &lights, UI &ui);

	void connected();
	void receive(std::string &&topic, std::string &&payload);
	bool startup_complete();

private:
	static constexpr const char *TAG = "API";

	~API() = delete;

	void startup_complete(bool state);

	std::mutex &file_mutex_;
	Network &network_;
	Config &config_;
	Dali &dali_;
	Dimmers &dimmers_;
	Lights &lights_;
	UI &ui_;
	const std::string topic_prefix_;
	bool startup_complete_{false};
};
