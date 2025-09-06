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
#include <string>

#include "dali.h"
#include "rotary_encoder.h"
#include "thread.h"
#include "util.h"

static constexpr unsigned int NUM_DIMMERS = 2;
static constexpr int MIN_ENCODER_STEPS = -127;
static constexpr int MAX_ENCODER_STEPS = 127;

class Config;
class Lights;
class Network;

enum DimmerMode {
	INDIVIDUAL = 0,
	GROUP,
};

struct DimmerState {
public:
	DimmerState() = default;

	long encoder_steps{0};
};

class Dimmers: public WakeupThread {
public:
	Dimmers(Network &network, const Config &config, Lights &lights);

	static bool mode_value(const std::string &text, DimmerMode &mode);
	static const char* mode_text(DimmerMode mode);

	void setup();
	void publish_debug(unsigned int dimmer_id);

private:
	static constexpr const char *TAG = "Dimmers";
	static constexpr auto MAX_LEVEL = Dali::MAX_LEVEL;
	static constexpr unsigned long WATCHDOG_INTERVAL_MS = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 4;

	~Dimmers() = delete;

	unsigned long run_tasks() override;
	void run_dimmer(unsigned int dimmer_id);

	Network &network_;
	const Config &config_;
	Lights &lights_;

	std::array<RotaryEncoder,NUM_DIMMERS> encoder_;
	std::array<DimmerState,NUM_DIMMERS> state_;
};
