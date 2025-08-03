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
The light switch is monitored using GPIO 17, which must be low when the switch is turned on.

The DALI interface is on GPIO 16 (RX) and 18 (TX).

The light switches are GPIO 11 and GPIO 12 (active low).

## Network interface
Copy `src/config.h.example` to `src/config.h` to configure the WiFi network,
MQTT hostname and MQTT topic (e.g. `dali`).

## MQTT interface

### Lights

Configure which lights are present by listing them as uppercase hexadecimal
values:
```
dali/addresses [<00-3F>...] (retain)
```

### Switches

Configure which light switches are present by listing the lights as uppercase
hexadecimal values and configuring the default preset:
```
dali/switch/<0-1>/name [name] (retain)
dali/switch/<0-1>/addresses [<00-3F>...] (retain)
dali/switch/<0-1>/preset <name> (retain)
```

Light switch status is reported as `dali/switch/<0-1>/state` when it changes
and then every 60 seconds.

### Presets

Up to 50 presets can be configured, setting an empty value to skip that light:

```
dali/preset/comfort/<0-63> <0-254>
dali/preset/comfort/<0-63> (null)
```

The reserved preset names `off`, `custom` and `unknown` can't be configured.

The maximum length of a preset name is 50 characters and they can only contain
lowercase alphanumeric characters as well as `.`, `-` and `_`.

Presets will be republished with light level values for all addresses in order:
```
dali/preset/<name>/levels <00-FF>... (retain)
```

The active presets are reported as `dali/preset/<name>/active` when they change
and then every 60 seconds. It's possible for multiple presets to be active as
long as one or more lights were last set using that preset.

Remove a preset:

```
dali/preset/comfort/delete (null)
```

Set individual lights:

```
dali/set/<0-63> <0-254>
```

Select preset:

```
dali/preset/comfort (null)
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
