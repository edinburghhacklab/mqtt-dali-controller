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

Configure which lights are present by listing them as uppercase hexadecimal
values:
```
dali/addresses [<00-3F>...]
```

Configure which light switches are present by listing the lights as uppercase
hexadecimal values and configuring the default preset:
```
dali/switch/<0-1>/addresses [<00-3F>...]
dali/switch/<0-1>/preset <name>
```
(If there are two switches configured, switch 0 is the "left" switch and
switch 1 is the "right" switch.)

Light switch status is reported as `dali/switch/<0-1>/state` when it changes
and then every 60 seconds.

Up to 50 presets can be configured:

```
dali/preset/comfort/<0-63> <0-254> (retain)
```

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

Reload config:

```
dali/reload (null)
```

Reboot:

```
dali/reboot (null)
```
