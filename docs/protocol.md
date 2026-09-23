# Protocol

Two formats are defined here: the canonical `FO1` text format used for scripts,
CLI input and human-readable evidence, and the binary framing used by the
loopback transport.

## FO1 text format (version 1)

One record per line. Tokens are separated by single spaces. Values that can
contain spaces are double quoted with `\"` and `\\` escapes. Empty lines and
lines whose first non-space character is `#` are comments. **Unknown tokens are
rejected**: the parser fails closed.

```
SRC  v=1 key="<key>" auth=<u32> [caps=<a,b,...>] [advisory=0|1]
OBS  v=1 src=<id> flow=<id> gen=<u64> inc=<u64> epoch=<u64> rev=<u64> seq=<u64>
         obs=<ns> [rx=<ns>] kind=<name> dir=<name> ev=<name>
         [ep=<id>] [peer=<id>] [path=<id>] [queue=<id>] [link=<id>]
         [pathgen=<u64>] [hopcap=<u64>,...]
         [bytes=<u64>|-] [pkts=<u64>|-] [retx=<u64>|-] [dur=<ns>] [rtt=<ns>]
         [caps=<a,b,...>] [schema=<u32>]
AT   v=1 now=<ns>
TICK v=1 [now=<ns>]
FREEZE
```

`<id>` is either a canonical key in double quotes or a raw identity written as
`@` followed by exactly sixteen hexadecimal digits. The raw form is what `foctl`
prints for evidence whose original key was never declared; it round trips
exactly.

`-` means "the source did not supply this counter". It is never read as zero.

Capability names: `completion`, `reset`, `path`, `queue`, `counters`,
`identity`, or `none`.

Observation kinds: `unknown`, `open`, `sample`, `progress`, `idle`, `close`,
`reset`, `error`, `path-change`, `counter-reset`, `heartbeat`.

Directions: `unspecified`, `forward`, `reverse`, `aggregate`.

Evidence classes: `unknown`, `complete`, `incomplete`, `unsupported`.

`AT` moves a caller-supplied clock. It is how a script pins the evaluation
instant so that freshness decisions are reproducible. `TICK` applies the expiry
policy at an instant. `FREEZE` refuses all further writes.

### Example

```
SRC v=1 key="fabric-a" auth=20 caps=completion,reset,path,queue,counters
AT  v=1 now=1700000000000000000
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=1 seq=1 flow="tenant-7/flow-1" gen=1 \
      obs=1700000000000000000 kind=open dir=forward ev=complete \
      ep="host-a:443" peer="host-b:9000" path="p0" queue="q0" bytes=0 pkts=0
TICK v=1 now=1700000003000000000
```

## Wire protocol (version 1)

Loopback TCP. Little endian. A frame is:

```
magic    'F' 'O' 'I' '1'      4 bytes
version  u16                  kWireProtocolVersion
kind     u16                  MessageKind
length   u32                  payload byte count
crc      u32                  CRC-32C over kind, length and payload
payload  length bytes
```

A frame whose magic, version, length or checksum is wrong is a protocol error and
closes the connection. The transport never resynchronises by scanning, because
scanning is how a corrupt stream turns into a plausible one.

| Kind | Direction | Payload |
| --- | --- | --- |
| `hello` (1) | request | empty |
| `welcome` (2) | reply | `flow-observatory/<version>` |
| `ingest` (3) | request | FO1 records |
| `ingest-ack` (4) | reply | `applied=.. duplicates=.. refused=.. ticks=.. sources=..` |
| `query` (5) | request | `limit=.. offset=.. [flow=<id>] [state=<name>] [only_live=1] [pretty=1] [include_generations=1]` |
| `query-result` (6) | reply | the JSON export |
| `explain` (7) | request | `flow=<id>` |
| `explain-result` (8) | reply | the deterministic explanation |
| `history` (9) | request | `flow=<id> [limit=N]` |
| `history-result` (10) | reply | JSON |
| `attribution` (11) | request | `flow=<id> [generation=N]` |
| `attribution-result` (12) | reply | JSON |
| `stats-request` (13) | request | empty |
| `stats-result` (14) | reply | `key=value` lines |
| `tick` (15) | request | `[now=<ns>]` |
| `tick-result` (16) | reply | `evaluated_at=.. examined=.. transitions=..` |
| `bye` (17) | request | empty, or `shutdown` |
| `error` (18) | reply | human-readable reason |

A connection may carry any number of requests. A malformed frame produces an
`error` reply and closes the connection; the server stays healthy and keeps
serving other connections.

`bye shutdown` stops the listener. It is only reachable over a loopback
connection the operator established.

## Determinism of the transport

The transport is a delivery mechanism, not a source of policy. It adds nothing
to an observation and changes nothing about how one is judged. An observation
delivered over a socket produces exactly the state that the same observation
delivered in-process produces; `fo_test_transport` asserts that by comparing a
socket-driven process against a file-driven one.
