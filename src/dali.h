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
#include <bitset>
#include <mutex>

#include "thread.h"

class Config;
class Lights;

class DaliStats {
public:
	uint64_t min_tx_us{UINT64_MAX}; /**< Minimum duration of a transmitted command (µs) */
	uint64_t max_tx_us{0}; /**< Maximum duration of a transmitted command (µs) */
	uint64_t tx_count{0}; /**< Number of transmitted commands */
	uint64_t max_burst_tx_count{0}; /**< Maximum number of consecutively transmitted commands  */
	uint64_t max_burst_us{0}; /**< Maximum runtime of consecutively transmitted commands (µs) */
};

class Dali: public WakeupThread {
public:
	using address_t = uint8_t;
	using group_t = uint8_t;
	using level_t = uint8_t;
	using level_fast_t = uint_fast8_t;

private:
	static constexpr address_t MAX_ADDR = 63;
	static constexpr group_t MAX_GROUP = 15;

public:
	static constexpr size_t num_addresses = MAX_ADDR + 1;
	static constexpr level_t MAX_LEVEL = 254;
	static constexpr level_t LEVEL_NO_CHANGE = 255;

	using addresses_t = std::bitset<num_addresses>;

	Dali(const Config &config, const Lights &lights);

	void setup();
	void start();
	DaliStats get_stats();

	using WakeupThread::wake_up;
	using WakeupThread::wake_up_isr;

private:
	static constexpr const char *TAG = "DALI";

	/**
	 * Texas Instruments, SLAA422A (2012-10)
	 * Digital Addressable Lighting Interface (DALI) Implementation Using MSP430 Value Line Microcontrollers
	 * Page 6
	 *
	 * The signal from the DALI bus is inverted by the optocoupler
	 */
	static constexpr auto BUS_ARDUINO_LOW = HIGH;
	static constexpr auto BUS_ARDUINO_HIGH = LOW;
	static constexpr unsigned int BUS_RMT_LOW = 1;
	static constexpr unsigned int BUS_RMT_HIGH = 0;

	/**
	 * Microchip Technology, AN1465 (2012)
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Pages 2
	 *
	 * The bus is high when idle
	 */
	static constexpr auto BUS_ARDUINO_IDLE = BUS_ARDUINO_HIGH;
	static constexpr unsigned int BUS_RMT_IDLE = BUS_RMT_HIGH;

	/**
	 * Microchip Technology, AN1465 (2012)
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Page 5
	 *
	 * The half-bit time is 416.67µs ±10% (rounded up to better ensure the
	 * minimum time between frames is met).
	 */
	static constexpr unsigned long BAUD_RATE = 1200;
	static constexpr unsigned long TICK_NS = 1000UL;
	static constexpr unsigned long HALF_SYMBOL_TICKS = (1000000000UL / TICK_NS / BAUD_RATE + 1) / 2;
	static_assert(HALF_SYMBOL_TICKS == 417 /* µs */);

	static constexpr unsigned int START_BITS = 1;
	static constexpr unsigned int STOP_BITS = 2;
	static constexpr unsigned int IDLE_SYMBOLS = 11;

	static constexpr unsigned long TX_POWER_LEVEL_TICKS = (START_BITS + 8 + 8 + STOP_BITS + IDLE_SYMBOLS) * HALF_SYMBOL_TICKS * 2;
	static constexpr unsigned long TX_POWER_LEVEL_NS = TX_POWER_LEVEL_TICKS * TICK_NS;
	static constexpr unsigned long TX_POWER_LEVEL_MS = TX_POWER_LEVEL_NS / 1000000UL;
	static_assert(TX_POWER_LEVEL_MS == 25);

	static constexpr unsigned long REFRESH_PERIOD_MS = 5000;
	static constexpr unsigned long WATCHDOG_INTERVAL_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 4;

	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Pages 3 to 6
	 */
	DRAM_ATTR static constexpr const rmt_data_t DALI_0{{{
		.duration0 = HALF_SYMBOL_TICKS, .level0 = BUS_RMT_HIGH,
		.duration1 = HALF_SYMBOL_TICKS, .level1 = BUS_RMT_LOW,
	}}};

	DRAM_ATTR static constexpr const rmt_data_t DALI_1{{{
		.duration0 = HALF_SYMBOL_TICKS, .level0 = BUS_RMT_LOW,
		.duration1 = HALF_SYMBOL_TICKS, .level1 = BUS_RMT_HIGH,
	}}};

	DRAM_ATTR static constexpr const rmt_data_t DALI_STOP_IDLE{{{
		/* Stop bits */
		.duration0 = HALF_SYMBOL_TICKS * STOP_BITS * 2, .level0 = BUS_RMT_IDLE,
		/* Minimum idle time */
		.duration1 = HALF_SYMBOL_TICKS * IDLE_SYMBOLS * 2, .level1 = BUS_RMT_IDLE,
	}}};

	static constexpr uint8_t BROADCAST_ADDRESS = 0x7F;
	static constexpr uint8_t DATA_POWER_LEVEL = 0x00;
	static constexpr uint8_t DATA_COMMAND = 0x01;

	/*
	 * https://en.wikipedia.org/wiki/Digital_Addressable_Lighting_Interface
	 *
	 * and
	 *
	 * Texas Instruments, SLAA422A (2012-10)
	 * Digital Addressable Lighting Interface (DALI) Implementation Using MSP430 Value Line Microcontrollers
	 * Page 9
	 */
	static constexpr uint8_t COMMAND_TARGET_LEVEL_OFF = 0x00;
	static constexpr uint8_t COMMAND_STEP_UP = 0x03;
	static constexpr uint8_t COMMAND_STEP_DOWN = 0x04;
	static constexpr uint8_t COMMAND_STEP_DOWN_AND_OFF = 0x07;
	static constexpr uint8_t COMMAND_ON_AND_STEP_UP = 0x08;
	static constexpr uint8_t COMMAND_RESET = 0x20;
	static constexpr uint8_t COMMAND_STORE_ACTUAL_LEVEL_IN_DTR = 0x21;
	static constexpr uint8_t COMMAND_SET_SYSTEM_FAILURE_LEVEL_FROM_DTR = 0x2C;
	static constexpr uint8_t COMMAND_SET_POWER_ON_LEVEL_FROM_DTR = 0x2D;
	static constexpr uint8_t COMMAND_ADD_TO_GROUP = 0x60;
	static constexpr uint8_t COMMAND_REMOVE_FROM_GROUP = 0x70;

	static size_t byte_to_symbols(rmt_data_t *symbols, uint8_t value);

	~Dali() = delete;

	unsigned long run_tasks() override;

	bool async_ready();
	bool tx_idle();
	bool tx_frame(uint8_t address, uint8_t data);
	bool tx_power_level(address_t address, level_t level);
	bool tx_broadcast_command(uint8_t command);
	bool tx_set_dtr_from_actual_level();
	bool tx_set_power_on_level_from_dtr();
	bool tx_set_system_failure_level_from_dtr();

	const Config &config_;
	const Lights &lights_;
	rmt_obj_t *rmt_{nullptr};
	std::array<level_fast_t,num_addresses> tx_levels_{};
	unsigned int next_address_{0};

	std::mutex stats_mutex_;
	DaliStats stats_;
};
