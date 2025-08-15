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

#include <mutex>

class Dali;
class Lights;
class Network;
class Switches;

class UI {
public:
	UI(std::mutex &file_mutex, Network &network, Lights &lights);

	void setup();
	void set_dali(Dali &dali);
	void set_switches(Switches &switches);
	void loop();
	void startup_complete(bool state);
	void status_report();
	void ota_update();
	void ota_good();
	void ota_bad();

private:
	static constexpr const char *TAG = "UI";
	static constexpr unsigned int LED_GPIO = 38;

	UI(const UI&) = delete;
	UI& operator=(const UI&) = delete;

	void publish_application();
	void publish_boot();
	void publish_partitions();
	void publish_stats();
	void publish_tasks();

	void ota_result(bool good);

	Network &network_;
	Lights &lights_;
	Dali *dali_{nullptr};
	Switches *switches_{nullptr};
	std::mutex &file_mutex_;
	uint64_t last_publish_us_{0};
	bool startup_complete_{false};
};
