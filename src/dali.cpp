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

#include "dali.h"

#include <Arduino.h>
#include <esp32-hal.h>
#include <driver/rmt.h>
#include <hal/rmt_types.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <cassert>
#include <algorithm>
#include <array>
#include <mutex>

#include "config.h"
#include "lights.h"
#include "util.h"

/* github:espressif/arduino-esp32 cores/esp32/esp32-hal-rmt.h v2.0.17 */
#if 0
struct rmt_obj_s {
	bool allocated;
	EventGroupHandle_t events;
	uint32_t channel;
	uint32_t buffers;
	uint32_t data_size;
	uint32_t* data_ptr;
	rmt_rx_data_cb_t cb;
	void * arg;
	TaskHandle_t rxTaskHandle;
	bool rx_completed;
	bool tx_not_rx;
};
#endif

static constexpr unsigned int RX_GPIO = 40;
static constexpr unsigned int TX_GPIO = 21;

#define DALI_LOG ESP_LOGD

Dali::Dali(const Config &config, const Lights &lights)
		: WakeupThread("dali", true), config_(config),
		lights_(lights) {
	tx_levels_.fill(LEVEL_NO_CHANGE);
	tx_group_levels_.fill(LEVEL_NO_CHANGE);
}

void Dali::setup() {
	pinMode(RX_GPIO, INPUT);
	pinMode(TX_GPIO, OUTPUT);
	digitalWrite(TX_GPIO, BUS_ARDUINO_IDLE);

	/*
	 * github:espressif/arduino-esp32 cores/esp32/esp32-hal-rmt.h v2.0.17
	 *
	 * RMT_DEFAULT_ARD_CONFIG_TX(...) = {
	 *   .tx_config = {
	 *     .idle_level = RMT_IDLE_LEVEL_LOW,
	 *     .idle_output_en = true,
	 * }
	 *
	 * Idle state defaults to 0 (LOW), which is BUS_RMT_IDLE (HIGH)
	 */
	rmt_ = rmtInit(TX_GPIO, RMT_TX_MODE, RMT_MEM_256);
	static_assert((uint32_t)(1000/12.5f) == 80U);
	rmtSetTick(rmt_, TICK_NS);
	tx_idle();
}

void Dali::start() {
	std::thread t;
	make_thread(t, "dali", 8192, 1, 19, &Dali::run_loop, this);
	t.detach();
}

DaliStats Dali::get_stats() {
	std::lock_guard lock{stats_mutex_};
	DaliStats stats = stats_;

	stats_ = {};
	return stats;
}

unsigned long Dali::run_tasks() {
	const unsigned long num_lights = config_.get_addresses().count();
	const unsigned long refresh_delay_ms = num_lights == 0
		? ULONG_MAX : std::max(0UL, REFRESH_PERIOD_MS / num_lights - TX_POWER_LEVEL_MS);
	const unsigned long delay_ms = std::min(WATCHDOG_INTERVAL_MS, refresh_delay_ms);
	bool changed = false;
	bool refresh = true;

	esp_task_wdt_reset();

	uint64_t start = esp_timer_get_time();
	LightsState state = lights_.get_state();
	uint64_t count = 0;
	/*
	 * Set power level for lights that have changed level, cycling through the
	 * addresses each time to avoid preferring low-numbered lights.
	 */
	do {
		changed = false;

		for (unsigned int i = 0; i <= MAX_GROUP; i++) {
			unsigned int group = next_group_;

			if (state.group_levels[group] != tx_group_levels_[group]) {
				if (state.group_levels[group] == LEVEL_NO_CHANGE) {
					tx_group_levels_[group] = LEVEL_NO_CHANGE;
				} else if (tx_group_power_level(group, state.group_levels[group])) {
					tx_group_levels_[group] = state.group_levels[group];
					changed = true;
					refresh = false;
					count++;

					for (unsigned int address = 0;
							address < state.group_addresses[group].size();
							address++) {
						tx_levels_[address] = state.group_levels[group];
					}
				}

				state = lights_.get_state();
				esp_task_wdt_reset();
			}

			next_group_++;
			next_group_ %= MAX_GROUP + 1;
		}

		for (unsigned int i = 0; i <= MAX_ADDR; i++) {
			unsigned int address = next_address_;

			if (state.addresses[address]
					&& !state.group_level_addresses[address]
					&& (state.force_refresh[address]
						|| state.levels[address] != tx_levels_[address])) {
				if (state.levels[address] == LEVEL_NO_CHANGE) {
					tx_levels_[address] = LEVEL_NO_CHANGE;
				} else if (tx_address_power_level(address, state.levels[address])) {
					tx_levels_[address] = state.levels[address];
					changed = true;
					refresh = false;
					count++;

					if (state.force_refresh[address]) {
						lights_.completed_force_refresh(address);
					}
				}

				state = lights_.get_state();
				esp_task_wdt_reset();
			}

			next_address_++;
			next_address_ %= MAX_ADDR + 1;
		}
	} while (changed);

	if (count > 0) {
		uint64_t finish = esp_timer_get_time();
		std::lock_guard lock{stats_mutex_};

		stats_.max_burst_tx_count = std::max(stats_.max_burst_tx_count, count);
		stats_.max_burst_us = std::max(stats_.max_burst_us, finish - start);
	}

	for (unsigned int group = 0; group < state.group_sync.size(); group++) {
		if (!state.group_sync[group]) {
			continue;
		}

		if (!tx_group_empty(group)) {
			continue;
		}

		for (unsigned int address = 0;
				address < state.group_addresses[group].size(); address++) {
			if (state.group_addresses[group][address]) {
				if (!tx_address_group_add(address, group)) {
					goto group_sync_failed;
				}
			}
		}

		lights_.completed_group_sync(group);
group_sync_failed:;
	}

	if (state.broadcast_power_on_level || state.broadcast_system_failure_level) {
		if (tx_set_dtr_from_actual_level()) {
			if (state.broadcast_power_on_level) {
				if (tx_set_power_on_level_from_dtr()) {
					lights_.completed_broadcast_power_on_level();
				}
			}

			if (state.broadcast_system_failure_level) {
				if (tx_set_system_failure_level_from_dtr()) {
					lights_.completed_broadcast_system_failure_level();
				}
			}
		}
	}

	if (refresh) {
		/*
		* Refresh light power levels individually over a short time period,
		* cycling through the addresses each time to avoid preferring
		* low-numbered lights. Delays between lights keeps the bus idle most of
		* the time to improve responsiveness when dimming with a rotary encoder.
		*/
		for (unsigned int i = 0; i <= MAX_ADDR && !changed; i++) {
			unsigned address = next_address_;

			if (state.addresses[address]) {
				if (state.levels[address] == LEVEL_NO_CHANGE) {
					tx_levels_[address] = LEVEL_NO_CHANGE;
				} else if (tx_address_power_level(address, state.levels[address])) {
					tx_levels_[address] = state.levels[address];
					changed = true;
				}

				esp_task_wdt_reset();
			}

			next_address_++;
			next_address_ %= MAX_ADDR + 1;
		}
	}

	return delay_ms;
}

#if 0
bool Dali::async_ready() {
	return rmt_wait_tx_done(static_cast<rmt_channel_t>(rmt_->channel), 0) == ESP_OK;
}
#endif

inline size_t Dali::byte_to_symbols(rmt_data_t *symbols, uint8_t value) {
	for (int i = 7; i >= 0; i--) {
		*symbols = (((value >> i) & 1) ? DALI_1 : DALI_0);
		symbols++;
	}
	return 8;
}

bool Dali::tx_idle() {
	DALI_LOG(TAG, "Idle");

	std::array<rmt_data_t,1> symbols{DALI_STOP_IDLE};

	return rmtWriteBlocking(rmt_, symbols.data(), symbols.size());
}

bool Dali::tx_frame(uint8_t address, uint8_t data, bool repeat) {
	uint64_t start = esp_timer_get_time();

	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Pages 3 to 6
	 *
	 * 1 - Start bit (1 bit: 1)
	 *
	 * 8 - Address byte (8 bits)
	 * 8 - Data byte (8 bits)
	 *
	 * 1 - Stop bits (2 bits: idle)
	 *     Time between consecutive forward frames (11 bits: idle)
	 */
	std::array<rmt_data_t,2 * (1 + 8 + 8 + 1)> symbols;
	size_t i = 0;

	symbols[i++] = DALI_1;
	i += byte_to_symbols(&symbols[i], address);
	i += byte_to_symbols(&symbols[i], data);
	symbols[i++] = DALI_STOP_IDLE;

	if (repeat) {
		symbols[i++] = DALI_1;
		i += byte_to_symbols(&symbols[i], address);
		i += byte_to_symbols(&symbols[i], data);
		symbols[i++] = DALI_STOP_IDLE;

		assert(i == symbols.size());
	} else {
		assert(i == symbols.size() / 2);
	}

	bool ret = rmtWriteBlocking(rmt_, symbols.data(), i);
	uint64_t finish = esp_timer_get_time();
	std::lock_guard lock{stats_mutex_};

	stats_.min_tx_us = std::min(stats_.min_tx_us, finish - start);
	stats_.max_tx_us = std::max(stats_.max_tx_us, finish - start);
	stats_.tx_count += repeat ? 2 : 1;
	return ret;
}

bool Dali::tx_address_power_level(address_t address, level_t level) {
	if (address > MAX_ADDR) {
		return true;
	}

	DALI_LOG(TAG, "Power level A/%u = %u", address, level);

	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Page 5
	 *
	 * Address:
	 *   Short address (1 bit: 0)
	 *   Address (6 bits)
	 *   Selector: direct arc power level (1 bit: 0)
	 *
	 * Data:
	 *   Power level (8 bits)
	 */
	return tx_frame((address << 1) | DATA_POWER_LEVEL, level, false);
}

bool Dali::tx_group_power_level(group_t group, level_t level) {
	if (group > MAX_GROUP) {
		return true;
	}

	DALI_LOG(TAG, "Power level G/%u = %u", group, level);

	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Page 5
	 *
	 * Address:
	 *   Group address (3 bits: 100)
	 *   Group (4 bits)
	 *   Selector: direct arc power level (1 bit: 0)
	 *
	 * Data:
	 *   Power level (8 bits)
	 */
	return tx_frame((GROUP_ADDRESS << 1) | (group << 1) | DATA_POWER_LEVEL, level, false);
}

bool Dali::tx_address_group_add(address_t address, group_t group) {
	DALI_LOG(TAG, "Add to group %u (address %u)", group, address);
	return tx_address_command(address, COMMAND_ADD_TO_GROUP + group, true);
}

bool Dali::tx_group_empty(group_t group) {
	DALI_LOG(TAG, "Remove from group %u (group %u)", group, group);
	return tx_group_command(group, COMMAND_REMOVE_FROM_GROUP + group, true);
}

bool Dali::tx_address_command(address_t address, uint8_t command, bool repeat) {
	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Page 5
	 *
	 * Address:
	 *   Short address (1 bit: 0)
	 *   Address (6 bits)
	 *   Selector: command (1 bit: 1)
	 *
	 * Data:
	 *   Command (8 bits)
	 */
	return tx_frame((address << 1) | DATA_COMMAND, command, repeat);
}

bool Dali::tx_group_command(group_t group, uint8_t command, bool repeat) {
	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Page 5
	 *
	 * Address:
	 *   Group address (3 bits: 100)
	 *   Group (4 bits)
	 *   Selector: command (1 bit: 1)
	 *
	 * Data:
	 *   Command (8 bits)
	 */
	return tx_frame((GROUP_ADDRESS << 1) | (group << 1) | DATA_COMMAND, command, repeat);
}

bool Dali::tx_broadcast_command(uint8_t command, bool repeat) {
	/*
	 * Microchip Technology, AN1465
	 * Digitally Addressable Lighting Interface (DALI) Communication
	 * Page 5
	 *
	 * Address:
	 *   Broadcast address (7 bits: 1s)
	 *   Selector: command (1 bit: 1)
	 *
	 * Data:
	 *   Command (8 bits)
	 */
	return tx_frame((BROADCAST_ADDRESS << 1) | DATA_COMMAND, command, repeat);
}

bool Dali::tx_set_dtr_from_actual_level() {
	DALI_LOG(TAG, "Copy actual level to DTR (broadcast)");
	return tx_broadcast_command(COMMAND_STORE_ACTUAL_LEVEL_IN_DTR, true);
}

bool Dali::tx_set_power_on_level_from_dtr() {
	DALI_LOG(TAG, "Copy DTR to power on level (broadcast)");
	return tx_broadcast_command(COMMAND_SET_POWER_ON_LEVEL_FROM_DTR, true);
}

bool Dali::tx_set_system_failure_level_from_dtr() {
	DALI_LOG(TAG, "Copy DTR to system failure level (broadcast)");
	return tx_broadcast_command(COMMAND_SET_SYSTEM_FAILURE_LEVEL_FROM_DTR, true);
}
