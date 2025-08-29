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

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dali.h"
#include "util.h"

static const std::string RESERVED_PRESET_CUSTOM = "custom";
static const std::string RESERVED_PRESET_UNKNOWN = "unknown";

class Config;
class Network;

struct LightsState {
	Dali::addresses_t addresses; /**< Valid addresses */
	std::array<Dali::addresses_t,Dali::num_groups> group_addresses; /**< Group members */
	std::array<Dali::level_fast_t,Dali::num_addresses> levels; /**< Individual light levels */
	std::array<Dali::level_fast_t,Dali::num_groups> group_levels; /**< Group light levels */
	Dali::addresses_t group_level_addresses; /**< Individual lights where the level has been set on the group */
	Dali::level_fast_t broadcast_level; /**< Broadcast light level */
	Dali::groups_t group_sync; /**< Sync group members to DALI bus */
	Dali::addresses_t force_refresh; /**< Force refresh individual lights */
	bool broadcast_power_on_level; /**< Broadcast store of power on level to DALI bus */
	bool broadcast_system_failure_level;/**< Broadcast store of system failure level to DALI bus */
};

class Lights {
public:
	Lights(Network &network, const Config &config);

	static std::string rtc_boot_memory();

	void setup();
	void set_dali(Dali &dali);
	void loop();
	void startup_complete(bool state);

	BootRTCStatus rtc_boot_status() const;

	void address_config_changed();
	void address_config_changed(const std::string &group);

	LightsState get_state() const;
	void completed_force_refresh(unsigned int light_id) const;
	void select_preset(std::string name, const std::string &light_ids, bool internal = false);
	void set_level(const std::string &light_ids, long level);
	void set_power(const Dali::addresses_t &lights, bool on);
	void dim_adjust(unsigned int dimmer_id, long level);

	void request_group_sync();
	void request_group_sync(const std::string &group);
	void completed_group_sync(Dali::group_t group) const;

	void request_broadcast_power_on_level();
	void request_broadcast_system_failure_level();
	void completed_broadcast_power_on_level() const;
	void completed_broadcast_system_failure_level() const;

private:
	static constexpr const char *TAG = "Lights";
	static constexpr auto MAX_LEVEL = Dali::MAX_LEVEL;
	static constexpr size_t REPUBLISH_PER_PERIOD = 5;
	static constexpr uint64_t IDLE_US = 10 * ONE_S;
	static constexpr uint64_t DIM_REPORT_DELAY_US = 5 * ONE_S;
	static constexpr unsigned int FORCE_REFRESH_COUNT = 2;
	static constexpr unsigned int LEVEL_PRESENT = (1U << 8);
	static constexpr unsigned int LEVEL_POWER_ON = (1U << 9);
	static constexpr unsigned int LEVEL_POWER_OFF = (1U << 10);
	static constexpr unsigned int LEVEL_GROUPED = (1U << 11);
	static constexpr size_t RTC_LEVELS_SIZE = (Dali::num_addresses + 3) / 4;
	static constexpr uint32_t RTC_MAGIC = 0x0d1325ab;

	static uint32_t rtc_crc(const std::array<uint32_t,RTC_LEVELS_SIZE> &levels);

	void publish_active_presets();
	void publish_levels(bool force);
	void clear_group_levels(const Dali::addresses_t &lights);
	void report_dimmed_levels(const Dali::addresses_t &lights, uint64_t time_us);
	void clear_dimmed_levels(const Dali::addresses_t &lights);
	bool is_idle();

	void load_rtc_state();
	void save_rtc_state();

	static uint32_t rtc_levels_[RTC_LEVELS_SIZE];
	static uint32_t rtc_crc_;

	Network &network_;
	const Config &config_;
	Dali *dali_{nullptr};
	BootRTCStatus boot_rtc_{BootRTCStatus::UNKNOWN};

	mutable std::recursive_mutex lights_mutex_;
	std::array<Dali::level_fast_t,Dali::num_addresses> levels_{};
	std::array<Dali::level_fast_t,Dali::num_groups> group_levels_{};
	Dali::level_fast_t broadcast_level_{Dali::LEVEL_NO_CHANGE};
	Dali::addresses_t group_level_addresses_{};
	mutable Dali::groups_t group_sync_{};
	mutable Dali::addresses_t force_refresh_;
	mutable bool broadcast_power_on_level_{false};
	mutable bool broadcast_system_failure_level_{false};
	Dali::addresses_t power_on_;
	Dali::addresses_t power_known_;
	std::array<uint64_t,Dali::num_addresses> dim_time_us_{};
	mutable std::array<unsigned int,Dali::num_addresses> force_refresh_count_{};
	uint64_t last_publish_levels_us_{0};
	uint64_t last_activity_us_{0};

	std::mutex publish_mutex_;
	bool startup_complete_{false};
	std::array<std::string,Dali::num_addresses> active_presets_{};
	std::unordered_set<std::string> republish_groups_;
	std::unordered_set<std::string> republish_presets_;
	uint64_t last_publish_active_us_{0};
	size_t publish_index_{0};
};
