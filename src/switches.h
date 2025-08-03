/*
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

static constexpr unsigned int NUM_SWITCHES = 2;

class Config;
class Lights;
class Network;

struct SwitchState {
	SwitchState() : value(LOW), report_us(0) {}

	int value;
	uint64_t report_us;
};

class Switches {
public:
    Switches(Network &network, Config &config, Lights &lights);

    void setup();
    void loop();

private:
    Network &network_;
    Config &config_;
    Lights &lights_;
    std::array<SwitchState,NUM_SWITCHES> state_;
};
