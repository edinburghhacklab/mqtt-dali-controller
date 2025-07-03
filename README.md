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

## Network interface
Copy `src/config.h.example` to `src/config.h` to configure the WiFi network,
MQTT hostname and MQTT topic (e.g. `dali/g14`).

## MQTT interface

Up to 50 presets can be configured. If more are configured then the oldest
preset is discarded. Use the reboot command to recover from this after removing
unwanted presets.

```
dali/g14/preset/comfort/0 <0-255> (retain)
dali/g14/preset/comfort/1 <0-255> (retain)
```

Set individual lights:

```
dali/g14/set/0 <0-255>
dali/g14/set/1 <0-255>
```

Select preset:

```
dali/g14/preset/comfort (null)
```

Reboot:

```
dali/g14/reboot (null)
```
