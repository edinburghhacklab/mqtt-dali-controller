# mqtt-dali-controller

## Overview
Control DALI lights using an ESP32-S2 over MQTT.

## Dependencies
[PlatformIO](https://platformio.org/)

## Build
`platformio run`

## Install
`platformio run -t upload`

## Hardware interface
The DALI interface is on GPIO 16 (RX) and 18 (TX).

The light switches are GPIO 11 and GPIO 12 (active low).

## Network interface
Copy `src/fixed_config.h.example` to `src/fixed_config.h` to configure the WiFi
network, MQTT hostname and MQTT topic (e.g. `dali`).

## MQTT interface

### Lights

Configure which lights are present by listing them as uppercase hexadecimal
values:
```
dali/addresses [<00-3F>...] (retain)
```

Light levels will be output when they change and every 60 seconds:
```
dali/levels <000-7FE>... (retain)
```
The level for all addresses are output, with a value of `FF` if the level hasn't
been set yet.

The lower 4 bits indicate the power status of the light:
* `1` = Present (in configured list of addresses)
* `2` = Power on
* `4` = Power off

The power bits will be absent until a switch has been configured for the lights.

### Groups

Up to 10 groups of lights can be configured and they can overlap. The built-in
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

The reserved group names `all`, `delete` and `levels` can't be configured.

The maximum length of a group name is 50 characters and they can only contain
lowercase alphanumeric characters as well as `.`, `-` and `_`. Group names must
not start with a number.

### Switches

Configure which light switches are present by setting the group associated with
the switch and configuring the default preset:
```
dali/switch/<0-1>/name [name] (retain)
dali/switch/<0-1>/group <name> (retain)
dali/switch/<0-1>/preset <name> (retain)
```

Light switch status is reported as `dali/switch/<0-1>/state` when it changes
and then every 60 seconds.

### Presets

Up to 10 presets can be configured, setting an empty value to skip that light:

```
dali/preset/<name>/<<0-63>[-<0-63>]|group>,... <0-254>
dali/preset/<name>/<<0-63>[-<0-63>]|group>,... (null)
```

The reserved preset names `off`, `custom` and `unknown` can't be configured.

The maximum length of a preset name is 50 characters and they can only contain
lowercase alphanumeric characters as well as `.`, `-` and `_`.

Presets will be republished with light level values for all addresses in order:
```
dali/preset/<name>/levels <00-FF>... (retain)
```

The active presets are reported as `dali/active/<group>/<name>` on startup, when
they change and every 60 seconds (cycling through one group at a time because
there are a lot of topic name combinations). It's possible for multiple presets
to be active as long as one or more lights were last set using that preset.

Remove a preset:

```
dali/preset/<name>/delete (null)
```

### Usage

Set individual lights or groups:

```
dali/set/<<0-63>[-<0-63>]|group>,... <0-254>
```

Select preset for all lights:

```
dali/preset/<name> (null)
dali/preset/<name> all
```

Select preset for individual lights or groups:

```
dali/preset/<name> <<0-63>[-<0-63>]|group>
```

### Miscellaneous

Reload config:

```
dali/reload (null)
```

Reboot:

```
dali/reboot (null)
```

### OTA Updates

Query status:

```
dali/status (null)
```

Responses are `dali/g0/application/#` and `dali/g0/partition/#`.

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
