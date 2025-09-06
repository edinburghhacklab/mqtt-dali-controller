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

static constexpr unsigned int NUM_BUTTONS = 4;

class Config;
class Lights;

class Buttons: public WakeupThread {
public:
	Buttons(const Config &config, Lights &lights);

	void setup();

private:
	static constexpr const char *TAG = "Buttons";
	static constexpr unsigned long DEBOUNCE_US = 50 * 1000;
	static constexpr unsigned long WATCHDOG_INTERVAL_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 4;

	~Buttons() = delete;

	unsigned long run_tasks() override;
	unsigned long run_button(unsigned int button_id);

	const Config &config_;
	Lights &lights_;

	std::array<Debounce,NUM_BUTTONS> debounce_;
};
