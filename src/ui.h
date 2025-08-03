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
#include <Adafruit_NeoPixel.h>

class Network;

class UI {
public:
    UI(Network &network);

    void setup();
    void loop();
    void startup_complete(bool state);

private:
    static constexpr unsigned int LED_GPIO = 38;

    void publish_uptime();

    Network &network_;
    Adafruit_NeoPixel led_{1, LED_GPIO, NEO_GRB | NEO_KHZ800};
    uint64_t last_led_us_{0};
    uint64_t last_publish_us_{0};
    bool startup_complete_{false};
};
