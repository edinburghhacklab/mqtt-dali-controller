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
#include <utility>

class WakeupThread;

IRAM_ATTR void rotary_encoder_interrupt_handler_0(void *arg);
IRAM_ATTR void rotary_encoder_interrupt_handler_1(void *arg);

struct RotaryEncoderDebug {
	uint32_t pin:1;
	uint32_t level:1;
	uint32_t time_us:30;
};

class RotarySignal {
public:
	bool active_low{true};
	bool level{true};
	uint64_t active_us{0};

	inline bool active() const { return level ^ active_low; }
};

enum RotaryMode {
	NOT_AB = 0,
	A_NOT_B,
	B_NOT_A,
	AB
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

	explicit RotaryEncoder(std::array<gpio_num_t,2> pins);
	~RotaryEncoder();

	void start(WakeupThread &wakeup);
	std::pair<RotaryMode,long> read();
	void debug(std::array<RotaryEncoderDebug,DEBUG_RECORDS> &records) const;

private:
	static constexpr uint64_t ACTIVE_CHANGE_US = 250000;
	static constexpr uint64_t ENCODER_CHANGE_US = 125000;

	IRAM_ATTR void update_mode();

	// cppcheck-suppress unusedPrivateFunction
	IRAM_ATTR void interrupt_handler(int pin_id);

	WakeupThread *wakeup_{nullptr};
	const std::array<gpio_num_t,2> pins_;
	std::array<RotarySignal,2> signal_{};
	unsigned int count_{0};
	uint64_t start_us_{0};
	int first_{0};

	std::atomic<long> change_{0};
	std::atomic<RotaryMode> mode_{RotaryMode::NOT_AB};
	std::array<RotaryEncoderDebug,DEBUG_RECORDS> debug_{};
	size_t debug_pos_{0};
};
