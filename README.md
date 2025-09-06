# mqtt-dali-controller

## Overview
Control DALI lights using an ESP32-S3 over MQTT.

## Dependencies
[PlatformIO](https://platformio.org/)

## Build
`platformio run`

## Install
`platformio run -t upload`

## Hardware interface
The DALI interface is on GPIO 40 (RX) and 21 (TX). This is active high which is
the inverse of the DALI bus. The bus is idle (high) when the signal is low.

The light switches are GPIOs 11, 12, 13 and 14 (active low).

The selector option GPIOs are 16 and 17 (active low).

Rotary encoders for dimming are on the following GPIOs (active low):
* A0 and A1 (1 and 2)
* A2 and A3 (3 and 4)

* [PCB](https://github.com/edinburghhacklab/dali-pcb)

## Network interface
Copy `src/fixed_config.h.example` to `src/fixed_config.h` to configure the WiFi
network, MQTT hostname and MQTT topic (e.g. `dali`).

### References

* [Digitally Addressable Lighting Interface (DALI) Communication](https://ww1.microchip.com/downloads/en/AppNotes/01465A.pdf)
* [Digitally Addressable Lighting Interface (DALI) Unit Using the MC68HC908KX8](https://www.nxp.com/docs/en/reference-manual/DRM004.pdf)
* [Digital Addressable Lighting Interface (DALI) Implementation Using MSP430 Value Line Microcontrollers](https://www.ti.com/lit/an/slaa422a/slaa422a.pdf)
* [DALI Communication Using the EFR32](https://www.silabs.com/documents/public/application-notes/an1220-efr32-dali.pdf)

## MQTT interface

### Lights

Configure which lights are present by listing them as uppercase hexadecimal
values:
```
dali/addresses [<00-3F>...] (retain)
```

Light levels will be output when they change and every 60 seconds:
```
dali/levels <000-7FF>... (retain)
```
The level for all addresses are output, with a value of `FF` if the level hasn't
been set yet.

The lower 4 bits indicate the power status of the light:
* `1` = Present (in configured list of addresses)
* `2` = Power on
* `4` = Power off
* `8` = Dimmed as a member of a DALI group

The power bits will be absent until a switch has been configured for the lights.

### Groups

Up to 16 groups of lights can be configured and they can overlap. The built-in
group `all` includes all lights.

Configure which lights are present in groups by listing them as uppercase hexadecimal
values:
```
dali/group/<name> [<00-3F>...] (retain)
```

Remove a group by setting an empty value:
```
dali/group/<name> (null) (retain)
```

Sync group with the lights:
```
dali/group/<name> sync
```

Sync all groups with the lights:
```
dali/group/sync (null)
```

The reserved group names `all`, `delete`, `idle`, `levels` and `sync` can't be
configured.

The maximum length of a group name is 20 characters and they can only contain
lowercase alphanumeric characters as well as `.`, `-` and `_`. Group names must
start with a letter.

Group IDs are reported as comma-separated values in `dali/groups/ids` and are
internally managed.
```
dali/groups/ids kitchen,,table,,,door,,,,,,,,,,
```

### Switches

Configure which light switches are present by setting the group associated with
the switch and configuring the default preset:
```
dali/switch/<0-3>/name [name] (retain)
dali/switch/<0-3>/group <name> (retain)
dali/switch/<0-3>/preset <name> (retain)
```

Light switch status is reported when it changes and then every 60 seconds:
```
dali/switch/<0-3>/state <0-1> (retain)
```

Whenever the light switch is turned on or off, the default preset (if
configured) will be applied. This ensures that the lights function as expected
when they're turned on even if their light levels have previously been dimmed or
set to `off`.

### Buttons

Configure which buttons are present by setting the groups associated with the
button (optional, see [selector](#selector) below) and configuring the preset:
```
dali/button/<0-3>/groups <name>,... (retain)
dali/button/<0-3>/preset <name> (retain)
```

### Dimmers

Configure which light dimmers are present by setting the groups associated with
the dimmer (optional, see [selector](#selector) below) and configuring the
encoder/level steps:
```
dali/dimmer/<0-1>/groups <name>,... (retain)
dali/dimmer/<0-1>/encoder_steps <steps> (retain)
dali/dimmer/<0-1>/level_steps <0-254> (retain)
dali/dimmer/<0-1>/mode <individual|group> (retain)
```

Encoder steps can be configured in the range 1 to 127 (for forward movement) or
-1 to -127 (for backward movement). Setting the steps to 0 will disable the
dimmer.

Dimmers can operate on two modes: `individual` which allows each light to have a
different level, or on lights as one or more DALI `group`s. Using groups on the
DALI interface will be faster but all lights in the group must have the same
level so the level will be set to the average of all lights whenever dimming
starts. Multiple groups can be specified but if they overlap the dimmer will be
ignored; each group will be dimmed independently.

Synchronise the DALI groups before dimming otherwise unexpected behaviour will
happen, particularly if the previously stored groups overlap on the same dimmer
and that can no longer be identified.

### Selector

The "selector" lets you choose from one of 4 group options by switching 2 GPIOs.

Configure the groups associated with each selector option:
```
dali/selector/<0-3>/groups <name>,... (retain)
```

These groups are used by default if a button or dimmer has no groups configured.

### Presets

Up to 20 presets can be configured, by assigning light levels to each light,
setting an empty value to skip that light (retaining whatever its previous level
was):
```
dali/preset/<name>/<<0-63>[-<0-63>]|group>,... <0-254>
dali/preset/<name>/<<0-63>[-<0-63>]|group>,... (null)
```

The reserved preset names `off`, `custom`, `order` and `unknown` can't be
configured.

The maximum length of a preset name is 50 characters and they can only contain
lowercase alphanumeric characters as well as `.`, `-` and `_`. Preset names must
start with a letter.

Presets will be republished with light level values for all addresses in order:
```
dali/preset/<name>/levels <00-FF>... (retain)
```

The active presets are reported as on startup, when they change and every 60
seconds (cycling through one group at a time because there are a lot of topic
name combinations):
```
dali/active/<group>/<name> <0-1> (retain)
```
It's possible for multiple presets to be active as long as
one or more lights were last set using that preset.

Remove a preset:
```
dali/preset/<name>/delete (null)
```

A subset of presets can be configured to be accessed in numeric order:
```
dali/preset/order <name>,... (retain)
```

### Usage

Set the level of individual lights or groups:

```
dali/set/<<0-63>[-<0-63>]|group>,... <0-254>
```

Select a preset for all lights:

```
dali/preset/<name> (null)
dali/preset/<name> all
```

Select a preset for individual lights or groups:

```
dali/preset/<name> <<0-63>[-<0-63>]|group>,...
```

The built-in group `idle` changes the behaviour so that it only has an effect
if the light levels and switches haven't been changed for at least 10 seconds.
This can be used to avoid a race condition when deciding to automatically turn
off the lights at the same time someone is about to switch them on.

Select preset `off` for all lights, but only when idle for at least 10 seconds:
```
dali/preset/off all,idle
```

Select a numeric order preset:
```
dali/preset/<0-18446744073709551615> <<0-63>[-<0-63>]|group>
```

### Miscellaneous

Statistics are output every 5 minutes. Responses are `dali/stats/#`, look at
[`UI::publish_stats()`](src/ui.cpp) and [`class DaliStats`](src/dali.h) for more
information.

Reload config:
```
dali/reload (null)
```

Reboot:
```
dali/reboot (null)
```

Idle time (reported every 60 seconds after a change in light levels):
```
dali/idle_us <microseconds>
```

#### Broadcast command to all lights

Ensure the lights are set to the desired level before using these configuration
commands.

Set current level as the power on level:
```
dali/command/store/power_on_level (null)
```

Set current level as the system failure level (when DALI bus is disconnected):
```
dali/command/store/system_failure_level (null)
```

### OTA Updates

Query application, boot and firmware status and output stats:
```
dali/status (null)
```
Responses are `dali/g0/application/#`, `dali/g0/boot/#` and
`dali/g0/partition/#`.

Perform update:
```
dali/ota/update (null)
dali/reboot (null)
```

Verify update:
```
dali/ota/good (null)
dali/ota/bad (null)
```
