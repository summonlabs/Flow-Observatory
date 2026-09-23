# Compatibility and versions

Every versioned surface is listed here with what it fences.

| Constant | Value | Meaning |
| --- | --- | --- |
| `kVersionMajor/Minor/Patch` | 1.0.0 | library version |
| `kApiVersion` | 1 | source/binary interface revision of the public C++ surface |
| `kJournalFormatVersion` | 1 | on-disk journal format |
| `kJournalMinReadableVersion` | 1 | oldest journal this build reads |
| `kIdentitySchemeVersion` | 1 | identity hash construction, recorded in every journal header |
| `kWireProtocolVersion` | 1 | loopback frame protocol |
| `kTextObservationVersion` | 1 | FO1 text format |
| `kObservationSchemaVersion` | 1 | observation schema accepted by the engine |

## What each version fences

* **Journal format.** A journal outside
  `[kJournalMinReadableVersion, kJournalFormatVersion]` is refused with
  `version-mismatch`; it is never reinterpreted.
* **Identity scheme.** A journal written under a different identity scheme is
  refused rather than having its identifiers silently reinterpreted.
* **Observation schema.** An observation whose `schema_version` differs from
  `kObservationSchemaVersion` is refused with `version-mismatch`.
* **Wire protocol.** A frame with a different version is refused with
  `version-mismatch` and closes the connection.
* **Counter policy.** The only policy that participates in the durable fold is
  the counter policy; its digest is recorded in the snapshot and a snapshot
  written under a different one is refused. Every other policy is operational and
  may change between runs without invalidating persisted evidence.
* **API.** `flowobs::compatible_with(api_version)` answers whether a component
  compiled against a given `kApiVersion` interoperates with this runtime.

## Forward and backward rules

* New enumerators are appended; parsing an unknown enumerator fails closed.
* New record kinds are appended and an unknown kind is a hard failure, so an
  older reader refuses a newer journal rather than skipping records it does not
  understand.
* New FO1 tokens are rejected by an older parser. Extending the format therefore
  requires a version bump.
* Policy defaults may change only with a minor version bump, and any change that
  alters the durable fold requires a counter-policy digest change.

## Identities across processes

The identity hash is fully specified and versioned, so an independently written
producer can compute the same identities from the same keys. `foctl` accepts
either the canonical key or the raw `@`-prefixed identity, which is what makes
cross-process and cross-version round trips exact.
