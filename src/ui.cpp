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

#include "ui.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <esp_crt_bundle.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <FS.h>
#include <LittleFS.h>

#include <cstring>
#include <mutex>
#include <string>

#include "dali.h"
#include "lights.h"
#include "network.h"
#include "switches.h"
#include "util.h"

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static constexpr auto &FS = LittleFS;

UI::UI(std::mutex &file_mutex, Network &network, Lights &lights)
		: network_(network), lights_(lights), file_mutex_(file_mutex) {
}

void UI::startup_complete(bool state) {
	startup_complete_ = state;

	if (startup_complete_) {
		status_report();
	}
}

template<typename T, size_t size>
static inline std::string null_terminated_string(T(&data)[size]) {
	T *found = reinterpret_cast<T*>(std::memchr(&data[0], '\0', size));
	return std::string{&data[0], found ? (found - &data[0]) : size};
};

static const char *ota_state_string(esp_ota_img_states_t state) {
	switch (state) {
	case ESP_OTA_IMG_NEW: return "new";
	case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
	case ESP_OTA_IMG_VALID: return "valid";
	case ESP_OTA_IMG_INVALID: return "invalid";
	case ESP_OTA_IMG_ABORTED: return "aborted";
	case ESP_OTA_IMG_UNDEFINED: return "undefined";
	}

	return "unknown";
}

static const char *boot_rtc_status_string(BootRTCStatus value) {
	switch (value) {
	case UNKNOWN: return "unknown";
	case POWER_ON_IGNORED: return "power-on-ignored";
	case CHECKSUM_MISMATCH: return "checksum-mismatch";
	case LOADED_OK: return "loaded-ok";
	}

	return "invalid";
}

void UI::status_report() {
	publish_application();
	publish_boot();
	publish_partitions();
	publish_stats();
}

void UI::publish_application() {
	const esp_app_desc_t *desc = esp_ota_get_app_description();
	std::string topic = FixedConfig::mqttTopic("/application");

	network_.publish(topic + "/name", null_terminated_string(desc->project_name), true);
	network_.publish(topic + "/version", null_terminated_string(desc->version), true);
	network_.publish(topic + "/idf_ver", null_terminated_string(desc->idf_ver), true);
	network_.publish(topic + "/timestamp", null_terminated_string(desc->date) + " " + null_terminated_string(desc->time), true);
}

void UI::publish_boot() {
	std::string topic = FixedConfig::mqttTopic("/boot");

	network_.publish(topic + "/reset_reason/0", std::to_string(rtc_get_reset_reason(0)), true);
	network_.publish(topic + "/reset_reason/1", std::to_string(rtc_get_reset_reason(1)), true);
	network_.publish(topic + "/wakeup_cause", std::to_string(rtc_get_wakeup_cause()), true);

	network_.publish(topic + "/lights",
		Lights::rtc_boot_memory() + " -> " + boot_rtc_status_string(lights_.rtc_boot_status()), true);
	if (switches_) {
		network_.publish(topic + "/switches",
			Switches::rtc_boot_memory() + " -> " + boot_rtc_status_string(switches_->rtc_boot_status()), true);
	}
}

void UI::publish_partitions() {
	const esp_partition_t *current = esp_ota_get_running_partition();
	const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
	const esp_partition_t *boot = esp_ota_get_boot_partition();
	const esp_partition_t *part = current;

	if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
		part = esp_ota_get_next_update_partition(part);
	}

	for (int i = 0; i < esp_ota_get_app_partition_count(); i++, part = esp_ota_get_next_update_partition(part)) {
		esp_app_desc_t desc;
		esp_ota_img_states_t state;
		std::string topic = FixedConfig::mqttTopic("/partition/");

		if (esp_ota_get_state_partition(part, &state)) {
			state = ESP_OTA_IMG_UNDEFINED;
		}

		topic += std::to_string(i);

		std::string ota_payload = part->label;
		if (part == current) {
			ota_payload += " [current]";
		}
		if (part == next) {
			ota_payload += " [next]";
		}
		if (part == boot) {
			ota_payload += " [boot]";
		}
		ota_payload += ' ';
		ota_payload += ota_state_string(state);

		network_.publish(topic + "/ota", ota_payload);

		if (!esp_ota_get_partition_description(part, &desc)) {
			network_.publish(topic + "/name", null_terminated_string(desc.project_name));
			network_.publish(topic + "/version", null_terminated_string(desc.version));
			network_.publish(topic + "/idf_ver", null_terminated_string(desc.idf_ver));
			network_.publish(topic + "/timestamp", null_terminated_string(desc.date) + " " + null_terminated_string(desc.time));
		}
	}
}

void UI::publish_stats() {
	std::string topic = FixedConfig::mqttTopic("/stats");

	if (dali_) {
		DaliStats dali_stats = dali_->get_stats();
		std::string dali_topic = topic + "/dali";

		network_.publish(dali_topic + "/tx_count", std::to_string(dali_stats.tx_count));

		if (dali_stats.tx_count > 0) {
			network_.publish(dali_topic + "/min_tx_us", std::to_string(dali_stats.min_tx_us));
			network_.publish(dali_topic + "/max_tx_us", std::to_string(dali_stats.max_tx_us));
		}

		if (dali_stats.max_burst_tx_count > 0) {
			network_.publish(dali_topic + "/max_burst_tx_count", std::to_string(dali_stats.max_burst_tx_count));
			network_.publish(dali_topic + "/max_burst_us", std::to_string(dali_stats.max_burst_us));
		}
	}

	network_.publish(topic + "/heap/size", std::to_string(ESP.getHeapSize()));
	network_.publish(topic + "/heap/free", std::to_string(ESP.getFreeHeap()));
	network_.publish(topic + "/heap/min_free_size", std::to_string(ESP.getMinFreeHeap()));
	network_.publish(topic + "/heap/max_block_size", std::to_string(ESP.getMaxAllocHeap()));

	network_.publish(topic + "/psram/size", std::to_string(ESP.getPsramSize()));
	network_.publish(topic + "/psram/free", std::to_string(ESP.getFreePsram()));
	network_.publish(topic + "/psram/min_free_size", std::to_string(ESP.getMinFreePsram()));
	network_.publish(topic + "/psram/max_block_size", std::to_string(ESP.getMaxAllocPsram()));

	{
		std::lock_guard lock{file_mutex_};

		network_.publish(topic + "/flash/filesystem/size", std::to_string(FS.totalBytes()));
		network_.publish(topic + "/flash/filesystem/used", std::to_string(FS.usedBytes()));
	}

	network_.publish(topic + "/stack/min_size", std::to_string(uxTaskGetStackHighWaterMark(nullptr)));
	network_.publish(topic + "/max_queue_size", std::to_string(network_.maximum_queue_size()));
	network_.publish(topic + "/temperature_c", std::to_string(temperatureRead()));
	network_.publish(topic + "/uptime_us", std::to_string(esp_timer_get_time()));

	last_publish_us_ = esp_timer_get_time();
}

void UI::setup() {
	pinMode(LED_GPIO, OUTPUT);
	digitalWrite(LED_GPIO, LOW);

	if (x509_crt_bundle_end - x509_crt_bundle_start >= 2) {
		arduino_esp_crt_bundle_set(x509_crt_bundle_start);
	}
}

void UI::set_dali(Dali &dali) {
	dali_ = &dali;
}

void UI::set_switches(Switches &switches) {
	switches_ = &switches;
}

void UI::loop() {
	if (startup_complete_ && network_.connected()) {
		if (!last_publish_us_ || esp_timer_get_time() - last_publish_us_ >= FIVE_M) {
			publish_stats();
		}
	}
}

void UI::ota_update() {
	esp_http_client_config_t http_config{};
	esp_https_ota_config_t ota_config{};
	esp_https_ota_handle_t handle{};

	ESP_LOGE(TAG, "OTA update");

	http_config.crt_bundle_attach = arduino_esp_crt_bundle_attach;
	http_config.disable_auto_redirect = true;
	http_config.url = FixedConfig::otaURL();
	ota_config.http_config = &http_config;

	esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
	if (err) {
		network_.report(TAG, std::string{"OTA begin failed: "} + std::to_string(err));
		return;
	}

	const int size = esp_https_ota_get_image_size(handle);

	network_.report(TAG, std::string{"OTA size: "} + std::to_string(size));

	while (true) {
		err = esp_https_ota_perform(handle);

		if (err == ESP_OK) {
			err = esp_https_ota_finish(handle);
			if (err) {
				network_.report(TAG, std::string{"OTA finish failed: "} + std::to_string(err));
			} else {
				network_.report(TAG, std::string{"OTA finished"});
			}
			publish_partitions();
			return;
		} else if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
			network_.report(TAG, std::string{"OTA perform failed: "} + std::to_string(err));
			esp_https_ota_abort(handle);
			publish_partitions();
			return;
		}
	}
}

void UI::ota_good() {
	ota_result(true);
}

void UI::ota_bad() {
	ota_result(false);
}

void UI::ota_result(bool good) {
	esp_ota_img_states_t state;

	if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state)) {
		state = ESP_OTA_IMG_UNDEFINED;
	}

	if (state == ESP_OTA_IMG_PENDING_VERIFY) {
		if (good) {
			ESP_LOGE(TAG, "OTA good");
			esp_ota_mark_app_valid_cancel_rollback();
		} else {
			ESP_LOGE(TAG, "OTA bad");
			esp_ota_mark_app_invalid_rollback_and_reboot();
		}
	}
}
