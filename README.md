# Flow Observatory

Flow Observatory is a vendor-neutral **runtime that observes per-flow lifecycle
and resource consumption** for a fabric OS. It ingests evidence produced by
other systems, reconciles that evidence, and answers questions about flows with
an explicit statement of what is known, what is unknown, and why.

```
C++20 - Apache-2.0 - Copyright 2026 Summon Software Labs - no telemetry
```

## What this runtime owns, and what it does not

Implemented scope, stated as a boundary because the boundary is part of the
contract:

| Owns | Does not own |
| --- | --- |
| Per-flow lifecycle state and its transitions | Scheduling flows |
| Resource-consumption observation and attribution | Choosing paths or routes |
| Multi-source reconciliation and conflict reporting | Rate enforcement or shaping |
| Freshness, staleness and expiry evaluation | Admission or congestion policy |
| Durable, integrity-checked evidence history | Application semantics beyond supplied metadata |
| Deterministic explanations of its own conclusions | Asserting facts it was not given evidence for |

Nothing in the runtime invents a number. A counter that was not supplied is
`null`, not `0`; a delta across an unobserved gap is reported as an unknown
interval, not guessed; a flow whose evidence aged out is `expired`, never
`completed`.

## Quick start

```console
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
$ cmake --build build --parallel
$ ctest --test-dir build --output-on-failure
```

Every command-line example below uses the canonical `FO1` text format, which is
also the format the transport accepts:

```console
$ cat > demo.fo1 <<'EOF'
SRC v=1 key="fabric-a" auth=20 caps=completion,reset,path,queue,counters
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=1 seq=1 flow="tenant-7/flow-1" gen=1 \
      obs=1700000000000000000 kind=open dir=forward ev=complete bytes=0 pkts=0
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=2 seq=2 flow="tenant-7/flow-1" gen=1 \
      obs=1700000001000000000 kind=sample dir=forward ev=complete bytes=4096 pkts=8
EOF
$ foctl apply  --state demo.foj --script demo.fo1
applied=2 duplicates=0 refused=0 sources=1 ticks=0
$ foctl query  --state demo.foj --format text
flow:... key=tenant-7/flow-1 gen=1 state=observed cause=first-observation ...
$ foctl explain --state demo.foj --flow "tenant-7/flow-1"
```

## Implemented capability

### Identities, generations and fencing

Strongly typed, distinct identities for flows, endpoints, paths, links, queues,
sources, epochs, incarnations, generations and revisions. Named identities are
derived from a caller-supplied canonical key with a documented, versioned,
domain-separated hash; the key is retained so explanations are readable. Two
distinct keys mapping to one identity is a detected collision with its own
anomaly, never a silent merge.

Evidence is fenced on five independent axes - epoch, incarnation, generation,
revision and source sequence - plus a monotone source high-water mark. A
superseded path generation cannot support current attribution; a replayed
observation from before a snapshot is refused rather than re-applied.

### Lifecycle

`unknown → observed → active → idle` with distinct terminal states
`completed`, `reset` and `expired`, and a non-terminal `conflicting` state for
equal-authority disagreement.

* **Completion must be proven.** Only an explicit close from a source that holds
  the completion capability, or a terminal counter declaration, produces
  `completed`. `idle` is not completion and `expired` explicitly is not either.
* **A proven termination is durable.** Completion and reset survive staleness and
  restart; liveness and currency do not.
* **A reset never fabricates consumption.** The interval closes at the last
  known value and the boundary is counted as unknown.

### Freshness, provenance and evidence quality

Every observation records what was observed, by which source, for which
generation, at what observation and receive times, under which source
incarnation and epoch, and what the source claims about its own completeness.
Freshness is a pure function of those instants and the policy, recomputed on
every read; it is never stored.

`unknown`, `stale`, `conflicting`, `incomplete` and `unsupported` are five
distinct, separately reported conditions. Absence of evidence is never treated
as positive evidence.

### Multi-source reconciliation

The reconciled view is a **pure function of the accepted observation set, the
policy and the evaluation instant** - not of arrival order, worker thread or how
many times a duplicate was delivered. Sources are ranked by explicit authority
and can be marked advisory; a source cannot grant itself the right to declare
completion by asserting it in a payload. Outcomes are `corroborated`,
`resolved` (a strictly higher authority decided) or `conflicting` (equals
disagree and nobody is above them).

### Attribution

Deterministic, integer-only attribution with three policies: endpoint-only,
uniform across hops, and proportional to source-supplied capacity. Attribution
refuses to publish a number for a generation whose evidence is not fresh or which
has been superseded, and reports `unsupported-by-metadata` rather than guessing
when capacity weights are absent. Shares always sum exactly to the total.

### Persistence

A versioned, append-only, CRC-32C checked journal that is an **observation log
with periodic snapshots**. Recovery replays evidence through the same code path
that produced the live state, so a recovered view is the view the previous run
computed. A corrupt record is always a hard failure; only a torn *tail* may be
dropped, and only when the recovery policy allows it. The journal header records
the format version, identity scheme and the counter policy that participated in
the durable fold.

**A restart does not resurrect liveness.** Restored generations are `observed`
with cause `recovered-from-journal`; they cannot become `active` until fresh
evidence arrives in the current runtime epoch, and restored evidence is
classified `unknown` rather than `fresh`.

### Bounds and concurrency

Every unbounded quantity has an explicit bound in `flowobs::Limits`: flows,
sources, generations, paths, queues, hops, observations per source, lifecycle
history, anomaly history, queue depth, batch size, worker threads, result sets,
journal growth, key and detail lengths, frame size and connections. Every
eviction and every refusal is counted and surfaced as an anomaly. Cancellation
and shutdown are real: `stop_workers` drains the queue before the workers exit
and joins every thread.

### Transport

A loopback TCP ingest/query protocol with CRC-checked framing, a bounded
connection pool and an explicit shutdown request. The test suite starts
`foctl` as an independent operating-system process, drives it over a socket and
then reads the same journal from a second process.

## Proof surfaces

Every claim in this repository is labelled.

### REAL

* The runtime, its tests, its tools and its package. Everything in
  `include/`, `src/`, `tools/` and `tests/` is implemented here and runs here.
* Independent-process transport: `fo_test_transport` creates real child
  processes and exchanges real TCP frames on `127.0.0.1`.
* Install and downstream consumption: `fo_test_downstream` installs the package
  into a scratch prefix, configures a separate CMake project with
  `find_package(FlowObservatory)`, builds it and runs it.
* Persistence integrity, recovery, restart semantics, concurrency, determinism
  and adversarial refusal, all measured by the test suite on this build.

### SYNTHETIC

* The evidence used by the tests, examples and benchmarks. It is generated by
  the runtime's own producers (`FO1` scripts, `tests/support.hpp`) and by seeded
  pseudo-random generators. It is not captured from a real fabric.
* Benchmarks describe **this machine, this compiler and this build**. They are
  throughput and latency measurements of completed work, not capacity claims.

### UNSUPPORTED

Stated plainly, because a reader must not infer more than the code says:

* No switch, ASIC, NIC, RDMA, InfiniBand or NVLink integration exists. There is
  no vendor SDK, no hardware counter read, no P4/SAI/telemetry agent.
* No multi-host distributed deployment exists. The transport is loopback TCP
  between processes on one machine.
* No telemetry source is bundled. Sources must be produced by something else
  and registered explicitly.
* `AttributionPolicy::ProportionalToCapacity` operates only on capacity units the
  source supplied. The runtime never derives capacity from a name or a type.
* Retransmission accounting is reported only when a source supplies it. Where no
  source does, retransmissions are reported as unsupported, never as zero.

## Installing and consuming

```console
$ cmake --install build --prefix /opt/flow-observatory
```

```cmake
find_package(FlowObservatory 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE FlowObservatory::flowobs)
```

The exported target carries the include directories, the C++20 requirement and
the platform socket dependency. `tests/downstream/` is a complete, independent
consumer that is built and run by the test suite against a scratch install.

## Documentation

* [docs/architecture.md](docs/architecture.md) - modules, data flow, layering
* [docs/semantics.md](docs/semantics.md) - identities, lifecycle, freshness, determinism
* [docs/protocol.md](docs/protocol.md) - the FO1 text format and the wire protocol
* [docs/persistence.md](docs/persistence.md) - journal layout and recovery rules
* [docs/concurrency.md](docs/concurrency.md) - lock inventory, ordering audit, shutdown
* [docs/limits.md](docs/limits.md) - every bound and what happens when it is hit
* [docs/validation.md](docs/validation.md) - what the suite proves and how
* [docs/compatibility.md](docs/compatibility.md) - versions and what they fence

## Building with strict warnings

```console
$ cmake -S . -B build-debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug   -DFLOWOBS_WARNINGS_AS_ERRORS=ON
$ cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLOWOBS_WARNINGS_AS_ERRORS=ON
```

On MSVC the first-party warning level is `/W4` with `/WX`; the first-party
warning count is zero. `-DFLOWOBS_ENABLE_ASAN=ON` builds with AddressSanitizer
where the toolchain supports it.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
