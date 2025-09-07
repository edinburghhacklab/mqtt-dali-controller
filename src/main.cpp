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

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include <functional>
#include <mutex>
#include <string>

#include "api.h"
#include "dali.h"
#include "config.h"
#include "lights.h"
#include "local_lights.h"
#include "network.h"
#include "remote_lights.h"
#include "switches.h"
#include "ui.h"
#include "util.h"

static constexpr const char *TAG = "main";

std::string FixedConfig::mqtt_topic_str{FixedConfig::MQTT_TOPIC};
std::string FixedConfig::mqtt_remote_topic_str{
	FixedConfig::MQTT_REMOTE_TOPIC != nullptr
		? std::string{FixedConfig::MQTT_REMOTE_TOPIC} + "/x" : ""};

/**
 * LittleFS is NOT thread-safe. Lock this global mutex when accessing the
 * filesystem.
 */
static std::mutex file_mutex;

static Network network;
static Selector selector;
static Config config{file_mutex, network, selector};
static LocalLights local_lights{network, config};
static RemoteLights remote_lights{network, config};
static Lights &lights = FixedConfig::isLocal()
	? static_cast<Lights&>(local_lights) : static_cast<Lights&>(remote_lights);
static UI ui{file_mutex, network, FixedConfig::isLocal() ? &local_lights : nullptr};
static API *api{nullptr};
static bool startup_watchdog{false};
static bool startup_watchdog_failed{false};

static bool ota_verification_pending() {
	esp_ota_img_states_t state;

	if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state)) {
		state = ESP_OTA_IMG_UNDEFINED;
	}

	return state == ESP_OTA_IMG_PENDING_VERIFY;
}

extern "C" {

bool verifyRollbackLater() {
	return true;
}

bool testSPIRAM() {
	return true;
}

}

void setup() {
	using namespace std::placeholders;

	if (ota_verification_pending()) {
		ESP_LOGE(TAG, "Startup watchdog started: OTA verification pending");
		ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
		startup_watchdog = true;
	}

	ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));

	Switches &switches = *new Switches{network, config, lights};
	Buttons &buttons = *new Buttons{config, lights};
	Dimmers &dimmers = *new Dimmers{network, config, lights};
	Dali &dali = *new Dali{config, local_lights};
	api = new API{file_mutex, network, config, dali, dimmers, lights, ui};

	if (FixedConfig::isLocal()) {
		dali.setup();
	}
	selector.setup();
	config.setup();
	if (FixedConfig::isLocal()) {
		local_lights.setup();
		switches.setup();
	}
	buttons.setup();
	dimmers.setup();
	if (FixedConfig::isLocal()) {
		dali.start();
	}
	ui.setup();

	if (FixedConfig::isLocal()) {
		local_lights.set_dali(dali);
		ui.set_dali(dali);
		ui.set_switches(switches);
	}

	network.setup(std::bind(&API::connected, api),
		std::bind(&API::receive, api, _1, _2));

	if (ota_verification_pending()) {
		const esp_app_desc_t *desc = esp_ota_get_app_description();

		network.report(TAG, std::string{"Running version: "} + null_terminated_string(desc->version) + " (verification pending)");
	}
}

void loop() {
	if (startup_watchdog) {
		if (api->startup_complete()) {
			ESP_LOGE(TAG, "Startup watchdog cancelled");
			esp_task_wdt_delete(nullptr);
			startup_watchdog = false;
		} else if (esp_timer_get_time() < ONE_M) {
			esp_task_wdt_reset();
		} else if (!startup_watchdog_failed) {
			ESP_LOGE(TAG, "Startup watchdog failure");
			startup_watchdog_failed = true;
		}
	}

	if (FixedConfig::isLocal()) {
		local_lights.loop();
	}
	ui.loop();
	network.loop();
	config.loop();
}
