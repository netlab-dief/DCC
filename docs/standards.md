# Fixed DCC standards profile

[Architecture](architecture.md) · [Usage](usage.md) · [DCC_NET](dcc-net.md)

This project targets a fixed ITS-G5 Release 1 DCC architecture on a single
channel. Version numbers are intentional. This is an implementation profile and
traceability record, not a declaration of complete stack/radio conformity.

## References

| Function | Fixed reference |
| --- | --- |
| Access DCC algorithms | [ETSI TS 102 687 V1.2.1](https://www.etsi.org/deliver/etsi_ts/102600_102699/102687/01.02.01_60/ts_102687v010201p.pdf) |
| Cross-layer DCC management | [ETSI TS 103 175 V1.1.1](https://www.etsi.org/deliver/etsi_ts/103100_103199/103175/01.01.01_60/ts_103175v010101p.pdf) |
| DCC_NET / ITS-G5 GN extensions | [ETSI TS 102 636-4-2 V1.4.1](https://www.etsi.org/deliver/etsi_ts/102600_102699/1026360402/01.04.01_60/ts_1026360402v010401p.pdf) |
| GeoNetworking packet format | [ETSI EN 302 636-4-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/3026360401/01.04.01_60/en_3026360401v010401p.pdf) |
| ITS-G5 access | [ETSI EN 302 663 V1.3.1](https://www.etsi.org/deliver/etsi_en/302600_302699/302663/01.03.01_60/en_302663v010301p.pdf) |
| Radio constraints referenced by DCC | [ETSI EN 302 571 V2.1.1](https://www.etsi.org/deliver/etsi_en/302500_302599/302571/02.01.01_60/en_302571v020101p.pdf) |
| CAM generation | [ETSI EN 302 637-2 V1.4.1](https://www.etsi.org/deliver/etsi_en/302600_302699/30263702/01.04.01_60/en_30263702v010401p.pdf) |

The adaptive controller is the reference algorithm for further DCC development.
For command-line compatibility, the default remains reactive; `--limeric` selects
adaptive. DCC_NET is retained as opt-in for local-only experiments. A GN/ITS-G5
profile claiming the associated ETSI DCC architecture must enable NET; having an
experimental switch does not make the conditional normative requirement optional.

A complete Release 2 DCC_FAC allocator is outside this profile. `DccFacilities`
only adapts the channel resource port to the existing CAM service. A future
Release 2 migration must review the related stack standards together.

## Requirement categories and implementation mapping

A **normative requirement** is an applicable mandatory provision, including its
conditions. A **recommendation** (`should`) or **option** (`may`) is recorded as a
choice. **Informative** annexes are examples rather than mandatory algorithms.
**Project policies** must remain identifiable and cannot substitute for checking
applicable normative limits.

| Reference / category | Implementation and verification |
| --- | --- |
| TS 103 175 §§5–6: entity responsibilities and management exchanges, normative/conditional | `DccAccess`, `DccCross`, `DccNetwork`; typed `AccessControl`, `NetworkInput` and `ChannelResources`; architecture and manager integration suites |
| TS 103 175 §6.3: airtime/idle-time exchange | Estimated `TxFeedback` and distinct `estimated_idle_time` in CROSS; resource-unit tests. Actual PHY duration/timestamp needs radio validation |
| TS 102 687 §5.4: adaptive equations and cadence, normative when selected | `DccAdaptive` evaluated by CROSS, paired UTC samples, local/global selection; controller and architecture tests |
| TS 102 687 §5.3: reactive state progression, normative when selected | `DccStateMachine`; transition/persistence tests |
| TS 102 687 Annex A: reactive parameter examples, informative | Current project table is preserved: 50/100/200/400/1000 ms. It is not labelled an exact reproduction of either Annex A example |
| TS 102 687 Annex B: gate example, informative | Shared typed admission gate; conservative estimated enqueue accounting deliberately differs from the example |
| TS 102 636-4-2 §§5–7 and Annex A: NET variables, aggregation, SHB and lifetime | Existing `dccnet::State`, parser/encoder and receiver behind `DccNetwork`; NET tests and NET-to-CROSS integration tests |
| TS 102 636-4-2 §8: TC/UP/AC mapping | Default CAM is TC2 / UP3 / AC_BE. Existing arbitrary `--cam-priority` overrides remain experimental and do not claim the standard mapping |
| EN 302 637-2 §6.1.3: CAM generation | Existing generation policy plus Facilities advice; normal and forced-mode CAM tests. Forced 100 ms is an explicit project experiment |
| TS 103 175 §7.2 and EN 302 571 §4.2.10.2: over-air limits | Independent estimated Ton <= 4 ms, raw-CBR Toff guard and rolling 30 ms/second ledger; boundary and adaptive-transient tests. Actual PHY limits still require radio validation |
| EN 302 636-4-1 §9.5.2.2 and ITS time representation | Shared UTC-to-ITS conversion includes leap-second insertions since the epoch; tests include the ETSI 2007 example and all known insertion boundaries |

## Explicit project choices

- One fixed radio/channel per instance; no channel hopping or MCO allocation.
- Estimated full-frame airtime, accounting after successful driver enqueue;
  no claim of PHY completion, delivery or per-packet measured EIRP.
- Conservative debt is retained on budget changes and not capped at one second.
- Current reactive thresholds, persistence, power and bitrate tables are preserved.
  Adaptive power/bitrate mappings are additional project policies, separate from
  the ETSI delta equations.
- Invalid input closes admission. Adaptive recovery needs a new consecutive pair;
  NET mode does not silently change to local-only control.
- Local survey timing is bracketed by callback entry/exit. A nominal 100 ms
  measurement is accepted only when all possible durations lie in 90–110 ms.
  This tolerance is a project acquisition policy, not an ETSI constant.
- The production deadline is 150 ms from the control observation, also bounded
  by local freshness in NET mode. No radio operation extends that deadline.
- NET's aggregation threshold is `0.62`; the adaptive algorithm target is `0.68`.
  These names remain distinct, as discussed in TS 102 636-4-2 Annex A's note.
- Sparse-neighbour second-maximum convention and SHB field-offset interpretation
  remain documented in the [NET guide](dcc-net.md).

The initial architecture implements the separation and data contracts. It does
not add DENM/CPM generators, full GN routing, secured-GN decapsulation, a complete
multi-service Facilities allocator or hardware certification.
