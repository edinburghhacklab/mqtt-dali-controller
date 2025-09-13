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

#include "dimmers.h"

#include <Arduino.h>
#include <esp_crc.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <string>

#include "config.h"
#include "lights.h"
#include "network.h"
#include "rotary_encoder.h"
#include "util.h"

static constexpr std::array<std::array<gpio_num_t,2>,NUM_DIMMERS> DIMMER_GPIO{{
	{(gpio_num_t)1, (gpio_num_t)2},
	{(gpio_num_t)3, (gpio_num_t)4},
}};

bool Dimmers::mode_value(const std::string &text, DimmerMode &mode) {
	if (text == "individual") {
		mode = DimmerMode::INDIVIDUAL;
		return true;
	} else if (text == "group") {
		mode = DimmerMode::GROUP;
		return true;
	}

	return false;
}

const char* Dimmers::mode_text(DimmerMode mode) {
	switch (mode) {
	case DimmerMode::INDIVIDUAL:
		return "individual";

	case DimmerMode::GROUP:
		return "group";
	}

	return "unknown";
}

Dimmers::Dimmers(Network &network, const Config &config, Lights &lights)
		: WakeupThread("dimmers", true), network_(network), config_(config),
		lights_(lights), encoder_({
			RotaryEncoder{DIMMER_GPIO[0]},
			RotaryEncoder{DIMMER_GPIO[1]},
		}) {
}

void Dimmers::setup() {
	for (unsigned int i = 0; i < NUM_DIMMERS; i++) {
		encoder_[i].start(*this);
	}

	std::thread t;
	make_thread(t, "dimmers", 8192, 1, 20, &Dimmers::run_loop, this);
	t.detach();
}

unsigned long Dimmers::run_tasks() {
	esp_task_wdt_reset();

	if (network_.busy()) {
		return 1;
	}

	for (unsigned int i = 0; i < NUM_DIMMERS; i++) {
		run_dimmer(i);
	}

	return WATCHDOG_INTERVAL_MS;
}

void Dimmers::run_dimmer(unsigned int dimmer_id) {
	long encoder_steps = config_.get_dimmer_encoder_steps(dimmer_id);
	long encoder_change = encoder_[dimmer_id].read();

	if (encoder_steps == 0) {
		state_[dimmer_id].encoder_steps = 0;
	} else {
		state_[dimmer_id].encoder_steps = std::max(-LONG_MAX,
			state_[dimmer_id].encoder_steps + encoder_change);
	}

	if (state_[dimmer_id].encoder_steps == 0) {
		return;
	}

	long abs_encoder_steps = std::abs(encoder_steps);
	bool encoder_forward = state_[dimmer_id].encoder_steps > 0;
	bool steps_forward = encoder_steps > 0;
	long change_count = std::abs(state_[dimmer_id].encoder_steps) / abs_encoder_steps;

	if (change_count == 0) {
		return;
	}

	if (!encoder_forward) {
		change_count = -change_count;
	}

	state_[dimmer_id].encoder_steps -= change_count * abs_encoder_steps;

	if (!steps_forward) {
		change_count = -change_count;
	}

	long level_steps = config_.get_dimmer_level_steps(dimmer_id);
	long level_change = std::max(-(long)MAX_LEVEL, std::min((long)MAX_LEVEL, change_count * level_steps));

	lights_.dim_adjust(dimmer_id, level_change);
}

void Dimmers::publish_debug(unsigned int dimmer_id) {
	static std::array<RotaryEncoderDebug,RotaryEncoder::DEBUG_RECORDS> records;

	if (dimmer_id >= NUM_DIMMERS) {
		return;
	}

	std::string topic = FixedConfig::mqttTopic("/dimmer/")
		+ std::to_string(dimmer_id) + "/debug_log";

	encoder_[dimmer_id].debug(records);

	for (const auto &record : records) {
		std::string output;

		output += std::to_string(record.time_us);
		output += ' ';
		if (record.state) {
			output += record.pin == 0 ? 'A' : 'B';
		} else {
			output += record.pin == 0 ? 'a' : 'b';
		}

		network_.publish(topic, output);
	}
}
