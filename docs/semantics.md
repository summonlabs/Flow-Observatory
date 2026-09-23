# Semantics

This document is normative for the behaviour the tests assert.

## Identities

Named identities (flow, endpoint, path, link, queue, source) are 64-bit values
derived from a caller-supplied canonical key:

```
id = FNV1a64(domain || 0x00 || key)   with 0 mapped to 1
```

`domain` is a fixed string per identity family (`"flow"`, `"endpoint"`, `"path"`,
`"link"`, `"queue"`, `"source"`). Zero is reserved for "invalid", so a
successfully constructed identity is never invalid. The scheme version is
`kIdentitySchemeVersion` and is recorded in the journal header and the wire
handshake.

Numeric identities (epoch, incarnation, generation, revision, sequence) are
opaque counters whose domain belongs to the producer; the runtime only fences
them.

**Collisions.** Two distinct keys that hash to one identity are a detected
condition, not a merge. The runtime proves `hash(key) == id` before it looks
anything up, and an identity already registered under a different key produces an
`identity-collision` anomaly and a refusal. The collision path itself is covered
by a direct test of the identity registry, because a 64-bit collision cannot be
constructed on demand.

## Observation identity

An observation's *key* is `(source, incarnation, epoch, revision, sequence)`.

An observation's *evidence identity* is the digest of everything the source
declared - subject, kind, direction, evidence class, counters, duration, RTT,
capability set, schema version and observation instant - **excluding the instant
this runtime received the record**. Rationale: the receive instant is runtime
bookkeeping, and including it would make a redelivery with a new local timestamp
look like a different record. Excluding it gives two properties:

* a redelivery is idempotent, and
* a redelivery cannot refresh the age of the evidence, because the retained copy
  keeps the receive instant of the first acceptance.

When two records share a key but differ in evidence identity, the runtime retains
the copy with the **smaller** digest and refuses the other with
`evidence-conflict`. The smaller-digest rule is what makes the outcome
independent of arrival order; which copy wins does not depend on which arrived
first.

## Lifecycle

```
unknown --first evidence--> observed --progress--> active <--> idle
                                |                    |         |
                                +--------------------+---------+
                                                     |
                              completed / reset / expired (terminal)
                              conflicting (non-terminal)
```

* `observed`: evidence exists but carries no current progress.
* `active`: fresh, reconfirmed evidence inside the idle window shows progress.
* `idle`: no progress within `expiry.idle_after`. Not completion.
* `completed`: a source holding the completion capability declared a close, or
  declared terminal counters. Only this state asserts completion.
* `reset`: a source holding the reset capability declared a reset, or a
  regression was treated as one.
* `expired`: the expiry policy closed the generation because evidence aged out.
  Explicitly *not* completion.
* `conflicting`: two usable sources of equal authority disagree and no strictly
  higher authority exists above them.

Terminal states are never reopened. A new path generation is a new generation,
not a reopening of the old one.

Terminal states are *durable facts*: a proven completion or reset keeps its force
after the evidence goes stale and across a restart. Liveness and currency do not -
they require fresh, reconfirmed evidence in the current runtime epoch.

State advances when accepted evidence for that flow arrives, or on an explicit
`Engine::tick(instant)`. The runtime never advances state from an implicit clock
read, which is what makes it reproducible.

## Freshness

`classify_freshness(observed_at, received_at, context)`:

1. evidence carried over from a previous runtime epoch and not yet reconfirmed is
   `unknown` (when `require_reconfirmation_after_restart`, the default);
2. a missing observation instant is `unknown`;
3. an observation instant in the future relative to the receive instant is
   clamped to the receive instant;
4. if the evaluation instant is before the evidence, the classification is
   `unknown` - the runtime refuses to call evidence fresh for an instant that
   precedes it;
5. otherwise `fresh` within `fresh_window`, `stale` within `expire_window`,
   `expired` beyond it.

Freshness is recomputed on every read and is never persisted.

## Counters

Counters are optional. Absent is not zero.

* The first observed value of an interval establishes a baseline and contributes
  no consumption - a single sample is not a measurement of a delta.
* A value above the previous one contributes the difference, with checked
  arithmetic that saturates and flags rather than wrapping.
* A value below the previous one closes the interval, opens a new one at the new
  value, counts an **unknown interval**, and contributes nothing. The
  consumption before the regression is kept; the consumption across it is not
  invented.
* An incarnation change always breaks the interval: the producing process
  restarted and the runtime refuses to bridge the gap.
* A restart of the runtime itself is not a gap when the journal is replayed,
  because every accepted observation is rebuilt; it is a gap when a snapshot is
  restored, because the observations behind the snapshot are gone.

## Reconciliation

Sources are sorted by authority descending, then identity ascending. A source is
*usable* when it is not advisory, its freshness is `fresh`, and it has been
reconfirmed in the current runtime epoch.

* No two usable sources disagree → `corroborated`.
* Every disagreement is settled (one side has strictly higher authority, or a
  third source is strictly above both) → `resolved`.
* Some disagreement is between equals with nobody above them → `conflicting`,
  the flow enters the `conflicting` state, and attribution refuses to publish a
  number.

Totals come from the primary (highest-authority) usable source under the default
policy, or from the maximum/minimum over usable sources under the two other
policies. Durable accounting is still reported when nothing is usable; only its
*currency* is denied.

## Attribution

`endpoint-only` attributes to the flow and its two endpoints.
`uniform-across-hops` distributes the total across hop records with the integer
remainder assigned by largest-remainder, ties broken by index, so shares always
sum exactly to the total. `proportional-to-capacity` uses capacity units the
source supplied and reports `unsupported-by-metadata` when any hop lacks them.

Attribution refuses with `stale-generation` when the generation is not fresh and
with `not-current-generation` when a newer generation exists. Stale path
generations cannot support current attribution.

## Bounds

See [docs/limits.md](limits.md). Every bound is compiled in, reported by
`foctl limits`, recorded in the journal and enforced with a counted refusal or a
counted eviction.

## Determinism contract

For a fixed policy and a fixed evaluation instant, the current view - flows,
generations, sources, states, coverage, consistency, totals, correlation - is a
pure function of the accepted observation set. It does not depend on arrival
order, on the number of worker threads, or on how many times evidence was
redelivered.

Excluded from the contract, and documented as order-dependent by construction:
the per-flow ingest anomaly log, and the lifecycle transition log. Both describe
the path taken to the view; both are exported, bounded and counted.
