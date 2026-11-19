# Integrated CAM generator

[Overview](../README.md) · [Usage](../USAGE.md) ·
[Architecture](../CODEBASE_ANALYSIS.md) · [DCC_NET](../DCC_NET.md)

The integrated sender constructs CAMs, encapsulates them in GeoNetworking SHB
frames and injects them through the second interface passed to `dcc`. It uses
the internal DCC airtime gate directly, with admission and accounting inside
the same process.

## Quick start

Build with `make` in the repository root. Linux, raw-socket privileges and a
monitor interface configured for radiotap injection are required.

```bash
sudo ./dcc wlan0 wlan1 --cam-static
sudo ./dcc wlan0 wlan1 --cam-gps /dev/ttyACM0
sudo ./dcc wlan0 wlan1 --limeric --cam-static --cam-station-id 4316
```

Select one source. Station ID defaults to `4316`, QoS priority to `3`, and normal
vehicle generation is enabled unless `--cam-force-100ms` is supplied. See the
[option reference](../USAGE.md#command-line-reference) for validation and defaults.

## Source data and timestamps

| Source | Behaviour |
| --- | --- |
| Static | Position 44.6292708° N, 10.9479523° E; speed zero; heading unavailable |
| GPS | Serial at 9600 baud; partial-line assembly; checksummed GPRMC/GNRMC with valid `A` fix |

GPS mode stops producing usable messages when the fix is invalid or older than
two seconds. Duplicate or out-of-order fixes do not extend source validity.
Fixes more than two seconds behind or ahead of host UTC are rejected for
message generation. Date validation includes leap years. Synchronize the host
UTC clock before use.

CAM reference-position time comes from RMC date/time, converted to elapsed TAI
milliseconds since the ITS epoch (2004-01-01 00:00:00 UTC), modulo 65536 ms.
Static CAMs and GeoNetworking use the same conversion from the system UTC clock.
The shared conversion includes the five leap-second insertions from 2004 through
2017; its table must be extended when future insertions become effective.
Coordinates use 10⁻⁷ degrees, speed uses cm/s and heading uses 0.1° units.
RMC provides position, speed and heading without measurement confidence data.

The station is an ordinary `passengerCar`, with no special-vehicle container.
Missing vehicle dimensions, altitude, acceleration, curvature, yaw rate and
confidence values are encoded as unavailable; optional fields without sensors
are omitted. There is no CAN data source or hardware airtime sensor.

## Encoding and frame format

`UtilsEncode/` and `Asn1_schemas/` were imported from `ETSI_stack/cam_c++` and are
bundled in this repository. `UtilsEncode/utils.c` constructs CAMs and handles
allocation/encoding errors. `cam_message.cpp` uses the full encoded payload
directly, replacing the original binary/hexadecimal round trip and fixed
50-byte payload.

Frames contain:

```text
radiotap → broadcast QoS 802.11 → LLC/SNAP → GeoNetworking SHB
         → BTP-B destination port 2001 → complete CAM UPER payload
```

The interface supplies the MAC address. Network fields are serialized explicitly;
GN payload length includes BTP. CAM and GN headings are encoded separately:
CAM retains `3601` for unavailable heading, while GN uses zero as a fallback.
GN heading is also zero for a stationary station. Available 360° headings are
normalized to 0°. For a moving source with unknown heading, GN zero is a
fallback rather than a northbound measurement; CAM still signals unavailable.

Speed is also mapped separately. CAM retains `16383` (`SpeedValue_unavailable`)
when RMC supplies no speed; GN uses zero as an unknown-value fallback. The old
`0x7fff` encoding would represent -0.01 m/s in GN's signed 15-bit speed field.
GN zero alone therefore does not prove standstill. Available speeds are preserved
in both encodings, including the maximum available CAM value of 16382 cm/s.

## Generation policy

Normal generation implements the vehicle timing policy described in
[ETSI EN 302 637-2 V1.4.1, §6.1.3](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf).
The producer checks conditions every nominal 10 ms, independently of the
transmitter's gate wait:

```text
T_GenCam_DCC = clamp(current DCC interval, 100 ms, 1000 ms)
```

If the interval is unavailable, the input defaults to 100 ms. The first CAM is
generated as soon as source data become valid. Subsequent generations respect
`T_GenCam_DCC` and use either a motion trigger or the generation timer:

- Motion triggers when distance changes by more than 4 m, heading by more than
  4°, or speed by more than 0.5 m/s relative to the last successful driver enqueue.
  Heading uses the shortest angular difference across 0°/360°. Unavailable
  heading/speed does not trigger its corresponding condition.
- `T_GenCam` starts at 1000 ms. A motion trigger sets it to the elapsed interval
  since the previous generation. That interval is retained for `N_GenCam = 3`
  timed follow-ups, then returns to 1000 ms. A new motion trigger resets the count.

Elapsed generation time is measured from the start of the previous generation,
without adding another interval after send returns. A stationary source normally
generates at 1 Hz; motion may raise this to 10 Hz within DCC and fix availability
limits. Static mode behaves as a stationary vehicle and supplies no simulated
motion triggers.

| Reactive state | DCC gate minimum | Normal CAM generation lower bound |
| --- | --- | --- |
| `RELAXED` | 50 ms | 100 ms |
| `ACTIVE_1` | 100 ms | 100 ms |
| `ACTIVE_2` | 200 ms | 200 ms |
| `ACTIVE_3` | 400 ms | 400 ms |
| `RESTRICTIVE` | 1000 ms | 1000 ms |

These are lower bounds, not fixed generation periods. The stationary timer
remains 1000 ms in every state.

### Forced 100 ms generation

```bash
sudo ./dcc wlan0 wlan1 --cam-static --cam-force-100ms
sudo ./dcc wlan0 wlan1 --limeric --cam-gps /dev/ttyACM0 --cam-force-100ms
```

`--cam-force-100ms` requires a CAM source and selects nominal 100 ms generation
in every state, including `RESTRICTIVE`. The first CAM is immediate once data
are valid. This overrides motion triggers, `N_GenCam` and `T_GenCam_DCC` for
generation; it retains source validity, expiry, the latest-message slot,
low-frequency scheduling and radio admission.

Actual enqueue frequency can remain below 10 Hz. Delays do not cause catch-up
bursts: the next generation is at least 100 ms after the actual previous one.

## Pending messages, lifetime and containers

Generation and transmission are separate. One pending slot holds the newest
CAM; a new message replaces an older waiting message. The producer can continue
while the transmitter waits. Completion of an older selected version cannot
clear a newer pending version.

A construction taking at least 50 ms is discarded without consuming the trigger,
allowing a fresh attempt at the next check. A message is not selected after
1000 ms or after source expiry. Initial GN lifetime is 1000 ms; before enqueue,
local queue age is subtracted in 50 ms units. Messages with less than 50 ms
remaining are dropped. These checks do not measure kernel/driver queue time.
The 1000 ms generation bound does not force the gate to open or erase airtime
debt.

The low-frequency container is included in the first CAM and the first generation
at least 500 ms after its previous inclusion. It does not trigger an independent
send at 500 ms. If its pending CAM is replaced, the replacement retains the
container. It contains ordinary vehicle role, unavailable exterior lights
(zero bits), and available fix history with up to 23 points.

History is newest-first, uses offsets from the preceding point and times in
10 ms units, starts empty, and breaks across an unrepresentable gap. Its quality
and resolution depend on the source fixes. This is fix history; SAE J2735 path
selection/compression is not implemented.

## QoS priority and EDCA

`--cam-priority <0-7>` sets the user priority in the 802.11 QoS TID for either
source mode:

```bash
sudo ./dcc wlan0 wlan1 --cam-static --cam-priority 6
sudo ./dcc wlan0 wlan1 --cam-gps /dev/ttyACM0 --cam-priority 4
```

| User priority / TID | EDCA category |
| --- | --- |
| 0, 3 | AC_BE — Best Effort |
| 1, 2 | AC_BK — Background |
| 4, 5 | AC_VI — Video |
| 6, 7 | AC_VO — Voice |

The default is **TID 3 / AC_BE**, with **GeoNetworking TC2**. The TC2 → UP3 → AC_BE
mapping is documented in
[ETSI TS 102 636-4-2 V1.4.1, table 4](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf);
CAM DP2 is also discussed in
[ETSI TS 102 724 V1.1.1, Annex C.2](https://www.etsi.org/deliver/etsi_ts/102700_102799/102724/01.01.01_60/ts_102724v010101p.pdf).
Changing TID retains the CAM's TC2. Missing, negative, nonnumeric or out-of-range
values are rejected. Startup reports the selected priority/category.

The injected TID is a user priority, not a hardware queue number. mac80211 uses
its priority/category mapping for QoS frames; actual queue behaviour depends on
the driver and available queues. This flag does not configure AIFS, contention
windows or TXOP. EDCA priority is separate from DCC worker scheduling and from
airtime admission.

## Estimated airtime

For each selected packet, the sender snapshots the manager's current nominal
legacy bitrate and writes it into [radiotap RATE](https://radiotap.org/fields/Rate.html).
That packet's rate and airtime estimate remain fixed through admission; a
concurrent DCC update affects the next selection.

The model counts the complete MAC frame: QoS 802.11, LLC/SNAP, GN, BTP and all
present CAM containers. It excludes radiotap, adds four FCS bytes, and includes
OFDM SERVICE, TAIL, whole-symbol rounding, preamble and SIGNAL:

```text
L = MAC frame length excluding radiotap, in bytes
R = nominal DCC/Linux legacy rate, in Mb/s
N_DBPS = 4 × R
estimated_airtime_10MHz = 40 + 8 × ceil((16 + 8 × (L + 4) + 6) / N_DBPS)  [µs]
```

The Linux nominal rates used by DCC are 6, 12, 18 and 24 Mb/s. At 10 MHz, the
physical rate for the same modulation/coding is half its nominal 20 MHz value.
The model uses **10 MHz timings**: 8 µs symbols and 40 µs preamble/SIGNAL.
On a 20 MHz radio it overestimates airtime by a factor of two. It does not detect
channel width automatically. The PHY reference is
[ETSI EN 302 663 V1.3.1, tables C.1–C.2](https://www.etsi.org/deliver/etsi_en/302600_302699/302663/01.03.01_60/en_302663v010301p.pdf).

This estimates one broadcast transmission without ACK. It excludes backoff,
driver waiting and unobserved extra attempts, and assumes the driver honors the
requested RATE. Validate those assumptions on the intended radio.

The sender obtains a channel-wide admission ticket from the internal gate, injects
with `AF_PACKET/SOCK_RAW`, and accounts the estimated airtime after a complete
successful `sendto()`. This closes the reservation without waiting for PHY
feedback; it does not prove hardware completion or reception.

Failed/partial sends stop the sender and retain the uncertain reservation.
See [internal CAM admission](../USAGE.md#internal-cam-admission) for pacing and
recovery semantics. The sender needs no external client or feedback service.

## Optional DCC-MCO extension

```bash
sudo ./dcc wlan0 wlan1 --dcc-net --cam-static
sudo ./dcc wlan0 wlan1 --dcc-net --limeric --cam-gps /dev/ttyACM0
```

Without NET, the four SHB media-dependent bytes remain zero. With NET, the sender
patches local CBR, one-hop CBR and configured EIRP after gate admission,
immediately before enqueue. UPER payload, frame length and estimated airtime
are unchanged. This feature works with either source, any priority and forced
100 ms generation.

The second interface also receives SHB. Both interfaces must share a PHY/channel,
and configured TX power must be readable. The optional EIRP offset accounts for
antenna gain minus cable loss; zero assumes 0 dBi and no loss. Controllers use
global CBR. See [DCC_NET](../DCC_NET.md) for encoding, freshness and parser limits.

## Telemetry and validation

`[CAM MONITOR]` and `cam_telemetry_estimated_log.csv` report generation,
replacement, expiry, invalidation, submission, estimated accounting, errors,
enqueue frequency and mean timing. `Submitted_Hz` measures successful driver
enqueues, rather than receptions or PHY completion. See the
[CAM column definitions](../USAGE.md#cam-telemetry).

`make test` covers RMC parsing, UPER round trips, frame structure, motion
thresholds, angle wrap, unavailable data, `N_GenCam`, stationary timing,
low-frequency/history encoding, forced generation, internal admission, pending
replacement/expiry, generation during a blocked send, estimated accounting and
uncertain-send failure. No radio is required for these tests.

Validate full Linux builds, injection, queue mapping, bitrate application and
airtime estimates against the intended driver and radio. The current
implementation does not certify the complete ITS stack or hardware.
