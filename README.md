| Supported Targets | ESP32-C3 |
| ----------------- | -------- |

# Pilot Light Monitor

The pilot light monitor is intended to use an IR phototransistor to detect heat from the flame of the pilot light and take action if/when it goes out. This coupled with a watchdog situation hosted on a VPS elsewhere makes a well rounded setup so I know what kind of failure I am looking at.

1) The pilot light went out (because I live in a windy area). This should be
   detected within a few minutes, whenever the next time the monitor wakes up
   and reported directly as a text.
2) The pilot light monitor went out to lunch and failed to check in.
   Periodically, the monitor must wake up and check in even if there is no
   failure, so we know it is still alive. If it ever fails to check in for a
   given time, the watchdog on the VPS will send a text.
3) Another system on the same network as the monitor must also check in (at a
   higher frequency) so any network failure will be detected before we assume
   the monitor is out to lunch. This is to help us distinguish between offline
   and dead.

## How to use example

### Hardware Required

A K-type thermocouple must be connected to ADC1-channel-2.

A high-impedance voltage divider must be connected between batt+ and ground
with the mid-point connected to ADC1-channel-3.

It is assumed that a red LED and resistor are connected to GPIO-6.
It is assumed that a green LED and resistor are connected to GPIO-7.

A 3.7V 4300mAH lithium battery was used to power the project connected to batt+
and batt-.

### Configure the project

```
idf.py menuconfig
```

* Make sure all the settings under 'Pilot Light Monior' are set.

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

