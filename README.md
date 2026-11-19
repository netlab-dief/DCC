# DCC — Decentralized Congestion Control

C++17 congestion control for Linux ITS-G5 experiments, with separate DCC_ACC,
DCC_NET and DCC_CROSS entities and a CAM Facilities adapter. DCC measures Channel Busy
Ratio (CBR), requests radio power and legacy bitrate settings, and admits packets
through an internal airtime gate. It includes a CAM generator, optional neighbour
load exchange (DCC_NET), CSV telemetry and a terminal dashboard.

## Documentation

| Guide | Contents |
| --- | --- |
| [Usage](docs/usage.md) | Installation, every command-line option, controller settings, telemetry, internal admission and troubleshooting |
| [CAM](cam/README.md) | Source data, generation rules, frame format, EDCA, estimated airtime and limitations |
| [DCC_NET](docs/dcc-net.md) | Neighbour reception, global CBR, DCC-MCO encoding, radio requirements and validation |
| [Standards profile](docs/standards.md) | Fixed ETSI versions, requirement mapping and explicit project policies |
| [Architecture](docs/architecture.md) | Modules, ownership, threads, timing, admission and failure handling |
| [Field validation](docs/field-validation.md) | Real GPS, two-node DCC_NET, packet capture and acceptance checks |
| [Radio validation](docs/radio-validation.md) | Completed hardware checks, CAM capture results and remaining limits |

## Repository layout

| Directory | Contents |
| --- | --- |
| `src/app/` | Executable entry point |
| `src/dcc/` | Access/Cross entities, Facilities adapter, contracts and shared airtime gate |
| `src/radio/` | CBR acquisition and radio configuration |
| `src/net/` | DCC_NET entity, neighbour state and monitor receiver |
| `src/common/` | Shared logging, utilities and data definitions |
| `cam/` | Existing CAM implementation, ASN.1 sources, schemas and guide; preserved in place |
| `tools/` | Python dashboard and synthetic CBR generator |
| `tests/` | C++ and Python test sources |
| `docs/` | Usage, architecture, standards, field procedures and validation reports |
| `build/` | Generated objects, dependencies, test binaries and compatibility headers |

The main executable remains `./dcc`. Commands below run from the repository
root; telemetry and synthetic input paths still use the working directory.
Root-level documentation forwarding pages preserve earlier links.

## Build

The full application requires Linux, a C++17 compiler, GNU Make, pkg-config,
OpenSSL and libnl development libraries. Python 3 runs the dashboard, synthetic
CBR generator and Python tests. On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install build-essential pkg-config libssl-dev libnl-3-dev libnl-genl-3-dev iw python3
make
```

The executable is `./dcc`. All ASN.1 sources are bundled; a separate ETSI stack
checkout is unnecessary. Use `make clean` followed by `make` for a clean rebuild.
Some hardware-independent tests also run on macOS; the full application and raw
radio operations require Linux.

## Run

First identify the interfaces:

```bash
iw dev
```

Command syntax:

```text
sudo ./dcc <network_interface> <radiotap_interface> [options]
```

| Argument | Role |
| --- | --- |
| `network_interface`, e.g. `wlan0` | CBR survey, TX power and legacy bitrate configuration |
| `radiotap_interface`, e.g. `wlan2` | CAM injection and optional DCC_NET reception |

Power and bitrate target the first interface. OCB uses the multicast-rate command;
other modes use a 5 GHz legacy mask. The second argument is only for radiotap I/O.
Without CAM/NET, `sudo ./dcc wlan0 wlan0` is sufficient. With CAM/NET, `wlan2`
must be a monitor on the controlled PHY, not an unrelated managed radio.
See [interface setup and hardware checks](docs/usage.md).

The validated setup uses `wlan0` in OCB mode and `wlan2` as its monitor on
phy0, at 5900 MHz / 10 MHz. `wlan1` on that host is a separate managed radio.
Names and channel settings on another host must be checked with `iw dev`.

Configure the radio and channel before starting DCC. CAM and DCC_NET require a
monitor interface supporting radiotap injection/reception. The measured channel
must correspond to the controlled transmitter; DCC_NET additionally checks that
both interfaces share a PHY. DCC does not configure monitor mode, channel or
channel width. Radio operations normally require administrator privileges and
compatible driver support.

```bash
# Reactive control, without CAM generation (default)
sudo ./dcc wlan0 wlan2

# Adaptive delta controller, selected by the historical --limeric flag
sudo ./dcc wlan0 wlan2 --limeric

# CAMs from a stationary sample position or a GPS serial device
sudo ./dcc wlan0 wlan2 --cam-static
sudo ./dcc wlan0 wlan2 --limeric --cam-gps /dev/ttyACM0

# Nominal 100 ms CAM generation, including while stationary
sudo ./dcc wlan0 wlan2 --cam-static --cam-force-100ms

# Neighbour load exchange with integrated CAM transmission
sudo ./dcc wlan0 wlan2 --dcc-net --cam-static
```

Both interface arguments are always required. CAM generation is disabled until
one source is selected. `--cam-static` and `--cam-gps` are mutually exclusive.
See the [complete option reference](docs/usage.md#command-line-reference) for defaults,
validation and additional settings. Press Ctrl+C or send SIGTERM to stop.

## Generation, admission and transmission

Normal CAM generation checks motion and timing conditions every nominal 10 ms.
A stationary source produces one CAM per second; motion can increase generation
to 10 Hz, subject to the DCC generation lower bound. `--cam-force-100ms` selects
nominal 10 Hz generation. Source validity and packet admission still apply, so
actual driver enqueue frequency can be lower. Only the newest pending CAM is
retained; delayed generation does not cause a catch-up burst.

The sender injects frames directly through the second interface, with no
external driver bridge. It estimates airtime from the complete MAC frame and
selected legacy bitrate, writes that bitrate into radiotap RATE, and accounts
the estimate after a successful `sendto()`. This records driver enqueue, rather
than PHY completion or delivery. The airtime model uses 10 MHz OFDM timings;
[the CAM guide](cam/README.md#estimated-airtime) explains its assumptions.

Reactive control uses five states with asymmetric transition persistence.
Adaptive control updates a delta airtime budget on consecutive CBR samples.
The [usage guide](docs/usage.md#controllers) documents thresholds, requested radio
settings and gate timing. With `--dcc-net`, both controllers use global CBR;
the main console monitor continues to show local CBR.

## Monitoring

Telemetry is written in the working directory:

| File | Availability | Write policy |
| --- | --- | --- |
| `dcc_telemetry_log.csv` | Controller | Append |
| `cam_telemetry_estimated_log.csv` | CAM enabled | Append, with per-session counters |
| `dcc_net_telemetry_log.csv` | DCC_NET enabled | Recreated at NET startup |

In another terminal on the same host:

```bash
python3 tools/dashboard.py
python3 tools/dashboard.py --log-dir /path/to/logs
```

The dashboard requires Python's standard `curses` module and a terminal of at
least 72 columns by 24 rows. Press `q`, Escape or Ctrl+C to close it; DCC continues
running. See [telemetry](docs/usage.md#telemetry) for column definitions and freshness.

The integrated CAM generator calls the internal airtime gate directly. Admission
and estimated-airtime accounting run inside the DCC process; no external client
or local UDP service is used.

## Synthetic CBR and tests

In one terminal, write synthetic fractional CBR values:

```bash
python3 tools/cbr_generator.py --output fake_cbr_value
```

From the same working directory, start reactive control in another terminal:

```bash
sudo ./dcc wlan0 wlan2 --fake-cbr
```

The generator defaults to `cbr`, so the explicit output filename matters.
`--fake-cbr` replaces the CBR source only: radio configuration and enabled CAM
transmissions still use real interfaces. It cannot be combined with `--limeric`.

```bash
make test
```

Tests cover controllers, sampling, the shared gate, CAM encoding/generation,
DCC_NET, architecture integration, manager lifecycle with a simulated radio, the dashboard and Python utilities. They require no radio. CLI tests skip when the application binary is
absent. GitHub Actions builds and tests on Ubuntu. Hardware behaviour still
requires validation with the intended Linux driver and radio.

The [2026-11-19 CAM validation](docs/cam-validation-2026-11-19.md) recorded
795 CAMs with matching successful driver TX statuses across seven radio runs.
Same-PHY capture does not establish reception by a second station. Follow the
[field procedure](docs/field-validation.md) to validate GPS input and neighbour
exchange with an independent node.

## Authors

netlab-dief team.
