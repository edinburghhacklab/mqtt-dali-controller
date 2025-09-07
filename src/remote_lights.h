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
#include "lights.h"
#include "util.h"

class Config;
class Network;

class RemoteLights: public Lights {
public:
	RemoteLights(Network &network, Config &config);

	void select_preset(std::string name, const std::string &light_ids, bool internal = false) override;
	void select_preset(std::string name, const std::vector<std::string> &groups, bool internal = false) override;
	void set_level(const std::string &light_ids, long level) override;
	void dim_adjust(unsigned int dimmer_id, long level) override;

private:
	static constexpr const char *TAG = "Lights";
	static constexpr auto MAX_LEVEL = Dali::MAX_LEVEL;

	Network &network_;
	Config &config_;
};
