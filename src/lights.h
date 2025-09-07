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

#include <string>
#include <vector>

#include "config.h"
#include "util.h"

static const std::string RESERVED_PRESET_CUSTOM = "custom";
static const std::string RESERVED_PRESET_UNKNOWN = "unknown";

class Lights {
public:
	Lights() = default;
	virtual ~Lights() = default;

	virtual void setup() {};
	virtual void loop() {};
	virtual void startup_complete(bool state) {};

	virtual void address_config_changed() {};
	virtual void address_config_changed(const std::string &group) {};

	virtual void select_preset(std::string name, const std::string &light_ids, bool internal = false) = 0;
	virtual void select_preset(std::string name, const std::vector<std::string> &groups, bool internal = false) = 0;
	virtual void set_level(const std::string &light_ids, long level) = 0;
	virtual void set_power(const Dali::addresses_t &lights, bool on) {};
	virtual void dim_adjust(unsigned int dimmer_id, long level) = 0;
	virtual void dim_adjust(DimmerMode mode, const std::string &groups, long level) {};

	virtual void request_group_sync() {};
	virtual void request_group_sync(const std::string &group) {};

	virtual void request_broadcast_power_on_level() {};
	virtual void request_broadcast_system_failure_level() {};
};
