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

#include "lights.h"

#include <Arduino.h>
#include <esp_crc.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "dali.h"
#include "dimmers.h"
#include "network.h"
#include "util.h"

static_assert(NUM_DIMMERS <= Dali::num_groups);

RTC_NOINIT_ATTR uint32_t Lights::rtc_levels_[RTC_LEVELS_SIZE];
RTC_NOINIT_ATTR uint32_t Lights::rtc_crc_;

Lights::Lights(Network &network, const Config &config)
		: network_(network), config_(config) {
	levels_.fill(Dali::LEVEL_NO_CHANGE);
	group_levels_.fill(Dali::LEVEL_NO_CHANGE);
	active_presets_.fill(RESERVED_PRESET_UNKNOWN);
	republish_presets_.insert(BUILTIN_PRESET_OFF);
	republish_presets_.insert(RESERVED_PRESET_CUSTOM);
}

void Lights::setup() {
	load_rtc_state();
}

void Lights::set_dali(Dali &dali) {
	dali_ = &dali;
}

void Lights::loop() {
	if (startup_complete_ && network_.connected()) {
		Dali::addresses_t lights;

		lights.set();

		report_dimmed_levels(lights, DIM_REPORT_DELAY_US);
		publish_levels(false);
		publish_active_presets();
	}
}

void Lights::startup_complete(bool state) {
	std::lock_guard lock{publish_mutex_};

	startup_complete_ = state;
}

std::string Lights::rtc_boot_memory() {
	std::vector<char> buffer(64);

	snprintf(buffer.data(), buffer.size(), "%p+%zu, %p+%zu",
		&rtc_crc_, sizeof(rtc_crc_),
		rtc_levels_, sizeof(rtc_levels_));

	return {buffer.data()};
}

BootRTCStatus Lights::rtc_boot_status() const {
	return boot_rtc_;
}

void Lights::address_config_changed() {
	std::lock_guard publish_lock{publish_mutex_};
	auto groups = config_.group_names();

	republish_groups_.insert(groups.begin(), groups.end());

	std::lock_guard lights_lock{lights_mutex_};
	auto addresses = config_.get_addresses();

	group_level_addresses_ &= addresses;
}

void Lights::address_config_changed(const std::string &group) {
	std::lock_guard lock{publish_mutex_};

	republish_groups_.insert(group);
}

LightsState Lights::get_state() const {
	std::lock_guard lock{lights_mutex_};

	return {
		.addresses{config_.get_addresses()},
		.group_addresses{config_.get_group_addresses()},
		.levels{levels_},
		.group_levels{group_levels_},
		.group_level_addresses{group_level_addresses_},
		.broadcast_level = broadcast_level_,
		.group_sync{group_sync_},
		.force_refresh{force_refresh_},
		.broadcast_power_on_level = broadcast_power_on_level_,
		.broadcast_system_failure_level = broadcast_system_failure_level_,
	};
}

void Lights::completed_force_refresh(unsigned int light_id) const {
	if (light_id >= force_refresh_count_.size()) {
		return;
	}

	std::lock_guard lock{lights_mutex_};

	if (force_refresh_count_[light_id] > 0) {
		force_refresh_count_[light_id]--;
	}

	force_refresh_[light_id] = force_refresh_count_[light_id] > 0;
}

bool Lights::is_idle() {
	return esp_timer_get_time() - last_activity_us_ >= IDLE_US;
}

uint32_t Lights::rtc_crc(const std::array<uint32_t,RTC_LEVELS_SIZE> &levels) {
	return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&levels), sizeof(levels)) ^ RTC_MAGIC;
}

void Lights::load_rtc_state() {
	ESP_LOGE(TAG, "RTC state at %s", rtc_boot_memory().c_str());

	if (esp_reset_reason() == ESP_RST_POWERON) {
		ESP_LOGE(TAG, "Ignoring light levels in RTC memory, first power on");
		boot_rtc_ = BootRTCStatus::POWER_ON_IGNORED;
		return;
	}

	std::array<uint32_t,RTC_LEVELS_SIZE> levels{};

	for (unsigned int i = 0; i < RTC_LEVELS_SIZE; i++) {
		levels[i] = rtc_levels_[i];
	}

	uint32_t expected_crc = rtc_crc(levels);

	if (rtc_crc_ == expected_crc) {
		ESP_LOGE(TAG, "Restoring light levels from RTC memory");

		for (unsigned int i = 0; i < levels_.size(); i++) {
			levels_[i] = (levels[i/4] >> (8 * (i % 4))) & 0xFFU;
		}

		boot_rtc_ = BootRTCStatus::LOADED_OK;
	} else {
		ESP_LOGE(TAG, "Ignoring light levels in RTC memory, checksum mismatch 0x%08X != 0x%08X",
			rtc_crc_, expected_crc);
		boot_rtc_ = BootRTCStatus::CHECKSUM_MISMATCH;
	}
}

void Lights::save_rtc_state() {
	std::array<uint32_t,RTC_LEVELS_SIZE> levels{};

	for (unsigned int i = 0; i < levels_.size(); i++) {
		levels[i/4] |= levels_[i] << (8 * (i % 4));
	}

	for (unsigned int i = 0; i < levels.size(); i++) {
		rtc_levels_[i] = levels[i];
	}

	rtc_crc_ = rtc_crc(levels);
}

void Lights::select_preset(std::string name, const std::string &light_ids, bool internal) {
	const auto addresses = config_.get_addresses();
	bool idle_only;
	const auto lights = config_.parse_light_ids(light_ids, idle_only);
	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	std::array<Dali::level_fast_t,Dali::num_addresses> preset_levels;
	unsigned long long ordered_value;
	bool changed = false;

	if (name.empty()) {
		return;
	}

	if (ulonglong_from_string(name, ordered_value)) {
		if (!config_.get_ordered_preset(ordered_value, name)) {
			return;
		}
	}

	if (!config_.get_preset(name, preset_levels)) {
		return;
	}

	if (!internal && idle_only && !is_idle()) {
		network_.report(TAG, config_.lights_text(lights) + " = " + name + " (ignored - not idle)");
		return;
	}

	if (internal) {
		clear_dimmed_levels(lights);
	} else {
		report_dimmed_levels(lights, 0);
	}

	clear_group_levels(lights);

	for (int i = 0; i < levels_.size(); i++) {
		if (addresses[i]) {
			if (preset_levels[i] != Dali::LEVEL_NO_CHANGE) {
				if (lights[i]) {
					levels_[i] = preset_levels[i];
					if (active_presets_[i] != name) {
						republish_presets_.insert(active_presets_[i]);
						republish_presets_.insert(name);
						active_presets_[i] = name;
					}
					changed = true;
				}
			}
		} else {
			if (!active_presets_[i].empty()) {
				republish_presets_.insert(active_presets_[i]);
				active_presets_[i] = "";
			}
		}
	}

	if (!idle_only) {
		last_activity_us_ = esp_timer_get_time();
	}

	if (changed) {
		save_rtc_state();

		if (!internal) {
			network_.report(TAG, config_.lights_text(lights) + " = " + name + (idle_only ? " (idle only)" : ""));
		}

		publish_levels(true);

		if (dali_) {
			dali_->wake_up();
		}
	}
}

void Lights::set_level(const std::string &light_ids, long level) {
	if (level < 0 || level > MAX_LEVEL) {
		return;
	}

	const auto addresses = config_.get_addresses();
	bool idle_only;
	const auto lights = config_.parse_light_ids(light_ids, idle_only);
	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	bool changed = false;

	if (idle_only && !is_idle()) {
		return;
	}

	report_dimmed_levels(lights, 0);
	clear_group_levels(lights);

	for (int i = 0; i < levels_.size(); i++) {
		if (!addresses[i] || !lights[i]) {
			continue;
		}

		levels_[i] = level;
		if (active_presets_[i] != RESERVED_PRESET_CUSTOM) {
			republish_presets_.insert(active_presets_[i]);
			republish_presets_.insert(RESERVED_PRESET_CUSTOM);
			active_presets_[i] = RESERVED_PRESET_CUSTOM;
		}
		changed = true;
	}

	last_activity_us_ = esp_timer_get_time();

	if (changed) {
		save_rtc_state();

		network_.report(TAG, config_.lights_text(lights) + " = " + std::to_string(level));

		publish_levels(true);

		if (dali_) {
			dali_->wake_up();
		}
	}
}

void Lights::set_power(const Dali::addresses_t &lights, bool on) {
	std::lock_guard lock{lights_mutex_};

	power_known_ |= lights;

	if (on) {
		if ((lights & ~power_on_).any()) {
			/*
			 * The lights will already be considered set to the current level so
			 * it's necessary to force the Dali transmit loop to resend the
			 * power level in case the lights no longer remember it.
			 */

			for (unsigned int i = 0; i < lights.size(); i++) {
				if (lights[i] && !power_on_[i]) {
					force_refresh_count_[i] = FORCE_REFRESH_COUNT;
					force_refresh_[i] = true;
				}
			}

			if (dali_) {
				dali_->wake_up();
			}
		}

		power_on_ |= lights;
	} else {
		if ((power_on_ & lights).any()) {
			for (unsigned int i = 0; i < lights.size(); i++) {
				if (lights[i]) {
					force_refresh_count_[i] = 0;
					force_refresh_[i] = false;
				}
			}
		}

		power_on_ &= ~lights;
	}
}

void Lights::dim_adjust(unsigned int dimmer_id, long level) {
	if (dimmer_id >= NUM_DIMMERS) {
		return;
	}

	if (dim_adjust(config_.get_dimmer(dimmer_id), level)) {
		network_.publish(FixedConfig::mqttTopic("/dimmer/")
				+ std::to_string(dimmer_id) + "/change", std::to_string(level));
	}
}

void Lights::dim_adjust(DimmerMode mode, const std::string &groups, long level) {
	dim_adjust(config_.make_dimmer(mode, groups), level);
}

bool Lights::dim_adjust(DimmerConfig &&dimmer_config, long level) {
	if (level < -(long)MAX_LEVEL || level > (long)MAX_LEVEL) {
		return false;
	}

	std::lock_guard publish_lock{publish_mutex_};
	std::lock_guard lights_lock{lights_mutex_};
	uint64_t now = esp_timer_get_time();
	bool changed;
	std::array<long,Dali::num_groups> group_level{};
	long broadcast_level = 0;

	if (dimmer_config.mode == DimmerMode::GROUP) {
		if (dimmer_config.all) {
			unsigned int count = 0;

			for (unsigned int address = 0; address < dimmer_config.addresses.size(); address++) {
				if (!dimmer_config.addresses[address]) {
					continue;
				}

				if (levels_[address] != Dali::LEVEL_NO_CHANGE) {
					broadcast_level += levels_[address];
					count++;
				}
			}

			if (count > 0) {
				if (level >= 0) {
					/* Dimming up: round down */
					broadcast_level = broadcast_level / count;
				} else {
					/* Dimming down: round up */
					broadcast_level = (broadcast_level + (count - 1)) / count;
				}

				broadcast_level = std::max(0L, std::min((long)MAX_LEVEL, broadcast_level + level));
				broadcast_level_ = broadcast_level;
				group_level_addresses_ |= dimmer_config.addresses;
				changed = true;
			}
		} else {
			for (unsigned int group = 0; group < dimmer_config.groups.size(); group++) {
				unsigned int count = 0;

				if (!dimmer_config.groups[group]) {
					continue;
				}

				for (unsigned int address = 0; address < dimmer_config.addresses.size(); address++) {
					if (dimmer_config.address_group[address] != group) {
						continue;
					}

					if (levels_[address] != Dali::LEVEL_NO_CHANGE) {
						group_level[group] += levels_[address];
						count++;
					}
				}

				if (count > 0) {
					if (level >= 0) {
						/* Dimming up: round down */
						group_level[group] = group_level[group] / count;
					} else {
						/* Dimming down: round up */
						group_level[group] = (group_level[group] + (count - 1)) / count;
					}

					group_level[group] = std::max(0L, std::min((long)MAX_LEVEL, group_level[group] + level));
					group_levels_[group] = group_level[group];
					group_level_addresses_ |= dimmer_config.group_addresses[group];
					changed = true;
				}
			}
		}
	} else {
		clear_group_levels(dimmer_config.addresses);
	}

	for (int i = 0; i < levels_.size(); i++) {
		if (!dimmer_config.addresses[i]) {
			continue;
		}

		if (dimmer_config.mode == DimmerMode::GROUP) {
			if (dimmer_config.all) {
				levels_[i] = broadcast_level;
			} else {
				levels_[i] = group_level[dimmer_config.address_group[i]];
			}
		} else if (levels_[i] == Dali::LEVEL_NO_CHANGE) {
			continue;
		} else {
			levels_[i] = std::max(0L, std::min((long)MAX_LEVEL, (long)levels_[i] + level));
		}

		dim_time_us_[i] = now;
		if (active_presets_[i] != RESERVED_PRESET_CUSTOM) {
			republish_presets_.insert(active_presets_[i]);
			republish_presets_.insert(RESERVED_PRESET_CUSTOM);
			active_presets_[i] = RESERVED_PRESET_CUSTOM;
		}
		changed = true;
	}

	last_activity_us_ = esp_timer_get_time();

	if (changed) {
		save_rtc_state();

		publish_levels(true);

		if (dali_) {
			dali_->wake_up();
		}
	}

	return changed;
}

void Lights::request_group_sync() {
	std::lock_guard lock{lights_mutex_};

	group_sync_.set();

	network_.report(TAG, "Queued group sync for all groups");

	if (dali_) {
		dali_->wake_up();
	}
}

void Lights::request_group_sync(const std::string &group) {
	std::lock_guard lock{lights_mutex_};
	auto id = config_.get_group_id(group);

	if (id < group_sync_.size()) {
		group_sync_[id] = true;

		network_.report(TAG, "Queued group sync for " + group + " (" + std::to_string(id) + ")");

		if (dali_) {
			dali_->wake_up();
		}
	}
}

void Lights::completed_group_sync(Dali::group_t group) const {
	std::lock_guard lock{lights_mutex_};

	if (group < group_sync_.size()) {
		group_sync_[group] = false;

		if (group_sync_.none()) {
			network_.report(TAG, "Completed group sync commands");
		}
	}
}

void Lights::request_broadcast_power_on_level() {
	std::lock_guard lock{lights_mutex_};

	broadcast_power_on_level_ = true;

	network_.report(TAG, "Queued broadcast to configure power on level");

	if (dali_) {
		dali_->wake_up();
	}
}

void Lights::completed_broadcast_power_on_level() const {
	std::lock_guard lock{lights_mutex_};

	broadcast_power_on_level_ = false;

	network_.report(TAG, "Completed broadcast to configure power on level");
}

void Lights::request_broadcast_system_failure_level() {
	std::lock_guard lock{lights_mutex_};

	broadcast_system_failure_level_ = true;

	network_.report(TAG, "Queued broadcast to configure system failure level");

	if (dali_) {
		dali_->wake_up();
	}
}

void Lights::completed_broadcast_system_failure_level() const {
	std::lock_guard lock{lights_mutex_};

	broadcast_system_failure_level_ = false;

	network_.report(TAG, "Completed broadcast to configure system failure level");
}

void Lights::clear_group_levels(const Dali::addresses_t &lights) {
	Dali::addresses_t clear_lights{lights};

	/* Clear group level when setting individual light levels */
	for (Dali::group_fast_t i = 0; i < Dali::num_groups; i++) {
		if (group_levels_[i] != Dali::LEVEL_NO_CHANGE) {
			auto addresses = config_.get_group_addresses(i);

			if ((lights & addresses).any()) {
				group_levels_[i] = Dali::LEVEL_NO_CHANGE;

				/* All lights in the group now get updated individually */
				clear_lights |= addresses;
			}
		}
	}

	group_level_addresses_ &= ~clear_lights;
}

void Lights::report_dimmed_levels(const Dali::addresses_t &lights, uint64_t time_us) {
	std::lock_guard lock{lights_mutex_};
	Dali::addresses_t dimmed_lights;
	Dali::level_fast_t min_level = MAX_LEVEL;
	Dali::level_fast_t max_level = 0;
	uint64_t now = esp_timer_get_time();

	for (unsigned int i = 0; i < lights.size(); i++) {
		if (lights[i] && dim_time_us_[i] && now - dim_time_us_[i] >= time_us) {
			dimmed_lights[i] = true;
			min_level = std::min(min_level, levels_[i]);
			max_level = std::max(max_level, levels_[i]);
			dim_time_us_[i] = 0;
		}
	}

	if (dimmed_lights.count()) {
		if (min_level == max_level) {
			network_.report(TAG, config_.lights_text(dimmed_lights) + " = "
				+ std::to_string(min_level) + " (dimmer)");
		} else {
			network_.report(TAG, config_.lights_text(dimmed_lights) + " = "
				+ std::to_string(min_level) + ".." + std::to_string(max_level) + " (dimmer)");
		}
	}
}

void Lights::clear_dimmed_levels(const Dali::addresses_t &lights) {
	for (unsigned int i = 0; i < lights.size(); i++) {
		if (lights[i]) {
			dim_time_us_[i] = 0;
		}
	}
}

void Lights::publish_active_presets() {
	std::lock_guard publish_lock{publish_mutex_};
	bool force = (!last_publish_active_us_ || esp_timer_get_time() - last_publish_active_us_ >= ONE_M);

	if (!force && republish_groups_.empty() && republish_presets_.empty()) {
		return;
	}

	const auto groups = config_.group_names();
	const auto presets = config_.preset_names();
	size_t i = 0;

	for (const auto &group : groups) {
		const auto lights = config_.get_group_addresses(group);
		bool republish_group = republish_groups_.find(group) != republish_groups_.end();

		for (const auto &preset : presets) {
			bool republish_preset = republish_presets_.find(preset) != republish_presets_.end();

			if (republish_group || republish_preset
					|| (force && i >= publish_index_
						&& i < publish_index_ + REPUBLISH_PER_PERIOD)) {
				bool is_active = false;

				for (unsigned int j = 0; j < lights.size(); j++) {
					if (lights[j] && active_presets_[j] == preset) {
						is_active = true;
						break;
					}
				}

				network_.publish(FixedConfig::mqttTopic("/active/")
					+ group + "/" + preset,
					is_active ? "1" : "0", true);
			}

			i++;
		}
	}

	republish_groups_.clear();
	republish_presets_.clear();

	if (force) {
		/*
		 * Republish only one of the groups every time because the total message
		 * count can get very high (groups * presets).
		 */
		publish_index_ += REPUBLISH_PER_PERIOD;
		publish_index_ %= groups.size() * presets.size();
		last_publish_active_us_ = esp_timer_get_time();
	}
}

void Lights::publish_levels(bool force) {
	std::lock_guard lock{lights_mutex_};

	if (!force && last_publish_levels_us_
			&& esp_timer_get_time() - last_publish_levels_us_ < ONE_M) {
		return;
	}

	const auto addresses = config_.get_addresses();
	std::vector<char> buffer(3 * levels_.size() + 1);
	size_t offset = 0;

	for (unsigned int i = 0; i < levels_.size(); i++) {
		unsigned int value = (levels_[i] & 0xFFU);

		if (addresses[i]) {
			value |= LEVEL_PRESENT;
		}

		if (power_known_[i]) {
			value |= power_on_[i] ? LEVEL_POWER_ON : LEVEL_POWER_OFF;
		}

		if (group_level_addresses_[i]) {
			value |= LEVEL_GROUPED;
		}

		snprintf(&buffer[offset], 4, "%03X", value);
		offset += 3;
	}

	network_.publish(FixedConfig::mqttTopic("/levels"),
		{buffer.data(), offset}, true);
	if (!force) {
		network_.publish(FixedConfig::mqttTopic("/idle_us"),
			std::to_string(esp_timer_get_time() - last_activity_us_));
	}
	last_publish_levels_us_ = esp_timer_get_time();
}
