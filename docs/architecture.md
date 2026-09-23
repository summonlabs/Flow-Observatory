# Architecture

## Scope

Flow Observatory observes. It does not schedule, route, police or admit. The
documents and headers state that boundary in the same words so that a reader
never has to guess which half of a fabric OS they are looking at.

## Modules

```
include/flowobs/          public surface (installed)
  version identity time checked checksum clock limits error export
  semantics counters observation policy
  flow history reconcile attribution
  persistence stats engine
  textproto transport export_json flowobs  (umbrella)

src/                      implementation
  identity time checked? limits error version checksum clock
  semantics counters policy observation flow history
  reconcile            bounded per-source ledger and the pure folds
  attribution          deterministic resource attribution
  persistence          journal, state codec, replay
  engine engine_api engine_impl  runtime state and public entry points
  query explain export_json      deterministic rendering
  textproto transport            FO1 and the loopback wire protocol
  lockorder                      deadlock and re-entrancy audit
  binary obs_codec render        internal, not installed
```

## Layering

```
        tools/foctl        examples        benchmarks        tests
             \                |               |              /
              +---------------+--------------+-------------+
                              |
                        public API (include/flowobs)
                              |
   +--------------------------+---------------------------+
   |                          |                           |
 engine  ------------------>  reconcile  ----------->   semantics
   |            pure folds         |                   counters
   |                               v                   observation
   +--> persistence (journal + codec)                  identity/time
   |
   +--> transport (loopback TCP)  <-- textproto (FO1)
```

Nothing below the public API reaches upward. `render.hpp`, `binary.hpp`,
`obs_codec.hpp`, `engine_impl.hpp` and `lockorder.hpp` are internal headers;
they are not installed and are only reachable from the build tree.

## Data flow

1. Evidence arrives as an `Observation` - from a caller, from an FO1 script or
   from the transport.
2. The engine validates it structurally and proves that every claimed identity
   matches its canonical key.
3. Accepted evidence is appended to the journal *before* it is applied, so a
   journal failure is a clean refusal rather than a divergence between memory
   and disk.
4. The observation is inserted into the bounded per-source ledger for its
   (flow, generation, source).
5. The ledger is folded, in canonical order, into a `SourceView`.
6. Source views are reconciled, in authority order, into a `GenerationView`.
7. A pure state machine turns the generation view into a lifecycle decision, and
   the transition is recorded in the bounded per-flow history.
8. Queries, explanations, attributions and exports are pure functions of the
   above plus the evaluation instant.

## Why the fold is a pure function of the accepted set

The ledger keeps its entries in canonical order (incarnation, epoch, revision,
sequence) and folds them from the lowest upward. When the ledger reaches its
capacity the *lowest* entries are folded into a bounded prefix summary rather
than discarded. Folding a prefix of the canonical order and then folding the
remainder is exactly the fold of the whole set, so:

* capacity changes do not change the accounting,
* arrival order does not change the accounting, and
* a duplicate that is recognised costs nothing.

Evidence that falls *inside* the already-folded region - below the folded
high-water mark - cannot be re-folded, so it is either recognised exactly (when
it lands in the 64-wide dedupe window over the source sequence) or refused with
an explicit `stale-revision` refusal. It is never applied out of order.

## Determinism

Two runtimes with equal policy fed the same evidence in any order produce
byte-identical exports and identical `Engine::state_digest()` values. The digest
covers the current evidence-derived view; it deliberately excludes the ingest
anomaly log and the lifecycle transition log, which describe the *path* the
runtime took and therefore legitimately depend on arrival order. Both logs are
still exported; they are simply not part of the determinism contract.

## Concurrency shape

One engine mutex guards all runtime state, taken shared for reads and exclusive
for writes. One queue mutex guards the bounded ingest queue. The engine mutex may
be held while the queue mutex is taken and never the other way round; every
acquisition goes through a ranked guard that asserts this at run time. See
[docs/concurrency.md](concurrency.md).

## Failure philosophy

Every condition that means "I saw something and chose not to trust it" is a
counted, named anomaly attached to the flow. Nothing is dropped silently: the
number of anomalies that could not be retained is itself reported. Limits refuse
rather than evict, except where eviction is explicitly defined and counted.
