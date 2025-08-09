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
#include <array>

#include "debounce.h"
#include "thread.h"
#include "util.h"

static constexpr unsigned int NUM_SWITCHES = 5;

class Config;
class Lights;
class Network;

struct SwitchState {
public:
	SwitchState() = default;

	bool active{true};
	uint64_t report_us{0};
};

class Switches: public WakeupThread {
public:
	Switches(Network &network, const Config &config, Lights &lights);

	static std::string rtc_boot_memory();

	void setup();

	BootRTCStatus rtc_boot_status() const;

private:
	static constexpr const char *TAG = "Switches";
	static constexpr unsigned long DEBOUNCE_US = 20 * 1000;
	static constexpr unsigned long WATCHDOG_INTERVAL_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 4;
	static constexpr uint32_t RTC_MAGIC = 0xa75be95b;

	~Switches() = delete;

	static uint32_t rtc_crc(const std::array<uint32_t,NUM_SWITCHES> &states);

	unsigned long run_tasks() override;
	unsigned long run_switch(unsigned int switch_id);
	void publish_switch(unsigned int switch_id, const std::string &group);

	void load_rtc_state();
	void save_rtc_state();

	static uint32_t rtc_states_[NUM_SWITCHES];
	static uint32_t rtc_crc_;

	Network &network_;
	const Config &config_;
	Lights &lights_;
	BootRTCStatus boot_rtc_{BootRTCStatus::UNKNOWN};

	std::array<Debounce,NUM_SWITCHES> debounce_;
	std::array<SwitchState,NUM_SWITCHES> state_;
	bool using_rtc_state_{false};
};
