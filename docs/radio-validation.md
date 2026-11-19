# Radio validation — 2026-11-19

[Overview](../README.md) · [Usage](usage.md) · [Field procedure](field-validation.md)

The validated host has `wlan0` and monitor `wlan2` on phy0, with OCB at
5900 MHz / 10 MHz. `wlan1` is a separate managed interface on phy1 and was not
used for CAM injection. Interface names must be verified on each host.

## Configuration checks

The production Linux radio backend was exercised with `wlan0` as its first
argument and an intentionally nonexistent second argument. All 15 combinations
of 10, 15 and 20 dBm with nominal legacy rates 6, 9, 12, 18 and 24 succeeded.
Each rate command received a kernel ACK; each power was read back and matched
the request. Invalid rate 999 was rejected. The check restored the original
power of 23 dBm and requested nominal rate 6. It transmitted no CAMs.

The full application build and `make test` passed: nine C++ executables on Linux
and 13 Python tests. Earlier restricted execution could not install socket
options; the host run passed. The subsequent complete run also passed.

To repeat the configuration check, see the
[opt-in hardware diagnostic](usage.md#opt-in-radio-hardware-check).

## CAM injection and capture

After configuring `wlan2` as the phy0 monitor, seven radio runs exercised both
controllers, static and forced CAM generation, DCC_NET decoration, QoS priority
and a synthetic congestion/recovery sequence with real transmission.

A total of 795 CAM enqueues matched 795 successful driver TX-status records,
795 estimated-airtime accounting events and the increase in the controlled
interface's multicast TX packet counter. All CAM records decoded through BTP-B
and ITS CAM, with no reported TX failures or retries. Tcpdump reported zero
kernel drops in every run.

The [detailed CAM report](cam-validation-2026-11-19.md) contains per-run counts,
intervals, radio settings, cleanup and artifact locations. It is self-contained;
local PCAP and CSV files are excluded from Git under `build/`.

## What remains to validate

Configuration readback and same-PHY TX statuses do not establish received delivery
on another station, physical Mbps, radiated EIRP, EDCA contention behaviour or
actual over-air airtime. The GPS used in these runs was the static source; no
real GPS receiver or independent DCC_NET peer was exercised.

Follow the [field procedure](field-validation.md) for real GPS input, reception
on another node, neighbour expiry and a remote-load control test. The project
keeps nominal Linux/radiotap rate labels; they must not be treated as measured
physical throughput on a 10 MHz channel.
