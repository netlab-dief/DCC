# CAM capture validation — 2026-11-19

[Overview](../README.md) · [Radio validation](radio-validation.md) ·
[Field procedure](field-validation.md)

All software and hardware checks described below passed. Across seven live
radio runs, 795 CAM enqueues matched 795 successful driver TX statuses,
795 estimated-accounting events and a 795-packet increase in wlan0's multicast
TXQ counter (129 → 924). Tcpdump reported zero kernel drops in every run.

## Environment and software checks

- `wlan0`: phy0, OCB, 5900 MHz, 10 MHz channel width.
- `wlan2`: active radiotap monitor on the same phy0.
- `make -j2 all test`: exit status 0; nine C++ executables and 13 Python tests.
- `check_radio_hardware wlan0`: all 15 combinations of 10/15/20 dBm and nominal
  rates 6/9/12/18/24 succeeded, with rate-command ACK and matching configured
  power readback. Invalid rate 999 was rejected. Expected Netlink error messages
  in the software suite exercise failure paths.

## Live CAM runs

Each baseline run used a static source and real radio survey input for 14 seconds.
Intervals are medians between consecutive driver TX-status records.

| Run | CAMs / TX statuses | Median interval | Nominal radiotap RATE | TID |
| --- | ---: | ---: | --- | ---: |
| Reactive, stationary | 14 | 1005.774 ms | 6 | 3 |
| Reactive, forced 100 ms generation | 138 | 100.619 ms | 6 | 3 |
| Adaptive, stationary | 14 | 1006.071 ms | 12 | 3 |
| Adaptive, forced 100 ms generation | 135 | 100.656 ms | 12 | 3 |
| Reactive + DCC_NET, forced 100 ms | 136 | 100.677 ms | 6 | 3 |
| Adaptive + DCC_NET, forced 100 ms | 134 | 100.680 ms | 12 | 6 |
| Reactive, synthetic congestion, 42 s | 224 | Depends on state | 6/12/18/24 | 3 |

All CAM records decoded through radiotap, broadcast QoS 802.11, LLC/SNAP,
GeoNetworking SHB, BTP-B destination port 2001 and CAM UPER with station ID 4316.
No tshark expert messages were reported on the CAM records, and no GN/BTP length
mismatches were found. TX failure flags were clear and reported retries were zero.
Final `Submitted` and `Estimated_Accounted` matched the TX-status count for every
session; `TX_Errors` was always zero.

Each CAM produced two local capture records: the injection copy and the driver
TX-status copy, distinguished by the presence of `radiotap.txflags`. They were
counted separately, not treated as two transmissions. The reactive 100 ms run
also captured two unrelated mDNS frames.

With NET disabled the four-byte SHB extension was zero. With NET enabled the EIRP
bits were populated and followed configured power. Local/one-hop CBR bytes stayed
zero on the lightly loaded channel. TID 6 appeared in its dedicated run and TID 3
in the others. These runs checked NET decoration, without an independent peer.

## Congestion and recovery

The reactive congestion run used synthetic CBR with real radio transmissions:
0% for 3 s, 35% for 3 s, 45% for 3 s, 55% for 3 s, 80% for 6 s and then 0% for
24 s. Telemetry confirmed the following progression:

```text
RELAXED → ACTIVE_1 → ACTIVE_2 → ACTIVE_3 → RESTRICTIVE
        → ACTIVE_3 → ACTIVE_2 → ACTIVE_1 → RELAXED
```

All four nominal rates appeared in the capture. RESTRICTIVE reduced transmission
to about 1 Hz, with a maximum observed interval of 1000.108 ms; recovery restored
about 10 Hz. While admission was blocked, 194 pending CAMs were replaced by newer
samples. There were no recorded TX errors, expirations or invalidations.

## Cleanup and evidence limits

All test DCC and tcpdump processes terminated. wlan0 remained OCB at
5900 MHz / 10 MHz and was restored to 23 dBm. Nominal multicast rate 6 was restored
using `iw dev wlan0 set mcast_rate 6` with exit status 0. An earlier attempt to
restore a legacy unicast mask returned EINVAL in OCB; the multicast command
corrected it.

Same-PHY capture supplies driver TX statuses, not proof of reception at another
station. Radiated power and physical bitrate were not measured. RATE values above
are nominal Linux/radiotap labels. A real GPS receiver and neighbour DCC_NET radio
exchange were not exercised; the corresponding parsers and state handling passed
software tests. Synthetic CBR verifies control response without creating RF
congestion. For the remaining checks, follow the [field procedure](field-validation.md).

## Local artifacts

The results above are retained in this tracked report. Raw evidence was saved
locally under the following ignored directories and is not included in a Git push:

```text
build/radio-validation-20261119-103757/
build/radio-congestion-validation-20261119-103956/
```

Each contains `analysis.json` and `results.json`; each run subdirectory contains
`capture.pcap`, `decoded.tsv`, `dcc.log`, `tcpdump.log` and telemetry CSV files.
The baseline directory also contains `full-tests.log` and `analyze-captures.py`.
Both directories contain `run-validation.py` copies updated with the correct
multicast-rate restore command. These scripts encode this host's paths and
ownership; adapt them before use on another host.

`make clean` removes the evidence directories. Archive raw files elsewhere before
cleaning if they need to be shared or retained. The field guide provides portable
manual reproduction steps without depending on these local artifacts.
