# Concurrency, locking and shutdown

## Lock inventory

| Lock | Rank | Guards | Taken by |
| --- | --- | --- | --- |
| `Engine::Impl::mutex` (`std::shared_mutex`) | 1 (engine) | every field of the runtime: flows, sources, identity registries, journal, epoch, state | shared by all reads, exclusive by all writes |
| `Engine::Impl::queue_mutex` (`std::mutex`) | 2 (queue) | the bounded ingest queue and its condition variables | `enqueue`, worker threads, `drain`, `stats`, `close` |
| `Server::Impl::mutex` (`std::mutex`) | transport | the pending/active connection sets | the transport only |
| `Journal` | none | the journal file | always called with the engine mutex already held |

The transport mutex is a separate hierarchy from the engine's: a transport worker
holds it while moving a socket between the pending and active sets and releases
it before it calls into the engine. The two are never held at the same time.

## Ordering rule

**Rank 1 may be held while rank 2 is taken. Rank 2 is never held while rank 1 is
taken.** A mutex of rank *r* is never taken while a mutex of rank ≥ *r* is held,
which covers both inversion (deadlock) and re-entrant acquisition of the same
rank.

Every acquisition goes through a ranked guard in `src/lockorder.hpp`:

* `EngineSharedGuard` / `EngineMutexGuard` for rank 1,
* `QueueGuard` for rank 2.

Each guard records the acquisition on a thread-local stack and checks the rule.
The check is always compiled in - one thread-local compare per acquisition - and
is exercised by the suite, including a test that deliberately violates the order
and asserts that the checker catches it.

## The one place the engine lock is dropped

`Engine::close()` and `Engine::stop_workers()` must **not** hold the engine mutex
while joining workers: a worker may be blocked waiting to acquire that same
mutex, so joining under the lock is a deadlock. Both functions therefore:

1. take the engine mutex, set the stop flag, release it,
2. notify the queue condition variables,
3. join every worker **without** the engine mutex held,
4. re-take the engine mutex to flush and close the journal and clear state.

This is the only lock-dropping transition in the runtime and it has its own test.

## Worker protocol

Each worker loops:

1. take the queue mutex and wait until the queue is non-empty or the stop flag
   is set,
2. move up to `max_ingest_batch` observations into a local batch and release the
   queue mutex,
3. take the engine mutex exclusively and apply the whole batch,
4. release it and repeat.

Exiting: when the stop flag is set the worker keeps draining until the queue is
empty and only then returns. `stop_workers()` is therefore a real completion
barrier - every enqueued observation has been applied by the time it returns -
and it never depends on a timeout.

## Cancellation

`Engine::freeze()` sets an atomic flag. Every mutating entry point then returns
`shutting-down` while reads keep working, which lets an operator stop a
misbehaving producer without dropping the ability to inspect what happened.

`Server::stop()` sets an atomic flag, closes the listening socket and shuts down
every active connection socket, which makes the blocking reads in the connection
handlers return. `Server::join()` then joins the accept thread and every
connection thread.

## Audit result

* Locks in the runtime: 2 (engine, queue) plus the transport's own.
* Nesting sites: 2 - `Engine::stats()` and the worker batch apply both take
  engine-then-queue.
* Lock-order violations observed by the runtime's own checker across the whole
  suite: **0**.
* Re-entrant acquisitions: **0**.
* Locks held across a thread join: **0**.
* Locks held across a blocking socket read: **0** (the transport mutex is always
  released before I/O).

The suite includes a multi-threaded concurrency test (producers, workers and
readers running together) and a contention test where six threads exercise
ingest, query, stats and drain simultaneously; both assert zero lock-order
violations at the end.
