# GPS and DCC_NET field validation

[Overview](../README.md) · [Usage](usage.md) · [CAM](../cam/README.md) ·
[DCC_NET](dcc-net.md) · [Completed radio validation](radio-validation.md)

This procedure checks real GPS input and reception by an independent radio.
The commands use `wlan0` as the controlled OCB interface and `wlan2` as its
radiotap monitor. Adapt names and serial paths on each host. Run DCC commands
from the repository root; logs are written in the current directory.

## Equipment and preparation

- Node A: Linux host with this project and an OCB-capable radio supporting
  surveys, power readback and monitor injection/reception.
- Real GPS check: a serial NMEA receiver configured for 9600 baud and GPRMC or
  GNRMC output with UTC date/time and checksum.
- Neighbour check: node B with the same software and a second compatible radio.
  A GPS on B is optional; static CAMs are sufficient for NET validation.
- Both radios on the same channel and width. The completed local tests used
  5900 MHz / 10 MHz; each node's monitor must share its controlled PHY.
- Distinct MAC addresses on the two nodes. Use distinct CAM station IDs as well.
  NET identifies neighbours by GN address/MAC, not by CAM station ID.

Inspect each node before starting:

```bash
iw dev
ip -brief link
iw dev wlan0 info
```

The application does not configure OCB mode, join a channel or create a monitor.
See [radio preparation](usage.md#radio-preparation). Record the initial power
and channel settings. Use one DCC instance per controlled radio.

Install optional capture tools on Debian/Ubuntu:

```bash
sudo apt install tcpdump tshark
```

## Real GPS input

### Find the serial device and inspect NMEA

Connect the receiver and inspect stable device names and host time:

```bash
ls -l /dev/serial/by-id/
timedatectl status
```

USB receivers often appear as `/dev/ttyACM0` or `/dev/ttyUSB0`; a
`/dev/serial/by-id/` symlink can also be passed to `--cam-gps`. Synchronize host
UTC before the test. The parser rejects fixes more than two seconds ahead of
or behind the host's ITS timestamp.

With DCC stopped, inspect up to ten seconds of raw output:

```bash
sudo stty -F /dev/ttyACM0 9600 raw -echo
sudo timeout 10 cat /dev/ttyACM0
```

Expect `$GPRMC,...` or `$GNRMC,...` sentences ending in `*hh`, with status `A`
for a valid fix. Status `V` is invalid. The date/time must increase and represent
current UTC. Other NMEA sentences alone do not drive this sender. A timeout
exit status of 124 is expected when the inspection reaches its time limit.

Configure the receiver's baud rate and sentence output using its own controls
if needed; `stty` configures the host serial port, not the receiver firmware.
Stop the inspection and any daemon reading that port before starting DCC.
The sender reads the serial device directly and does not use a gpsd connection.

### Generate CAMs from the receiver

```bash
sudo ./dcc wlan0 wlan2 \
  --cam-gps /dev/ttyACM0 --cam-station-id 4316
```

In a second terminal:

```bash
tail -f cam_telemetry_estimated_log.csv
```

Acceptance checks:

- `Generated`, `Submitted` and `Estimated_Accounted` increase while fixes are
  valid; `TX_Errors` stays zero.
- Normal stationary generation is about 1 Hz. Motion can increase it subject to
  DCC admission and fix availability. `--cam-force-100ms` requests nominal 10 Hz
  generation, without increasing GPS measurement frequency.
- Decode a received CAM on node B as described below. Compare its numeric
  latitude, longitude, speed and heading with the GPS input, allowing for NMEA
  units and encoding resolution. Missing values retain unavailable sentinels.
- If input stops or the fix becomes invalid, usable source data expire within
  two seconds. An already transmitted frame can still be in flight. Fresh valid
  increasing fixes permit generation again.

Disconnecting the serial USB device can instead cause a sender error and process
shutdown. Use loss of fix or stopped NMEA output to test source expiry separately
from device-disconnection handling.

## Two-node neighbour exchange

### Start both nodes

First use static sources to isolate radio/NET behaviour from GPS availability.
Start capture on node A in another terminal before starting the applications:

```bash
sudo timeout -s INT 30 tcpdump -i wlan2 -s 0 -U -w node-a.pcap
```

On node A:

```bash
sudo ./dcc wlan0 wlan2 --dcc-net \
  --cam-static --cam-force-100ms --cam-station-id 4316
```

On node B:

```bash
sudo ./dcc wlan0 wlan2 --dcc-net \
  --cam-static --cam-force-100ms --cam-station-id 4317
```

Both nodes require `--dcc-net` and a CAM source to exchange their load in both
directions. Running NET without a CAM source receives load but advertises none.
Keep `--cam-force-100ms` for this initial check: normal stationary generation is
near the 1000 ms neighbour lifetime and scheduling jitter can cause brief expiry
gaps. Admission may still reduce actual frequency under load.

### Check reception and neighbour state

On each node:

```bash
tail -f dcc_net_telemetry_log.csv
```

After valid local observations and remote frames arrive, expect `Valid=YES` and
`Neighbors=1` in a two-node setup with no other accepted SHB sources. This file
is recreated at NET startup; archive it before restarting if it is evidence.

Decode B's CAMs captured on A:

```bash
tshark -r node-a.pcap \
  -Y 'its.messageID == 2 && its.stationID == 4317' -V
```

Check the remote source MAC, broadcast destination, GN SHB, BTP destination port
2001 and station ID 4317. On B, capture independently and filter for station ID
4316 to verify the reverse direction. Station ID filters identify the test CAMs;
they do not establish the identity of another device using the same ID.

Local captures can include both the injection record and a driver TX-status copy
for each local CAM. Count them separately using the presence of
`radiotap.txflags`; do not interpret them as two transmitted CAMs. Remote packets
from the other node provide the independent reception evidence. A radio may
omit RATE, RSSI or other metadata, so absent fields are not zero measurements.

### Check neighbour expiry

Stop DCC on B with Ctrl+C while A continues. On A, B's observation should expire
about 1000 ms after its last accepted frame, at reception or the next nominal
100 ms aggregation trigger. `Neighbors` should return to zero if no other
neighbours exist. Restart B and verify it reappears.

### Check the effect of remote load

One neighbour with a low advertised CBR can produce zero aggregated one-hop
load: below the 0.62 mean threshold the implementation selects the
second-largest value, and a missing second value contributes zero. This does
not mean reception failed. See [aggregation](dcc-net.md#neighbour-state-and-aggregation).

For a clear remote-load test, keep A on real surveys. Stop B, then on B write a
synthetic local CBR of 80% and restart in reactive mode:

```bash
printf '0.80\n' > fake_cbr_value
sudo ./dcc wlan0 wlan2 --dcc-net --fake-cbr \
  --cam-static --cam-force-100ms --cam-station-id 4317
```

Expected on A, while local samples remain valid and B is heard:

- `Neighbors=1` and `Valid=YES`.
- `One_Hop_CBR_Percent` near 80%, allowing for encoding as `floor(CBR × 255)`.
- Global CBR near or above that value; with sustained observations the reactive
  controller moves toward RESTRICTIVE. Radio requests and admission follow it.

The SHB DCC-MCO field is at GN octets 36–39: local CBR, one-hop CBR, EIRP bits,
then reserved zero. Inspect raw bytes using `tshark -r node-a.pcap -x` and the
[encoding description](dcc-net.md#shb-dcc-mco-encoding). A dissector may display
these bytes as reserved rather than naming the MCO fields.

This test sends real radio packets carrying a synthetic load value. It checks
NET propagation and control response, without generating RF congestion. For a
true two-hop aggregation test, use at least three nodes with a controlled
reception topology; two nodes alone cannot establish hidden-neighbour behaviour.

After the synthetic test, stop B and restart without `--fake-cbr` to restore
survey input. A missing `fake_cbr_value` also falls back to survey input, but
changing source requires fresh consecutive samples before admission resumes.

### Combine GPS and NET

Once the independent checks pass, run on A:

```bash
sudo ./dcc wlan0 wlan2 --dcc-net \
  --cam-gps /dev/ttyACM0 --cam-station-id 4316 --cam-force-100ms
```

B can continue with a static source or use its own GPS. Add `--limeric` to select
adaptive control; it cannot be combined with `--fake-cbr`.

## Evidence and cleanup

Save the command lines, interface/PHY/channel configuration, software revision,
GPS settings, captures from both nodes and all three telemetry CSVs. Compare
per-session CAM counters, decoded remote payloads, NET validity and neighbour
expiry. `Submitted` alone records driver enqueue and does not prove reception.

Stop both DCC processes and finish captures before changing radio settings.
Normal shutdown leaves the last power and bitrate in place. Restore your initial
power and use the OCB multicast rate command as described in
[hardware cleanup](usage.md#opt-in-radio-hardware-check).

The completed local validation artifacts are under ignored `build/` directories
and are removed by `make clean`. Copy captures outside `build/` before cleaning
if you need to retain them. Published results and the limits of the completed
checks are in the [CAM validation report](cam-validation-2026-11-19.md).
