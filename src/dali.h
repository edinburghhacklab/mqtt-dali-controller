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

#include "thread.h"

static constexpr uint8_t MAX_ADDR = 63;
static constexpr uint8_t MAX_LEVEL = 254;
static constexpr uint8_t LEVEL_NO_CHANGE = 255;

class Config;
class Lights;

class Dali: public WakeupThread {
public:
	Dali(const Config &config, const Lights &lights);

	void setup();

private:
	static constexpr const char *TAG = "DALI";
	static constexpr auto BUS_LOW = HIGH;
	static constexpr auto BUS_HIGH = LOW;
	static constexpr unsigned long TX_POWER_LEVEL_MS = 25;
	static constexpr unsigned long REFRESH_PERIOD_MS = 5000;
	static constexpr unsigned long WATCHDOG_INTERVAL_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 4;

	~Dali() = delete;

	unsigned long run_tasks() override;

	void tx_power_level(uint8_t address, uint8_t level);

	const Config &config_;
	const Lights &lights_;
	std::array<uint8_t,MAX_ADDR+1> tx_levels_{};
	uint64_t last_tx_us_{0};
	unsigned int next_address_{0};
};
