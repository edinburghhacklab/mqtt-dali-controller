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

#include "remote_lights.h"

#include <Arduino.h>

#include <string>
#include <vector>

#include "config.h"
#include "dimmers.h"
#include "lights.h"
#include "network.h"
#include "util.h"

RemoteLights::RemoteLights(Network &network, Config &config)
		: network_(network), config_(config) {
}

void RemoteLights::select_preset(std::string name, const std::string &light_ids,
		bool internal) {
	network_.publish(FixedConfig::mqttRemoteTopic(),
		std::string{"pt "} + name + ' ' + light_ids);
}

void RemoteLights::select_preset(std::string name,
		const std::vector<std::string> &groups, bool internal) {
	network_.publish(FixedConfig::mqttRemoteTopic(),
		std::string{"pt "} + name + ' ' + vector_text(groups));
}

void RemoteLights::set_level(const std::string &light_ids, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	network_.publish(FixedConfig::mqttRemoteTopic(),
		std::string{"sl "} + light_ids + ' ' + std::to_string(level));
}

void RemoteLights::dim_adjust(unsigned int dimmer_id, long level) {
	if (dimmer_id >= NUM_DIMMERS) {
		return;
	}

	if (level < -(long)MAX_LEVEL || level > (long)MAX_LEVEL) {
		return;
	}

	std::string payload;

	switch (config_.get_dimmer_mode(dimmer_id)) {
	case DimmerMode::INDIVIDUAL:
		payload = "di ";
		break;

	case DimmerMode::GROUP:
		payload = "dg ";
		break;

	default:
		return;
	}

	payload += std::to_string(level);
	payload += ' ';
	payload += vector_text(config_.dimmer_active_groups(dimmer_id));

	network_.publish(FixedConfig::mqttRemoteTopic(), payload, false, true);
}
