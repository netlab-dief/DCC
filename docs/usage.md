# DCC usage guide

[Overview](../README.md) · [CAM](../cam/README.md) · [DCC_NET](dcc-net.md) ·
[Architecture](architecture.md)

DCC controls application-level access to one ITS-G5 channel. It measures CBR,
updates a reactive or adaptive controller, requests radio settings and exposes a
internal per-packet airtime gate. Integrated CAM generation is optional.

## Contents

- [Installation and build](#installation-and-build)
- [Radio preparation](#radio-preparation)
- [Command-line reference](#command-line-reference)
- [Running DCC](#running-dcc)
- [Controllers](#controllers)
- [CBR acquisition and validity](#cbr-acquisition-and-validity)
- [Telemetry](#telemetry)
- [Terminal dashboard](#terminal-dashboard)
- [Internal CAM admission](#internal-cam-admission)
- [Synthetic CBR](#synthetic-cbr)
- [Troubleshooting](#troubleshooting)
- [Tests and hardware validation](#tests-and-hardware-validation)

## Installation and build

The full program runs on Linux and requires a C++17 compiler, GNU Make,
pkg-config, OpenSSL and libnl development headers/libraries. Install `iw` for
radio inspection and Python 3 for the helper tools and Python tests.

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install build-essential pkg-config libssl-dev libnl-3-dev libnl-genl-3-dev iw python3
make
```

Run commands from the repository root unless stated otherwise. The build creates
`./dcc`, stores intermediate files and test binaries in `build/`, and compiles the bundled ASN.1 C sources in `cam/UtilsEncode/`. A sibling
`ETSI_stack` checkout is unnecessary.

```bash
# Clean rebuild
make clean
make
```

Hardware-independent tests can also run on macOS. Building the full application,
reading nl80211 surveys and using raw monitor sockets require Linux.

## Radio preparation

Inspect available interfaces and PHY assignments:

```bash
ip link show
iw dev
```

The program always takes two positional arguments:

| Argument | Operations |
| --- | --- |
| First: network interface, e.g. `wlan0` | Active-channel survey counters and TX power configuration |
| Second: radiotap interface, e.g. `wlan1` | Legacy bitrate configuration, integrated CAM injection and optional SHB reception |

The channel measured on the first interface must represent the transmitter
controlled through the second. Two unrelated radios do not establish this
relationship. With CAM or DCC_NET, configure the second interface as a monitor
with radiotap support before launch. DCC does not select a channel, create monitor
mode or configure channel width. Driver support is required for survey counters,
fixed power, legacy bitrate requests and any enabled injection/reception.

DCC_NET requires both interfaces on the same PHY, a fixed ITS-G5 channel and
TX-power readback. It validates the PHY relationship through sysfs. Do not change
channel, PHY, MAC address or power from another process while NET is running.
See [DCC_NET radio requirements](dcc-net.md#radio-and-power-requirements).

Radio operations normally require administrator privileges. A typical command is:

```bash
sudo ./dcc wlan0 wlan1
```

Interface names in this guide are examples; use the names reported by your host.

## Command-line reference

```text
./dcc <network_interface> <radiotap_interface> [options]
```

Reactive control with CAM and DCC_NET disabled is the default.

| Option | Default | Meaning and constraints |
| --- | --- | --- |
| `--limeric` | Off | Select the adaptive delta controller |
| `--fake-cbr` | Off | Enable `fake_cbr_value` input in reactive mode; incompatible with `--limeric` |
| `--dcc-net` | Off | Receive neighbour SHB load, control from global CBR and add DCC-MCO to integrated CAMs |
| `--dcc-net-eirp-offset-db <dB>` | `0` | Antenna gain minus cable loss; finite value from −31 to +31 dB, including fractions; requires `--dcc-net` |
| `--cam-static` | Off | Enable CAMs from the bundled stationary sample position |
| `--cam-gps <device>` | Off | Enable CAMs from a 9600-baud GPS serial device using GPRMC/GNRMC |
| `--cam-station-id <id>` | `4316` | Unsigned 32-bit CAM station ID, 0–4294967295 |
| `--cam-priority <0-7>` | `3` | QoS TID / user priority for injected CAM frames |
| `--cam-force-100ms` | Off | Force nominal 100 ms generation; requires a CAM source |

Choose at most one CAM source. Repeated source selections are rejected, including
repeating the same source option. Station ID and priority only affect an enabled
CAM sender. Unknown options and missing or invalid option values are rejected.
Both interface arguments remain mandatory in synthetic-CBR mode.

## Running DCC

### Reactive and adaptive control

```bash
sudo ./dcc wlan0 wlan1
sudo ./dcc wlan0 wlan1 --limeric
```

Both modes configure an internal gate for the integrated CAM sender. Select
`--cam-static` or `--cam-gps <device>` to enable generation.

### Integrated CAM generation

```bash
# Stationary sample data, normal generation at 1 Hz
sudo ./dcc wlan0 wlan1 --cam-static

# GPS data with adaptive control
sudo ./dcc wlan0 wlan1 --limeric --cam-gps /dev/ttyACM0

# Nominal 10 Hz generation, including while stationary
sudo ./dcc wlan0 wlan1 --cam-static --cam-force-100ms

# GPS, forced generation and explicit default priority
sudo ./dcc wlan0 wlan1 --cam-gps /dev/ttyACM0 --cam-force-100ms --cam-priority 3
```

Normal generation checks conditions every nominal 10 ms. The first CAM is
produced when source data become valid. A stationary source then generates at
1 Hz. Changes exceeding 4 m, 4° or 0.5 m/s relative to the last successful driver
enqueue can trigger generation, subject to a DCC lower bound of 100–1000 ms.
After a motion trigger, the elapsed generation interval is retained for three
timed follow-ups before returning to 1000 ms.

Forced mode selects nominal 100 ms generation independently of those triggers
and the DCC generation lower bound. It retains source checks, expiry and gate
admission. Generation frequency does not guarantee driver enqueue or delivery
frequency. A single pending slot retains the latest CAM; there is no catch-up
burst. The low-frequency container is included initially and at the next
generation after its 500 ms deadline.

The default frame uses GeoNetworking TC2 and QoS TID 3 (AC_BE). `--cam-priority`
changes the QoS priority while retaining TC2. See the [CAM guide](../cam/README.md)
for source validation, containers, EDCA mapping and estimated airtime.

### Optional DCC_NET

```bash
sudo ./dcc wlan0 wlan1 --dcc-net --cam-static
sudo ./dcc wlan0 wlan1 --dcc-net --limeric --cam-gps /dev/ttyACM0
sudo ./dcc wlan0 wlan1 --dcc-net
```

The last command receives neighbour load without advertising local load.
Advertising requires integrated CAMs; NET does not generate extra beacons.
With the flag enabled, controllers consume global CBR and the main monitor
continues to show local CBR. See [DCC_NET](dcc-net.md) for aggregation and limits.

### Console and shutdown

The main monitor prints once per second: local CBR in percent (`-1` when invalid
or stale), reactive state or `DYNAMIC (LIMERIC)`, and `T_off` in milliseconds.
`T_off` is a state minimum in reactive mode and an advisory interval in adaptive
mode. Reactive transitions and significant adaptive log updates produce
additional messages. With CAM enabled, `[CAM MONITOR]` reports lifecycle counters,
driver enqueue frequency, timing and estimated accounting.

Some runtime messages remain in Italian. For example, `Invii al driver` means
driver enqueues, and `Contabilizzati (stima)` means accounted using an estimate.
The documentation's English descriptions do not change these console labels.

The controller attempts `SCHED_FIFO` scheduling at priority 70. Failure is logged;
the worker can continue with ordinary scheduling. This thread priority is
separate from packet EDCA priority and does not guarantee hardware timing.

Press Ctrl+C or send SIGTERM to stop. Shutdown joins workers, records a final CAM
sample when enabled and closes resources. Integrated transmission does not wait
for external PHY feedback.

## Controllers

### Reactive mode

The first valid CBR initializes the state using the ranges below. Later changes
move one adjacent state at a time: CBR at or above the current state's upper
threshold must persist for 1 second to move upward; CBR below its lower threshold
must persist for 5 seconds to move downward. Invalid input or discontinuity resets
that persistence.

| State | Initial CBR range | Requested power | Requested legacy bitrate | Gate minimum | Normal CAM generation lower bound |
| --- | --- | --- | --- | --- | --- |
| `RELAXED` | <30% | 23 dBm | 6 Mb/s | 50 ms | 100 ms |
| `ACTIVE_1` | 30–<40% | 19 dBm | 12 Mb/s | 100 ms | 100 ms |
| `ACTIVE_2` | 40–<50% | 16 dBm | 18 Mb/s | 200 ms | 200 ms |
| `ACTIVE_3` | 50–<65% | 13 dBm | 24 Mb/s | 400 ms | 400 ms |
| `RESTRICTIVE` | ≥65% | 10 dBm | 24 Mb/s | 1000 ms | 1000 ms |

These are project settings. Gate minima and generation lower bounds are separate:
a stationary source still normally generates at 1 Hz. The reactive gate also
uses delta `0.03`; airtime debt can delay admission beyond the state minimum.
Radio values are requests whose application depends on the driver.

### Adaptive mode

The historical `--limeric` flag selects `DccAdaptive`. It processes consecutive
100 ms samples in pairs ending at UTC-aligned 200 ms boundaries. Internal CBR and
delta values are fractions, not percentages.

```text
pair_mean = (previous_cbr + current_cbr) / 2
filtered  = 0.5 × previous_filtered + 0.5 × pair_mean
offset    = clamp(0.0012 × (0.68 − filtered), −0.00025, 0.0005)
delta     = clamp((1 − 0.016) × previous_delta + offset, 0.0006, 0.03)
```

Initial filtered CBR is zero; initial delta is `0.0006`. Invalid input or a gap
breaks pair continuity without resetting the accumulated filtered value or delta.
The gate applies a 25 ms minimum and the current delta budget. Its advisory
interval may exceed 1000 ms; normal CAM generation clamps its input to
100–1000 ms while radio admission retains the full airtime debt.

Power and legacy bitrate selection are separate project policies. They use the
advisory rate and controller CBR; enabling NET changes that CBR input to global
CBR without changing the adaptive parameters. The implementation is based on
[ETSI TS 102 687 V1.2.1, §5.4](https://www.etsi.org/deliver/etsi_ts/102600_102699/102687/01.02.01_60/ts_102687v010201p.pdf).

## CBR acquisition and validity

A dedicated `CbrSampler` thread reads the active-channel survey through
`NL80211_CMD_GET_SURVEY` and calculates `delta_busy / delta_active`. The first
reading establishes a baseline. Only this thread accesses the radio reader or
synthetic input file. Each observation has a fractional value, monotonic
acquisition timestamp, UTC 100 ms slot, sequence number and validity flag.

Reads use absolute nominal UTC-aligned 100 ms deadlines, so processing time is
not added to each period. The synchronized queue holds 16 observations. A late
read, invalid value, missing window or source change interrupts continuity;
recovery requires a new baseline and a complete window. An old observation is
never reused as a new measurement.

Controllers reject samples older than 150 ms. With no fresh sample, they disable
admission and reset continuity. Reactive persistence uses acquisition timestamps.
Budget validity is measured from acquisition, so delayed processing does not
refresh stale data. The gate independently rejects budgets older than 500 ms,
including when control stalls. Netlink I/O has 80 ms timeouts per wait; a compound
operation may take longer and can invalidate its window. Actual timing depends
on Linux scheduling and the driver.

The monitor reads the sampler directly. Under NET, the separate receiver worker
combines local samples with neighbour observations; missing local input cannot
be replaced by neighbour data. Shutdown wakes timer and sample waiters.

## Telemetry

All relative log paths use the process working directory. Controller records are
flushed after each write, including when the state remains stable. Power and
bitrate fields describe requests; they do not certify hardware application.

### Controller telemetry

`dcc_telemetry_log.csv` is appended across runs.

| Column | Meaning |
| --- | --- |
| `Timestamp` | Local date/time in ISO-style format with microseconds, without a timezone offset |
| `CBR_Percent` | Controller input in percent: local normally, global with NET |
| `State_Mode` | Reactive state or `LIMERIC` |
| `Interval_ms` | Reactive gate minimum or adaptive advisory interval |
| `Tx_Power_dBm` | Requested power in dBm |
| `MCS_Netlink` | Requested legacy bitrate in Mb/s; the historical column name is retained |
| `HW_Config_Changed` | `YES` on a reactive state change or significant adaptive log update; not hardware confirmation |

Move or rename this file between runs when separate logs are needed. It does not
contain CAM transmission counts.

### CAM telemetry

With a CAM source enabled, `cam_telemetry_estimated_log.csv` is appended. Counters
restart for each `Session_Unix_ms`. Samples are written every nominal second,
with a final sample at shutdown. The older `cam_telemetry_log.csv` is untouched,
so estimated and historical measured-schema records remain separate.

| Column | Meaning |
| --- | --- |
| `Session_Unix_ms`, `Timestamp_Unix_ms` | Session start and sample time in Unix UTC milliseconds |
| `Elapsed_s` | Monotonic seconds since log opening |
| `Generated` | Successfully constructed CAMs published to the pending slot |
| `Replaced` | Pending valid CAMs replaced by newer ones |
| `Expired` | CAMs dropped at expiry or with less than 50 ms GN lifetime remaining |
| `Invalidated` | CAMs removed because source data became invalid |
| `Construction_Overruns` | Constructions discarded after taking at least 50 ms |
| `Submitted` | Frames accepted by `sendto()`; driver enqueue, not PHY confirmation |
| `Estimated_Accounted` | Enqueues whose estimated-airtime accounting was accepted by the gate |
| `TX_Errors` | Failed admission, send or accounting operations; shutdown while waiting is excluded |
| `Submitted_Hz` | Increase in `Submitted` divided by actual monotonic time between samples |
| `Queue_Mean_ms` | Cumulative mean from generation to send start, over submitted frames |
| `Gate_Mean_ms` | Cumulative mean from selection of the same pending version to send start, over submitted frames |
| `Send_Mean_ms` | Mean send-callback duration, over accounted submissions |
| `Estimated_Airtime_us` | Cumulative estimated airtime accepted by the gate, in microseconds |

Means with no observations are zero. Counters and means are cumulative within a
session; frequency covers the latest sampling interval. `Submitted` may increase
before `Estimated_Accounted`, or without it if accounting fails. No field measures
driver queue time separately, PHY completion or reception by another station.
Log failures are reported; final logging must not prevent worker cleanup.

### DCC_NET telemetry

NET creates `dcc_net_telemetry_log.csv` and truncates it at each NET startup.
It records local, one-hop, two-hop and global CBR, validity and neighbour count.
See the [NET column reference](dcc-net.md#telemetry). A previous NET file remains
on disk when a later run disables NET; its presence does not imply NET is active.

## Terminal dashboard

Open a second terminal on the same host:

```bash
python3 tools/dashboard.py
python3 tools/dashboard.py --log-dir /path/to/logs
```

The read-only dashboard uses Python's standard `curses` module, with no pip
packages or administrator privileges. Use at least 72 columns and 24 rows.
It refreshes four times per second and shows controller CBR, state/mode,
advisory interval, requested power/bitrate and sample history. CAM rows show
counters, driver enqueue frequency, mean waits and estimated airtime. NET rows
show the separate load-exchange log when available.

The graph holds the latest samples fitting the terminal width and displays its
time range; it is not a fixed-duration chart. Its main CBR series follows the
controller input, including global CBR when NET is enabled.

Missing files are retried automatically, and replacement/truncation is followed.
DCC and NET samples older than one second and CAM samples older than three
seconds are marked stale; historical values remain visible. This indicates log
freshness, rather than gate or radio health. CAM counters belong to the latest
logged session. Run on the same host because DCC timestamps use local time.
Press `q`, Escape or Ctrl+C to close only the dashboard.

## Internal CAM admission

The integrated generator and transmitter run inside DCC. The sender calls
`AirtimeGate` directly; there is no external client, UDP gate server or advisory
notification socket.

Each selected CAM reserves a packet with `ADMIT`. The gate grants admission only
when CBR is valid and the current airtime debt has elapsed. After a complete
successful driver enqueue, `ACCOUNT` charges the packet's estimated airtime and
closes the reservation. This accounting does not claim PHY completion or delivery.

```text
next admission delay = max(mode minimum, charged airtime / delta)
```

The delay starts at accounting time. Charged airtime is at least the original
reservation; a smaller value cannot refund it. The gate retains the most
restrictive delta seen during the reservation, and controller updates preserve
outstanding debt. Adaptive mode uses a 25 ms minimum; reactive mode uses its
state minimum with delta `0.03`. Large packets can delay admission beyond one
second; the debt is not capped.

Only one reservation may be open. Failed or partial sends stop the sender and
retain that uncertain reservation. It does not expire automatically. Stop and
drain the transmitter and driver queues before restarting after an uncertain
send. Invalid or stale CBR prevents new admission.

This conservative pacing policy is based on
[ETSI TS 102 687 V1.2.1](https://www.etsi.org/deliver/etsi_ts/102600_102699/102687/01.02.01_60/ts_102687v010201p.pdf)
and differs from an exact reproduction of its informative Annex B gate. See the
[CAM airtime model](../cam/README.md#estimated-airtime) for the estimate and its
radio assumptions.

## Synthetic CBR

Synthetic input is enabled only with `--fake-cbr` in reactive mode. Without this
flag, DCC ignores `fake_cbr_value`. Values are fractions in `[0, 1]`:

```bash
printf '0.45\n' > fake_cbr_value
sudo ./dcc wlan0 wlan1 --fake-cbr
```

From another terminal, change the value or remove the file to return to survey
readings:

```bash
printf '0.70\n' > fake_cbr_value
rm fake_cbr_value
```

A missing/unreadable file or an unparseable number falls back to the radio.
A successfully parsed out-of-range value is an invalid observation. Source
changes interrupt continuity and re-prime radio measurements.

For a generated trace:

```bash
python3 tools/cbr_generator.py --output fake_cbr_value
```

Start DCC from the same directory in another terminal. The generator defaults
to output `cbr`, logs its trace to `cbr_trace_log.csv` and writes every 150 ms
by default; DCC still samples on nominal 100 ms ticks. Synthetic CBR replaces
only measurement input. Power/bitrate requests and enabled CAM transmissions
continue to use real interfaces. `--fake-cbr` is incompatible with `--limeric`.

### Generator options

The generator runs four phases: stable low load, a ramp toward the threshold,
oscillation around it, and a final upward step. Output values are clamped to
`[0, 1]` and written atomically. Its trace CSV is recreated at startup and has
columns `timestamp_iso`, `t_rel_s`, `phase` and `cbr_value`.

| Option | Default | Meaning |
| --- | --- | --- |
| `--output` | `cbr` | Current-value file; set to `fake_cbr_value` for DCC |
| `--log` | `cbr_trace_log.csv` | Trace CSV path |
| `--interval` | `0.15` | Update interval in seconds |
| `--threshold` | `0.4` | Fractional CBR around which the trace is built |
| `--amplitude` | `0.03` | Oscillation amplitude |
| `--osc-period` | `4.0` | Oscillation period in seconds |
| `--noise-std` | `0.0` | Gaussian noise standard deviation; zero disables noise |
| `--low-offset` | `0.25` | Stable level below threshold |
| `--step-offset` | `0.25` | Final step above threshold |
| `--stable-duration` | `30.0` | Stable phase duration in seconds |
| `--ramp-duration` | `30.0` | Ramp duration in seconds |
| `--osc-duration` | `180.0` | Oscillation duration in seconds |
| `--step-duration` | `30.0` | Final step duration in seconds |
| `--seed` | Unset | Random seed for repeatable noise sequences |

## Troubleshooting

| Symptom | Checks and action |
| --- | --- |
| Missing-argument error (`Errore: Parametri mancanti!`) | Supply both interface names before the options |
| Interface not found | Inspect `ip link show` and `iw dev`; bring the intended interface up if needed, e.g. `sudo ip link set wlan0 up` |
| Netlink operation denied | Run with the required privileges; check driver support for fixed power and legacy bitrate on the configured band |
| Missing build libraries | Check `pkg-config --cflags --libs libnl-3.0 libnl-genl-3.0` and install the development packages above |
| Synthetic CBR ignored | Use `--fake-cbr`, the exact filename `fake_cbr_value`, the DCC working directory and a fraction such as `0.35`; disable `--limeric` |
| Generated CAMs are not enqueued | Check fresh CBR, gate budget, monitor injection, interface state, channel correspondence and driver support for the selected legacy rate |
| No GPS CAMs | Check device access, 9600 baud, checksummed GPRMC/GNRMC with fix `A`, fresh increasing fix times and synchronized host UTC; stationary normal generation is 1 Hz |
| DCC_NET startup or radio error | Verify same PHY/channel, a radiotap monitor, raw-socket privileges and TX-power readback with `iw dev <network_interface> info` |
| No usable neighbour data | Check unprotected GN SHB traffic and parser support; secured GN requires decapsulation not included here |
| Dashboard waits or shows stale data | Check `--log-dir`, enabled features and current CSV writes; old files can remain from earlier sessions |

`--cam-force-100ms` changes generation only and cannot force open a closed gate.
For NET-specific encoding and interoperability limitations, see [DCC_NET](dcc-net.md).

## Tests and hardware validation

```bash
make test
# Run either group separately:
make test-cpp
make test-python
```

C++ suites cover reactive transitions, adaptive control, airtime accounting,
CBR acquisition, CAM encoding/timing and DCC_NET. Python suites cover the
synthetic generator, dashboard and CLI validation.
CLI tests require a built application binary and skip if it is absent. No radio
is required.

Validate injection, survey counters, EDCA queue selection, requested bitrate,
power readback and estimated airtime using the target Linux driver and radio.
NET field validation should use multiple nodes on one channel and capture SHB
fields alongside logs. Passing software tests does not certify the ITS stack or
hardware. GitHub Actions builds the application and runs the tests on Ubuntu.
