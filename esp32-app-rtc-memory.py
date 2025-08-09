#!/usr/bin/env python3
# esp32-app-rtc-memory - Output location and size of RTC noinit memory
# Copyright 2025  Simon Arlott

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# PlatformIO usage:
#
# [env:...]
# extra_scripts = post:esp32-app-rtc-memory.py

import argparse
import collections
import re
import subprocess
import sys

RE_ELF_SECTION = re.compile(r"^\s*\[[0-9 ]+\]\s*(?P<name>\S+)\s+(?P<type>\w+)\s+(?P<addr>\w+)\s+(?P<offset>\w+)\s+(?P<size>\w+)\s+")
Symbol = collections.namedtuple("Symbol", ["value", "size", "line"])
RE_ELF_SYMBOL = re.compile(r"^(?P<before_value>\s*(?P<num>[0-9]+):\s+)(?P<value>\w+)(?P<after_value>\s+(?P<size>\w+)\s+(?P<type>\w+)\s+(?P<bind>\w+)\s+(?P<visibility>\w+)\s+(?P<ndx>\w+)\s+(?P<name>\w+))")

def print_rtc_memory(fw_elf):
	rtc_offset = None
	rtc_size = None
	width = 8

	lines = subprocess.run(["readelf", "-W", "--section-headers", fw_elf],
			check=True, universal_newlines=True, stdout=subprocess.PIPE
		).stdout.strip().split("\n")

	for line in lines:
		match = RE_ELF_SECTION.match(line)
		if match:
			if rtc_offset is None and match["name"] == ".rtc_noinit":
				rtc_offset = int(match["addr"], 16)
				rtc_size = int(match["size"], 16)

	lines = subprocess.run(["readelf", "-W", "--syms", "--dyn-syms", fw_elf],
			check=True, universal_newlines=True, stdout=subprocess.PIPE
		).stdout.strip().split("\n")
	syms = set()

	for line in lines:
		match = RE_ELF_SYMBOL.match(line)
		if match:
			sym_offset = int(match["value"], 16)
			sym_size = int(match["size"])
			width = len(match['value'])

			if (rtc_offset is not None
					and sym_offset >= rtc_offset
					and sym_offset <= rtc_offset + rtc_size
					and sym_offset + sym_size <= rtc_offset + rtc_size):
				syms.add(Symbol(sym_offset, sym_size, line))

	if syms:
		syms = list(syms)
		syms.sort()

		value = syms[0].value
		for sym in syms:
			if sym.value > value:
				print("\t{1:0{0}x} {2:5d} UNKNOWN".format(8, value, sym.value - value))
			print(sym.line)
			value = sym.value + sym.size

	print()
	print(f"Total RTC noinit size: {rtc_size} bytes")

def after_fw_elf(source, target, env):
	fw_elf = str(target[0])
	print_rtc_memory(fw_elf)

if __name__ == "__main__":
	parser = argparse.ArgumentParser(description="Output location and size of RTC noinit memory")
	parser.add_argument("fw_elf", metavar="ELF", type=str, help="Firmware ELF filename")

	args = parser.parse_args()
	print_rtc_memory(**vars(args))
elif __name__ == "SCons.Script":
	Import("env")

	env.AddPostAction("${BUILD_DIR}/${PROGNAME}.elf", after_fw_elf)
