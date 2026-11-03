# DCC — Distributed Congestion Control

C++17 congestion control for Linux ITS-G5 experiments, with an integrated C++ CAM
generator. DCC reads Channel Busy Ratio (CBR), requests radio power and bitrate
changes, and controls packet admission through a shared airtime gate.

- [Guida d'uso in italiano](GUIDA_USO.md): setup, options, telemetry and gate protocol.
- [CAM guide](cam/README.md): generation, forced 100 ms mode, EDCA and estimated airtime.
- [Architecture](CODEBASE_ANALYSIS.md): modules, threads and failure handling.

## Requirements and build

The full application requires Linux, GCC/G++ with C++17, GNU Make, pkg-config,
OpenSSL and libnl development libraries. Python 3 is required for Python tests
and the synthetic CBR generator.

On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential pkg-config libssl-dev libnl-3-dev libnl-genl-3-dev iw python3
make
```

The executable is `./dcc`. To rebuild from scratch:

```bash
make clean
make
```

Some hardware-independent tests run on macOS; full application compilation and
radio transmission require Linux.

## Interfaces

```bash
iw dev
sudo ./dcc <network_interface> <radiotap_interface> [options]
```

| Argument | Used for |
| --- | --- |
| First interface, e.g. `wlan0` | CBR survey readings and TX power configuration |
| Second interface, e.g. `wlan1` | Bitrate configuration and all integrated CAM transmissions |

With CAM enabled, the second interface must already support monitor/radiotap
injection. The application does not create or configure monitor mode or the
channel. The measured channel and controlled radio must correspond to the CAM
transmitter; passing two unrelated radios does not establish that relationship.
Radio operations normally require administrator privileges and driver support.

## Examples

DCC only, reactive mode:

```bash
sudo ./dcc wlan0 wlan1
```

Adaptive mode, selected by the historical `--limeric` option:

```bash
sudo ./dcc wlan0 wlan1 --limeric
```

Integrated CAM sender, using static sample data or a GPS serial device:

```bash
sudo ./dcc wlan0 wlan1 --cam-static
sudo ./dcc wlan0 wlan1 --limeric --cam-gps /dev/ttyACM0
```

The DCC process generates CAMs and injects them directly through the second
interface. **No external driver bridge is required.** Airtime is estimated from
the selected packet's complete MAC length and the current DCC legacy bitrate,
including FCS and OFDM overhead. The same rate is inserted into radiotap RATE.
Successful `sendto()` means driver enqueue, not measured PHY completion.

Force CAM generation every 100 ms, including when stationary:

```bash
sudo ./dcc wlan0 wlan1 --cam-static --cam-force-100ms
sudo ./dcc wlan0 wlan1 --limeric --cam-gps /dev/ttyACM0 --cam-force-100ms --cam-priority 3
```

This forces **generation at a nominal 10 Hz**. Radio admission and valid source data
are still required, so the actual transmission rate can be lower.
The newest CAM replaces an older pending one; there is no catch-up burst.

## Command-line options

Both positional interface arguments are always required. Reactive DCC with CAM
disabled is the default.

| Option | Default | Effect |
| --- | --- | --- |
| `--limeric` | Off | Select the adaptive delta controller |
| `--fake-cbr` | Off | Read `fake_cbr_value` in reactive mode; incompatible with `--limeric` |
| `--cam-static` | Off | Enable CAMs with the bundled stationary sample position |
| `--cam-gps <device>` | Off | Enable CAMs from GPRMC/GNRMC over serial at 9600 baud |
| `--cam-station-id <id>` | `4316` | Set CAM station ID, unsigned 32-bit integer |
| `--cam-priority <0-7>` | `3` | Set the injected frame's QoS TID / user priority |
| `--cam-force-100ms` | Off | Force generation every 100 ms; requires `--cam-static` or `--cam-gps` |

Choose one CAM source: `--cam-static` and `--cam-gps` are mutually exclusive.
Station ID and priority settings take effect when a CAM source is
enabled. Unknown options and invalid values are rejected.

## CAM generation and EDCA

Normal generation implements the vehicle timing rules of
[ETSI EN 302 637-2 V1.4.1, §6.1.3](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf):

- First CAM as soon as valid source data are available; stationary generation at 1 Hz.
- Motion triggers for changes exceeding 4 m, 4 degrees or 0.5 m/s relative to the last successful driver enqueue.
- `T_GenCam_DCC` clamped to 100–1000 ms; movement retains the elapsed generation interval for three timed follow-ups, then the timer returns to 1000 ms.
- Generation checks every nominal 10 ms, independently of radio admission.
- Low-frequency container in the first CAM and the first CAM after its 500 ms deadline.

`--cam-force-100ms` overrides the generation triggers and DCC generation interval.
Without it, normal timing remains active. Neither mode guarantees radio delivery
at the generation rate.

CAMs use **GeoNetworking TC2 and QoS TID 3 → AC_BE** by default. The mapping is
specified in [ETSI TS 102 636-4-2 V1.4.1, table 4](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf).
The optional priority flag overrides the QoS TID while keeping the CAM's TC2:

| QoS TID / user priority | EDCA category |
| --- | --- |
| 0, 3 | AC_BE |
| 1, 2 | AC_BK |
| 4, 5 | AC_VI |
| 6, 7 | AC_VO |

Actual queue selection depends on the driver and radio. EDCA packet priority is
separate from the DCC worker's attempted `SCHED_FIFO` scheduling priority of 70.

## DCC control

CBR is acquired on nominal UTC-aligned 100 ms ticks in its own thread. The reader
uses `NL80211_CMD_GET_SURVEY` on the first interface and computes the ratio of
busy-time increments to active-time increments. The first reading establishes a
baseline. Missing, invalid or stale observations interrupt controller continuity
and close admission. The monitor prints CBR as a percentage; internal values and
synthetic input use fractions from 0 to 1.

### Reactive mode

The following are the project's configured radio parameters. The CBR ranges
initialize the state; later transitions move one adjacent state at a time after
1 second above the upper threshold or 5 seconds below the lower threshold.

| State | CBR range | TX power | Requested legacy bitrate | Gate minimum | CAM generation lower bound, normal mode |
| --- | --- | --- | --- | --- | --- |
| RELAXED | <30% | 23 dBm | 6 Mb/s | 50 ms | 100 ms |
| ACTIVE_1 | 30–<40% | 19 dBm | 12 Mb/s | 100 ms | 100 ms |
| ACTIVE_2 | 40–<50% | 16 dBm | 18 Mb/s | 200 ms | 200 ms |
| ACTIVE_3 | 50–<65% | 13 dBm | 24 Mb/s | 400 ms | 400 ms |
| RESTRICTIVE | ≥65% | 10 dBm | 24 Mb/s | 1000 ms | 1000 ms |

These are lower bounds, not fixed CAM periods. Airtime debt can delay admission
further, and a stationary CAM source normally generates at 1 Hz in every state.

### Adaptive mode

`--limeric` selects the delta controller in
[ETSI TS 102 687 V1.2.1, §5.4](https://www.etsi.org/deliver/etsi_ts/102600_102699/102687/01.02.01_60/ts_102687v010201p.pdf),
using consecutive 100 ms samples and UTC-aligned 200 ms updates. The gate uses
the delta budget and a 25 ms minimum. The project-specific power/bitrate policy
is separate from the delta algorithm. The interval exposed by the controller is
advisory and may exceed 1000 ms; the normal CAM generation input is clamped,
while radio airtime debt remains intact.

## Local ports and telemetry

| Endpoint | Purpose |
| --- | --- |
| UDP `127.0.0.1:5555` | Outgoing advisory notifications: native `int32_t` state in reactive mode, interval in ms in adaptive mode |
| UDP `127.0.0.1:5556` | Shared `ADMIT` / `ACCOUNT` / `REPORT` gate for external transmitters; integrated CAMs call the same gate directly |

The monitor updates every second. `dcc_telemetry_log.csv` is appended in the
working directory and records controller decisions and requested radio settings,
not hardware success. With CAM enabled, a separate `[CAM MONITOR]` line reports
CAM lifecycle counters and the driver enqueue rate. `cam_telemetry_estimated_log.csv`
records queue/gate waits, send-call duration and **estimated** airtime. It uses a
new schema and leaves the previous `cam_telemetry_log.csv` unchanged. No counter
claims hardware confirmation. See the [CAM telemetry guide](GUIDA_USO.md#telemetria-cam).
Press Ctrl+C or send SIGTERM to stop.

## Terminal dashboard (ncurses)

With DCC running, open a second terminal in the telemetry directory:

```bash
python3 dashboard.py
# Or point to another local telemetry directory:
python3 dashboard.py --log-dir /path/to/logs
```

The read-only dashboard uses Python's standard `curses` module (ncurses on
Linux/macOS), without pip dependencies or administrator privileges. Use a terminal
of at least 72 columns and 24 rows. It refreshes four times per second and shows
the latest logged CBR, reactive state or adaptive mode, advisory interval,
requested power/bitrate, CBR sample history and CAM counters, driver enqueue rate,
mean waits and estimated airtime. The graph displays up to one terminal width of
samples; its time range is printed below it. It is not a fixed-duration plot.

Missing files are retried automatically. DCC samples older than one second and
CAM samples older than three seconds are marked stale; historical values remain
visible. This describes log freshness, not gate admission or radio health. CAM
telemetry appears only when the CAM sender is enabled; its counters refer to the
latest logged CAM session. The dashboard follows file replacement and truncation.
It uses the local clock to interpret DCC timestamps, so run it on the same host.
Press `q`, Escape or Ctrl+C to close the dashboard; the separate DCC process keeps
running. Rebuild DCC after updating to ensure every telemetry record is flushed,
including when the controller state remains stable.

## Synthetic CBR and tests

In one terminal:

```bash
python3 cbr_generator.py --output fake_cbr_value
```

In another, from the same working directory:

```bash
sudo ./dcc wlan0 wlan1 --fake-cbr
```

The script defaults to output file `cbr`, so `--output fake_cbr_value` matters.
This mode substitutes CBR readings only; radio configuration and any enabled CAM
transmissions still use the real interfaces.

```bash
make test
```

Tests cover reactive transitions, adaptive reference values, CBR acquisition,
airtime accounting and UDP exchanges, CAM parsing/encoding, motion and timing
triggers, forced 100 ms generation, pending-message replacement, rate-based airtime estimation, driver enqueue
failures and concurrent logging. They do not require a radio; UDP tests require local socket access.
GitHub Actions builds and tests on Ubuntu. Passing tests does not replace a
Linux driver/radio integration test.

See the [usage guide](GUIDA_USO.md#problemi-comuni) for troubleshooting and the
[CAM guide](cam/README.md) for source-data and path-history limitations.

## Authors

netlab-dief team.
