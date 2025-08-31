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

#include "util.h"

#include <cerrno>
#include <cstdlib>
#include <string>

bool long_from_string(const std::string &text, long &value) {
	if (text.empty()) {
		return false;
	}

	char *endptr = nullptr;

	errno = 0;
	value = std::strtol(text.c_str() + (text[0] == '+' ? 1 : 0), &endptr, 10);
	if (!endptr || endptr[0] || errno) {
		return false;
	}

	return true;
}

bool ulong_from_string(const std::string &text, unsigned long &value) {
	if (text.empty()) {
		return false;
	}

	char *endptr = nullptr;

	errno = 0;
	value = std::strtoul(text.c_str() + (text[0] == '+' ? 1 : 0), &endptr, 10);
	if (!endptr || endptr[0] || errno) {
		return false;
	}

	return true;
}

bool ulonglong_from_string(const std::string &text, unsigned long long &value) {
	if (text.empty()) {
		return false;
	}

	char *endptr = nullptr;

	errno = 0;
	value = std::strtoull(text.c_str() + (text[0] == '+' ? 1 : 0), &endptr, 10);
	if (!endptr || endptr[0] || errno) {
		return false;
	}

	return true;
}
