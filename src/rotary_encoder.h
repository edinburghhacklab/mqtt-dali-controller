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

#include <driver/gpio.h>
#include <esp_attr.h>

#include <atomic>
#include <array>

class WakeupThread;

IRAM_ATTR void rotary_encoder_interrupt_handler_0(void *arg);
IRAM_ATTR void rotary_encoder_interrupt_handler_1(void *arg);

struct RotaryEncoderDebug {
	uint32_t pin:1;
	uint32_t state:1;
	uint32_t time_us:30;
};

class RotaryEncoder {
	friend void rotary_encoder_interrupt_handler_0(void *arg);
	friend void rotary_encoder_interrupt_handler_1(void *arg);

public:
	/*
	 * One rotation has 100 positions, each with 4 state changes.
	 * Record the last 2 full rotations.
	 */
	static constexpr size_t DEBUG_RECORDS = 800;

	RotaryEncoder(std::array<gpio_num_t,2> pins);
	~RotaryEncoder();

	void start(WakeupThread &wakeup);
	long read();
	void debug(std::array<RotaryEncoderDebug,DEBUG_RECORDS> &records) const;

private:
	// cppcheck-suppress unusedPrivateFunction
	IRAM_ATTR void interrupt_handler(int pin_id);

	WakeupThread *wakeup_{nullptr};
	const std::array<gpio_num_t,2> pins_;
	bool state_[2];
	int first_{-1};

	std::atomic<long> change_{0};
	std::array<RotaryEncoderDebug,DEBUG_RECORDS> debug_{};
	size_t debug_pos_{0};
};
