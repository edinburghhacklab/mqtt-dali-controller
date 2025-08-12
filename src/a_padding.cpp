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

/*
 * This file is sorted first so it will be linked first
 *
 * Add some padding at the start of the RTC noinit memory, because the uploader
 * overwrites memory in this area
 */

RTC_NOINIT_ATTR uint64_t rtc_padding;

static void __attribute__((constructor(1000))) rtc_padding_init() {
	rtc_padding = 0;
}
