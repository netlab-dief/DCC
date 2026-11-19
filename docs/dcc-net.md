# DCC_NET: optional neighbour load exchange

[Overview](../README.md) · [Usage](usage.md) ·
[Architecture](architecture.md) · [CAM](../cam/README.md)

DCC_NET receives neighbour load from GeoNetworking single-hop broadcast (SHB)
frames, computes global CBR and optionally advertises local load through
integrated CAMs. It is disabled by default.

## Enable DCC_NET

```bash
sudo ./dcc wlan0 wlan2 --dcc-net --cam-static
sudo ./dcc wlan0 wlan2 --dcc-net --limeric --cam-gps /dev/ttyACM0
sudo ./dcc wlan0 wlan2 --dcc-net
```

Only `--dcc-net` enables the feature, with either controller. The last command
receives neighbour data and uses global CBR without generating local traffic.
Advertising requires a CAM source; NET creates no additional beacon or SHB.
It works with forced 100 ms CAM generation. Reactive `--fake-cbr` can substitute
local measurements, while reception and radio configuration remain real.

Without the flag, no GN receiver socket or NET log is created, controllers use
local CBR, and the four reserved SHB media-dependent bytes remain zero.

## Radio and power requirements

Use Linux, raw-socket privileges, an ITS-G5 radio and a radiotap monitor already
configured on the intended channel. The second interface receives SHB and injects
CAMs; the first supplies surveys, TX power and bitrate configuration. OCB broadcast
rate uses the multicast-rate command; injected CAMs carry radiotap RATE.
See [interface setup](usage.md) for creating the monitor. NET verifies through
sysfs that both interfaces share a PHY. It supports one fixed ITS-G5 channel per
instance and does not configure monitor mode, channel or width. Do not change
channel, PHY or MAC, perform channel hopping, or modify power from another process
during operation.

Advertised power comes from `NL80211_CMD_GET_INTERFACE` after configuration:

```text
EIRP = driver-reported configured power + antenna gain − cable loss
```

`--dcc-net-eirp-offset-db <dB>` supplies antenna gain minus cable loss, from
−31 to +31 dB, including fractional values. It requires NET; the default zero
assumes 0 dBi antenna gain and no cable loss.

```bash
sudo ./dcc wlan0 wlan2 --dcc-net --dcc-net-eirp-offset-db 2.5 --cam-static
```

Advertised EIRP is floored to 1 dBm resolution and saturated to 0–31 dBm.
Power configuration/readback and DCC-MCO patch/enqueue are serialized so the
controller cannot change power between field construction and driver enqueue.
A configuration/readback failure terminates NET with an error instead of
advertising assumed power.

The driver must expose power on the survey interface and apply that configuration
to monitor-injected frames. Readback confirms configuration, rather than measuring
each packet's radiated EIRP.

## Neighbour state and aggregation

The implementation follows the load-exchange model in
[ETSI TS 102 636-4-2 V1.4.1, §§5–7 and Annex A](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf),
with the boundary conventions described below.

`dccnet::State` stores one current observation per GN address on the receiving
interface. An entry contains remote local/one-hop CBR, SO PV timestamp, local
receive time, advertised EIRP and optional radiotap RSSI/frequency. Missing RSSI
remains absent; zero dBm is not substituted.

A NET worker computes a snapshot every nominal 100 ms, starting at a uniform
random phase from 0 to 99 ms. Deadlines and expiry use a monotonic clock; delayed
execution does not cause catch-up calculations. Entries expire after 1000 ms
and are removed on reception or the next calculation. The table is the DCC
LocTEX-G5 extension, rather than a full GN router. It holds at most 4096
addresses; at capacity, existing entries can update while new addresses are
ignored.

For remote local CBR and remote one-hop CBR separately:

1. Average the valid current values, one per neighbour.
2. If that mean exceeds `CBR_Target = 0.62`, select the maximum.
3. Otherwise select the second-largest value. Equal maxima from distinct
   neighbours count separately.

These produce `CBR_L_1_Hop` and `CBR_L_2_Hop`:

```text
CBR_G(n) = max(CBR_L_0_Hop(n−1), CBR_L_1_Hop(n), CBR_L_2_Hop(n))
```

The local term comes from the previous trigger. Remote terms start at zero;
control requires two consecutive valid local observations. Where fewer than two
values exist and the second-largest is needed, this implementation uses zero
because that sparse case is not explicit in the referenced specification. With
no neighbours, both remote terms return to zero and control uses the previous
interval's local value. NET's `0.62` target is separate from the adaptive
controller's unchanged `0.68` target.

## Controller integration and freshness

Each global snapshot is consumed at most once. Reactive transitions use global
CBR; adaptive control feeds it into the existing filter with the same 200 ms
update cadence and parameters. CBR-based radio policies also use global input.

Invalid, stale, repeated or skipped local observations invalidate NET control;
neighbours cannot replace missing local measurement. A trigger delayed by more
than 150 ms breaks continuity, and recovery again requires two valid intervals.
Global snapshots are valid for 150 ms. The production gate uses an explicit deadline bounded by both control and
local sample freshness (150 ms). Immediately before transmission, the sender checks local/global
freshness again.

## Reception and parser limits

An `AF_PACKET` socket bound to the monitor reads radiotap frames. The parser
handles extended radiotap bitmaps, field alignment, signed RSSI, announced FCS,
ordinary/QoS 802.11 headers, optional HT Control and padding. It accepts LLC/SNAP
with GN EtherType `0x8947` and unprotected SHB with consistent lengths. It need
not decode CAM payloads, so SHB from other services can contribute.

It rejects truncated packets, announced bad FCS, fragments, A-MSDU, DS/WDS,
MAC-encrypted traffic, secured GN and unsupported packet types. Cryptographic
verification and secured-GN decapsulation are outside this project; this is not
a complete secure GeoNetworking implementation.

`PACKET_OUTGOING` copies and GN addresses whose MID matches the local MAC are
ignored. Equal/older SO PV timestamps update neither values nor expiry. Timestamp
comparison supports 32-bit wrap-around.

## SHB DCC-MCO encoding

The existing four-byte media-dependent field contains:

| Byte within field | Value |
| --- | --- |
| 0 | `floor(local_CBR × 255)` |
| 1 | `floor(one_hop_CBR × 255)` |
| 2 | EIRP in network bits 0–4, the five most significant bits; remaining bits zero |
| 3 | Reserved MCO, zero |

The field follows the 24-byte SO PV at GN octets 36–39, as laid out in
[ETSI EN 302 636-4-1 V1.4.1, table 13](https://www.etsi.org/deliver/etsi_en/302600_302699/3026360401/01.04.01_60/en_3026360401v010401p.pdf).
Table 3 of TS 102 636-4-2 instead lists 40–43. This implementation interprets the
replacement requirement as using the existing field, without adding four bytes
or overwriting BTP/payload. Tests verify that placement.

Two-hop and global CBR are not advertised directly. After admission, the selected
frame's extension is patched immediately before enqueue, so gate waits do not
retain an old DCC-MCO value. CAM UPER and frame length stay unchanged.

There is no DCC-MCO presence flag. An all-zero field can represent valid zero
load/power and is accepted. A legacy peer leaving reserved bytes at zero cannot
be distinguished from a peer explicitly advertising zeros; no proprietary marker
is introduced. Packets too short to contain the field are rejected.

## Failure and shutdown

Receiver errors propagate to the main thread even without CAM enabled. Shutdown
wakes consumers, stops the monitor worker and closes its socket; radio reads do
not block indefinitely. A send error after admission retains the uncertain gate
reservation, as in local-only mode. Stop/drain the transmitter and driver queues
before restarting after an uncertain send.

## Telemetry

The historical controller schema stays unchanged. `CBR_Percent` in
`dcc_telemetry_log.csv` is global with NET enabled. `get_current_cbr()` and the
main console monitor still report local CBR in percent, or `-1` when unavailable.

`dcc_net_telemetry_log.csv` is created only when NET starts and is truncated at
each NET startup:

| Column | Meaning |
| --- | --- |
| `Timestamp` | Local ISO-style timestamp when the snapshot is consumed |
| `Valid` | `YES` for a usable control snapshot, otherwise `NO` |
| `Local_CBR_Percent` | Current local measurement; zero if invalid, so inspect `Valid` |
| `One_Hop_CBR_Percent` | Aggregated one-hop term |
| `Two_Hop_CBR_Percent` | Aggregated two-hop term |
| `Global_CBR_Percent` | Global value using the preceding trigger's local term |
| `Neighbors` | Number of unexpired observations |

The dashboard shows NET rows and invalid/stale status, and can still read
historical controller logs without added columns. A NET file from an earlier
session remains when NET is disabled; after one second it is marked stale.
The main dashboard graph follows controller CBR, including global input under NET.

## Tests and field validation

```bash
make test
# Focused NET suite:
make build/tests/test_dcc_net
./build/tests/test_dcc_net
```

CLI tests require a built Linux application and skip if the binary is absent.
Hardware-independent tests cover field bytes/bits, quantization/saturation,
independent synthetic frames, variable headers, RSSI, rejected traffic,
mean/second-maximum selection, the exact threshold, sparse neighbours,
duplicates, timestamp wrap, expiry, continuity and controller input. CAM tests
cover both zero-extension operation and extension integration.

For field validation, use two or three nodes on one channel. Enable NET and CAM
on transmitters, capture SHB and compare fields with logs. Stop a neighbour and
check its contribution disappears after one second; test remote congestion
exceeding local load. Validate configured power, EIRP assumptions, RSSI and actual
driver behaviour. Software tests alone do not certify radio interoperability or
complete stack compliance.

The [two-node field procedure](field-validation.md#two-node-neighbour-exchange)
provides commands, capture filters, neighbour-expiry checks and a synthetic
remote-load test. Nodes need distinct MAC/GN addresses; changing only the CAM
station ID does not change NET neighbour identity. Forced 100 ms generation is
useful for the reception test because normal stationary 1 Hz generation is
close to the 1000 ms neighbour lifetime and can produce brief expiry gaps.

## Entity interfaces

`DccNetwork` owns the receiver and neighbour state. Its CROSS input supplies the
measured local CBR and aggregation target; its resource query exposes available
airtime, estimated idle time and last enqueue feedback. SHB load exchange does
not require a forwarding scheduler. ACC serializes configured-power readback,
NET decoration and enqueue. See [architecture](architecture.md).
