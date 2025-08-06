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

#include <algorithm>
#include <vector>

#include "config.h"
#include "lights.h"
#include "util.h"

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

static constexpr unsigned int RX_GPIO = 40;
static constexpr unsigned int TX_GPIO = 21;

const rmt_data_t Dali::DALI_0 = {{{
	.duration0 = HALF_SYMBOL_TICKS, .level0 = BUS_RMT_LOW,
	.duration1 = HALF_SYMBOL_TICKS, .level1 = BUS_RMT_HIGH,
}}};

const rmt_data_t Dali::DALI_1 = {{{
	.duration0 = HALF_SYMBOL_TICKS, .level0 = BUS_RMT_HIGH,
	.duration1 = HALF_SYMBOL_TICKS, .level1 = BUS_RMT_LOW,
}}};

const rmt_data_t Dali::DALI_STOP_IDLE = {{{
	/* Stop bits */
	.duration0 = HALF_SYMBOL_TICKS * STOP_BITS * 2, .level0 = BUS_RMT_HIGH,
	/* Minimum idle time */
	.duration1 = HALF_SYMBOL_TICKS * IDLE_SYMBOLS * 2, .level1 = BUS_RMT_HIGH,
}}};

Dali::Dali(const Config &config, const Lights &lights)
		: WakeupThread("dali", true), config_(config),
		lights_(lights) {
	tx_levels_.fill(LEVEL_NO_CHANGE);
}

void Dali::setup() {
	pinMode(RX_GPIO, INPUT);
	pinMode(TX_GPIO, OUTPUT);
	digitalWrite(TX_GPIO, BUS_ARDUINO_HIGH);

	/* Idle state defaults to low, which is BUS_RMT_HIGH */
	rmt_ = rmtInit(TX_GPIO, RMT_TX_MODE, RMT_MEM_256);
	static_assert((uint32_t)(1000/12.5f) == 80U);
	rmtSetTick(rmt_, TICK_NS);
	tx_idle();

	std::thread t;
	make_thread(t, "dali", 8192, 1, 19, &Dali::run_loop, this);
	t.detach();
}

unsigned long Dali::run_tasks() {
	const auto lights = config_.get_addresses();
	unsigned long delay_ms = std::min(WATCHDOG_INTERVAL_MS,
		std::max(TX_POWER_LEVEL_MS, REFRESH_PERIOD_MS / std::max(1U, lights.count())));
	auto levels = lights_.get_levels();
	bool changed = false;

	/*
	 * Set power level for lights that have changed level, cycling through the
	 * addresses each time to avoid preferring low-numbered lights.
	 */
	for (unsigned int i = 0; i <= MAX_ADDR && !changed; i++) {
		unsigned address = next_address_;

		if (lights[address] && levels[address] != tx_levels_[address]) {
			if (tx_power_level(address, levels[address])) {
				tx_levels_[address] = levels[address];
				delay_ms = TX_POWER_LEVEL_MS;
				changed = true;
			} else {
				delay_ms = 0;
				goto done;
			}
		}

		next_address_++;
		next_address_ %= MAX_ADDR + 1;
	}

	/*
	* Refresh light power levels individually over a short time period,
	* cycling through the addresses each time to avoid preferring
	* low-numbered lights. Delays between lights keeps the bus idle most of
	* the time to improve responsiveness when dimming with a rotary encoder.
	*/
	for (unsigned int i = 0; i <= MAX_ADDR && !changed; i++) {
		unsigned address = next_address_;

		if (lights[address]) {
			if (tx_power_level(address, levels[address])) {
				tx_levels_[address] = levels[address];
				changed = true;
			} else {
				delay_ms = 0;
				goto done;
			}
		}

		next_address_++;
		next_address_ %= MAX_ADDR + 1;
	}

done:
	esp_task_wdt_reset();
	return delay_ms;
}

bool Dali::ready() {
	return rmt_wait_tx_done(static_cast<rmt_channel_t>(rmt_->channel),
		TX_POWER_LEVEL_MS / portTICK_PERIOD_MS) == ESP_OK;
}

inline void Dali::push_byte(std::vector<rmt_data_t> &symbols, uint8_t value) {
	for (int i = 7; i >= 0; i--) {
		symbols.push_back(((value >> i) & 1) ? DALI_1 : DALI_0);
	}
}

bool Dali::tx_idle() {
	if (!ready()) {
		return false;
	}

	//ESP_LOGE(TAG, "Idle");

	std::vector<rmt_data_t> symbols(1, DALI_STOP_IDLE);

	return rmtWrite(rmt_, symbols.data(), symbols.size());
}

bool Dali::tx_power_level(uint8_t address, uint8_t level) {
	if (address > MAX_ADDR) {
		return true;
	}

	if (!ready()) {
		return false;
	}

	//ESP_LOGE(TAG, "Power level %u = %u", address, level);

	std::vector<rmt_data_t> symbols;
	symbols.reserve(START_BITS + 8 + 8 + 1);

	symbols.push_back(DALI_1);
	push_byte(symbols, address << 1);
	push_byte(symbols, level);
	symbols.push_back(DALI_STOP_IDLE);

	rmtWrite(rmt_, symbols.data(), symbols.size());
	/* Always return success so we move on to the next light */
	return true;
}
