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
	static constexpr auto BUS_ARDUINO_LOW = HIGH;
	static constexpr auto BUS_ARDUINO_HIGH = LOW;
	static constexpr unsigned int BUS_RMT_LOW = 1;
	static constexpr unsigned int BUS_RMT_HIGH = 0;
	static constexpr unsigned long BAUD_RATE = 1200;
	static constexpr unsigned long TICK_NS = 1000UL;
	static constexpr unsigned long HALF_SYMBOL_TICKS = 1000000000UL / TICK_NS / BAUD_RATE / 2;
	static_assert(HALF_SYMBOL_TICKS == 416 /* µs */);
	static constexpr unsigned int START_BITS = 1;
	static constexpr unsigned int STOP_BITS = 2;
	static constexpr unsigned int IDLE_SYMBOLS = 11;
	static constexpr unsigned long TX_POWER_LEVEL_TICKS = (START_BITS + 8 + 8 + STOP_BITS + IDLE_SYMBOLS) * HALF_SYMBOL_TICKS * 2;
	static constexpr unsigned long TX_POWER_LEVEL_NS = TX_POWER_LEVEL_TICKS * TICK_NS;
	static constexpr unsigned long TX_POWER_LEVEL_MS = (TX_POWER_LEVEL_NS + 999999UL) / 1000000UL;
	static_assert(TX_POWER_LEVEL_MS == 25);
	static constexpr unsigned long REFRESH_PERIOD_MS = 5000;
	static constexpr unsigned long WATCHDOG_INTERVAL_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 4;

	static const rmt_data_t DALI_0;
	static const rmt_data_t DALI_1;
	static const rmt_data_t DALI_STOP_IDLE;

	~Dali() = delete;

	unsigned long run_tasks() override;

	bool ready();
	void push_byte(std::vector<rmt_data_t> &symbols, uint8_t value);
	bool tx_idle();
	bool tx_power_level(uint8_t address, uint8_t level);

	const Config &config_;
	const Lights &lights_;
	rmt_obj_t *rmt_{nullptr};
	std::array<uint8_t,MAX_ADDR+1> tx_levels_{};
	unsigned int next_address_{0};
};
