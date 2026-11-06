# Headless wire protocol — envelope v1

## Current State

`protocol/headless_protocol.{h,c}` implements a caller-fed request/response codec.
It does not open a socket, invoke the service, authenticate a connection, or execute
an operation. A host can carry these bytes over an injected transport; no production
transport adapter is implemented yet. The envelope is deliberately independent of
the database's persisted layout.

## Byte layout

All integers are unsigned, explicitly little-endian. Unaligned input is supported;
there are no packed-struct casts, pointers, implicit padding, or NUL terminators.

| Offset | Bytes | Field | Contract |
| :--- | ---: | :--- | :--- |
| 0 | 4 | body length | Excludes these four bytes; 12..65532 |
| 4 | 2 | version | Exactly 1 |
| 6 | 2 | kind | 1=request, 2=response |
| 8 | 2 | operation | Nonzero; semantic operation registry remains future work |
| 10 | 2 | status | Request must carry 0; response carries 0..6 |
| 12 | 4 | request ID | Nonzero correlation value, echoed by the eventual dispatcher |
| 16 | variable | payload | Opaque bytes; may be empty or contain NUL |

The 65536-byte total frame maximum is a v1 safety bound, not an initial allocation
capacity or a limit on connections. Each owner borrows 16..65536 writable bytes;
frames larger than its admitted backing reject before copying their bodies. A host
chooses backing on the cold path and may reconstruct the owner between streams.
No allocator, internal queue, thread or wait exists in the codec.

### Response statuses

| Value | Name |
| ---: | :--- |
| 0 | OK |
| 1 | VERSION_MISMATCH |
| 2 | BAD_REQUEST |
| 3 | UNAUTHENTICATED |
| 4 | REQUESTS_DISABLED |
| 5 | UNSUPPORTED_OPERATION |
| 6 | INTERNAL_ERROR |

These are wire vocabulary, not implemented service outcomes. Local codec results
are separately named `HEADLESS_PROTOCOL_*` values: invalid argument, busy, invalid
length, version mismatch, malformed header, backing capacity, truncated EOF, and
closed owner. Unknown operations remain opaque to the codec; a future dispatcher
must reject unsupported operations, not treat nonzero as permission.

## Stream ownership and progress

1. Construct `HeadlessProtocol(backing, capacity)` with live, disjoint storage.
2. Feed receive slices. `outConsumed` is exact; true may mean partial progress,
   not a complete frame. A zero-length slice is not EOF.
3. Stop once `isReady` is true. Any coalesced second frame remains in the host's
   input span. Another feed returns BUSY with zero consumption, preserving payload.
4. Getters borrow the admitted payload until release/reset. The host validates
   operation, session, authorization, gate and payload before executing anything.
5. After sending the corresponding response completely, release the frame and
   feed the retained stream tail. The host, not the codec, owns this send boundary
   and enforces one outstanding request. The codec cannot prove a response was sent.
6. At EOF call `finish`: idle/complete passes, a partial frame fails TRUNCATED.
   The host must still handle delivery failure if a complete request has no reply.

Malformed framing/version/oversize input latches failure. The host closes that
stream; there is no scan-for-next-header resynchronization. Reset explicitly
abandons a stream and scrubs initialized backing; do not silently reset a hostile
live connection. Invalid API arguments and BUSY preserve the admitted frame.
Capacity refusal emits one diagnostic per reset epoch and counts every repeat
(saturating at `UINT64_MAX`, never wrapping);
reset clears the counter. Other terminal faults report once when first detected.
No diagnostic or projection contains payload bytes or addresses.

Encoding validates before writing any destination byte. Failure preserves the
destination and sets `outLength=0`; insufficient destination capacity also sets
`outTruncated=true`. Payload/destination/output spans must be disjoint and live.
All calls are externally serialized. Buffer/geometry/initialized counts are changed
through lifecycle operations, not raw field setters.

## Scope and Limitations

The registered C owner is
`../../../../tests/darkbase-headless/protocol/headless_protocol_test.c`; the independent runner
is `python3 tests/darkbase-headless/protocol_run.py`. Tests exercise the byte oracle,
every split point, byte-at-a-time responses, coalesced tail retention, busy
preservation, all response statuses, header rejection, every incomplete EOF,
exact 64 KiB frame, counted exhaustion/recovery, safe defaults, output canaries,
redacted bounded projections, and unsupported constructor arities. The runner
uses optimized strict C23 and ASan/UBSan with bounded children. These are codec
tests, not actual TCP/TLS or provider tests. Linux/Windows/macOS14 runtime and leak
instrumentation remain unproved.

No TLS, listener/connect loop, send retry/partial-write adapter, deadline,
cancellation, token model, credential payload schema, dispatcher, CRUD or audit is
offered by this slice. Hosts must use bounded nonblocking transport callbacks and
deadlines when attaching it. Credentials must not be sent over an unprotected
transport. The existing service's admin authentication flag is process-local,
never a connection authorization token. No remote admin command is wired here.
