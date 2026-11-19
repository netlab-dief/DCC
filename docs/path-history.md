# Concise CAM path history

CAM history now selects original fixes geometrically instead of copying every
changed coordinate. This reduces stationary GPS noise and straight-road payloads
while retaining sampled corners and curves. It does not change CAM generation
timing or LF scheduling.

## Reference and conformance boundary

[ETSI EN 302 637-2 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf),
§B.38, references SAE J2735 dated 2009-11-19. A newer SAE edition cannot silently
replace that reference.

The public implementation reference is the original
[VSC-A report, DOT HS 811 492B](https://www.nhtsa.gov/sites/nhtsa.gov/files/811492b.pdf),
Appendix B-2, §3.4, method three (printed pages B-2-9 to B-2-11).
This method checks intermediate positions against a segment and selects the last
acceptable endpoint when the error or chord length exceeds its calibration.
The report supplies defaults of 1 m sampled error, 310 m maximum chord and 300 m
retained distance. These values are reference calibrations, not additional ETSI
normative constants.

The complete referenced SAE 2009 text was not available for a clause-by-clause
audit during implementation. This implementation closes the missing geometric
selection functionality; it does not establish full SAE or stack conformance.

## Implementation

`cam/path_history.h` maintains a chronological open segment and selected points.
For a new fix it calculates each intermediate point's distance to the finite
segment, including the endpoints. If the maximum error exceeds 1 m or the chord
exceeds 310 m, it commits the previous candidate. Selected points are original
measurements; coordinates and timestamps are never interpolated.

WGS84 ECEF coordinates projected into the anchor's north/east tangent plane avoid
longitude-wrap and polar singularities. RMC has no altitude, so geometry uses
the reference ellipsoid; encoded altitude stays unavailable. The geometric
bound applies to sampled horizontal positions, not unknown motion between fixes
or absolute GNSS accuracy.

Retention walks backwards from the current position until the selected chords
cover 300 m, keeping the crossing point. Startup, tight curves and the 23-point
limit can yield shorter coverage. Output is newest first and omits the current
point. A history wholly within 1 m of the current position is omitted. Fixes
inside that tolerance can still move CAM referencePosition; this is not a GPS
position filter or an assertion that the vehicle is physically stationary.

Project safeguards:

- Duplicate and decreasing timestamps do not modify history.
- Missing observations for more than 2 s, invalid coordinates, explicit source
  invalidation or a jump over 310 m reset continuity. They are not bridged by
  invented points. The 2 s cutoff follows existing source-freshness policy.
- The open segment holds at most 2048 observations. At capacity it commits the
  previous candidate, preserving the error bound while bounding memory/work.
- Identical consecutive coordinates refresh the candidate time without adding
  redundant samples.
- Encoding stops before an unrepresentable position/time offset. CAM deltas
  remain relative to the preceding point, with time in 10 ms units. Very old
  history after a prolonged stop can therefore disappear.

## Validation

`tests/test_path_history.cpp` checks straight-road compression and distance
coverage, sampled error on a complete circle and an out-and-back reversal,
bounded stationary noise, the point cap, chronological ordering, source gaps,
jumps, invalid coordinates, dateline/polar geometry and old time offsets.
An independent planar test calculation verifies the emitted segments against
the original synthetic samples, allowing coordinate quantization.

`tests/test_cam.cpp` additionally decodes complete generated frames to check
selected point count, original offsets/timestamps, stationary jitter suppression
and history reset after source recovery. Existing generation, DCC, NET and
encoding tests remain part of `make test`.

## Radio check after implementation — 2026-11-19

`make -j2 all test` passed all 10 C++ executables and 13 Python tests.
Six live-radio sessions used the same simulated GPS input as the earlier size
validation. All 42 enqueues matched 42 injection records and 42 successful TX
statuses. There were no tshark expert messages, GN length mismatches, TX failure
flags or kernel drops. These are same-radio checks, not independent reception.

| GPS input | TX status count | CAM bytes | History point counts |
| --- | ---: | --- | --- |
| gps-stationary | 6 | 43 | 0 |
| gps-jitter | 6 | 43 | 0 |
| gps-moving | 27 | 41, 43, 52 | 0, 1 |
| gps-no-fix | 0 | — | — |
| gps-clock-offset | 0 | — | — |
| gps-dropout | 3 | 43 | 0 |

Stationary jitter now yields 43-byte CAMs with empty history, compared with up
to 241 bytes before selection. The straight moving input uses a single selected
anchor instead of retaining every fix; LF CAMs with this anchor occupy 52 bytes.
Curve accuracy is exercised by the synthetic geometric tests above. Invalid-fix
and clock-offset inputs generate no CAMs; stopping GPS input stops transmission
after source expiry. Ordinary generation/admission still depends on dynamics and
valid CBR; the moving test median TX interval was 201.33 ms.

Evidence (ignored by Git): `build/cam-size-validation-20261119-161732`, including
`results.json`, captures, decoded fields, telemetry, the test log and reproduction
script. The radio remained OCB at 5900 MHz / 10 MHz; 23 dBm and nominal multicast
rate 6 were restored. `make clean` removes these artifacts.
