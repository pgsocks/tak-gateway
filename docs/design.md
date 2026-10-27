# TAK gateway design rationale

This design is dated October 22, 2026. The public contract is in
[api.md](api.md); protocol evidence and its limits are in [research.md](research.md).
Public declarations and contract tests define the implementation's behavior.
The foreground runtime composes framing, sessions, normalization, bounded
fan-out, and nonblocking transport adapters. Verification boundaries are in
[testing.md](testing.md).

## Purpose and invariants

The gateway aggregates live CoT events from independent TAK Server TLS
connections and UDP listeners, including multicast, for applications that
consume XML over TCP, UDP, or a named pipe. Networking, TAK authentication,
session negotiation, and Protobuf conversion belong inside the gateway.
Normal operation needs no browser, WebTAK installation, or REST account.
Server provisioning and enrollment remain administrative tasks.

The design requires these observable properties:

1. Each downstream payload is one complete UTF-8 CoT `<event>` document.
   Authentication, negotiation controls, TAK framing, and binary messages
   never become event output.
2. Accepted incoming XML retains its document bytes, including unknown
   attributes and detail extensions. Conversion through a restricted object
   model must not remove content from the XML path.
3. Protobuf conversion either represents the event, explicitly preserves
   unmapped bytes, or rejects it. It never silently invents or drops data.
4. Sources have separate credentials, session state, buffers, and reconnect
   schedules. Failure of one source must not restart healthy sources.
5. Upstream data is never forwarded to another upstream. Upstream writes are
   restricted to configured authentication, negotiation, maintenance, and
   optional identity announcements.
6. Admission offers each complete event to every enabled output, preserving
   per-source order and a common aggregate order. Duplicates, repeated UIDs,
   stale timestamps, and unfamiliar application types remain events.
7. Buffering is finite, and local rejection, queue loss, and delivery failure
   are observable. Delivery is live and best effort, without durable replay
   or an end-to-end processing acknowledgment.

The initial deployment is Linux, either foreground, supervised, or in a
Linux container. Native Windows includes a byte-mode named-pipe adapter.
Platform-specific deployment tests are required before claiming support.

## Transport scope and boundaries

| Input | Selected behavior | Server setting or deployment |
| --- | --- | --- |
| UDP | One XML document or TAK v1 mesh frame per datagram | Static server output or other sender |
| Multicast UDP | Explicit group/interface, passive reception | `mcast`, XML-only `cotmcast`, client broadcasts |
| TLS, negotiated | Start in XML; optionally negotiate v1 | `tls` |
| TLS, XML only | XML for the session | `cottls` or compatible mixed endpoint |
| TLS, Protobuf only | Fixed v1 stream after required authentication | `prototls` |

These distinctions appear in the [server transport mapping][transports].
Port 8089 is a default, not evidence of an encoding or authentication mode.
A UDP bind creates no authenticated server subscription: the server must
already send to that address or multicast group. Optional plaintext XML TCP
compatibility is explicit, never a TLS downgrade, and cannot send passwords.
gRPC, WebSocket, federation, QUIC, REST polling, and proprietary encrypted
mesh protocols are outside this ingress contract.

The architecture separates independently testable responsibilities:

```text
configuration / source supervisor / transport adapters
                         |
                  framing + session controls
                         |
               XML preservation / Protobuf normalization
                         |
                  ordered bounded fan-out
                         |
             TCP / UDP / platform pipe adapters
```

The low-level interfaces describe framing, complete-payload normalization,
session transitions, and record delivery without owning a socket. The
higher-level runtime composes those operations and owns configured network
resources. This permits deterministic tests with supplied bytes and time,
while retaining one model for real sessions. It avoids coupling codec tests
to certificates, DNS, real-time sleeps, or a running server.

The [repository style](../CONTRIBUTING.md) requires C11 public interfaces,
procedural C++17, opaque handles, plain data, explicit callbacks, and status
errors. Ownership, borrowed-view lifetimes, output state after failure, and
per-handle caller synchronization are part of [the contract](api.md).
No released binary ABI is implied. Dependencies must follow CPM policy;
candidate parsers or TLS libraries are not dependencies merely because
their APIs informed this design.

## Framing without losing XML

TCP, TLS, and POSIX FIFOs provide byte streams. Neither a socket read nor a
TLS record marks an application event boundary. Keep bounded incremental
framing state per source and connection generation. Reconnect discards
incomplete old-session data.

For XML, use structural recognition while retaining the original byte span.
Recognize declarations, split UTF-8, attributes, nested elements, comments,
CDATA, and namespace declarations. A literal `</event>` search, although
described in the traditional TAK protocol, would split comments or CDATA
incorrectly. A DOM round trip would change lexical content. Incremental
recognition costs parser state plus a bounded retained document; preserving
the original bytes costs a buffer that a model-only converter could avoid.
Expat's incremental C API, byte offsets, and caller-state callbacks are useful
[prior art](research.md#parser-and-native-api-prior-art).

Validate one CoT 2.0 event with its required core attributes and point,
without treating unknown attributes or extension elements as an error merely
because the codec cannot model them. Retain the optional XML declaration and
the document through the closing root. The framer retains a BOM and discards
inter-document whitespace; datagram decoding retains the whole XML datagram.
Reject DTDs and entity declarations, external resource
loading, invalid UTF-8, excessive depth, and excessive size. Predefined and
numeric XML entities remain ordinary XML. Malformed stream input fails that
session; scanning indefinitely for another apparent event is ambiguous and
can consume unbounded resources. Datagram errors reject only that datagram.

TAK framing has two distinct layouts, established by [the protocol][protocol]:

```text
mesh:    BF  unsigned-varint(version=1)  BF  TakMessage
stream:  BF  unsigned-varint(payload-byte-count)  TakMessage
```

The mesh varint is a version. Datagram length supplies its boundary. Never
join datagrams or accept an operating-system-truncated receive. Auto mode
recognizes valid XML starts or a valid TAK header; arbitrary non-XML bytes
are not presumed Protobuf. Reject unsupported versions, malformed magic,
concatenated XML documents, and trailing unrelated bytes.

The stream version comes from the session. Parse a length incrementally,
limit varint work to ten bytes, reject overflow, and check the message limit
before allocation. One read may finish several frames or only part of a
header. Framing does not make a malformed Protobuf payload acceptable.

Multicast joins require explicit receiving interfaces; IPv6 requires scope.
Where groups share a port, use destination/interface metadata or reject an
ambiguous configuration. `SO_REUSEPORT` is not broadcast fan-out. Passive
reception avoids advertising a fictitious client location or changing peers'
encoding choices. Full transmitting mesh-client behavior would require the
protocol's capability-announcement state machine and is a separate feature.

## Authentication and protocol state

TLS authentication and TAK streaming authentication solve separate problems.
Verify the server chain and expected hostname/identity, with no automatic
certificate bypass. Support PEM and protected PKCS#12 client credentials.
Where configured, the XML username/password/UID/callsign preamble is the first
client application write, including before a negotiation request. A profile
may require both mechanisms. Merely sending the preamble does not establish
an authentication-success acknowledgment that the protocol does not define.

The connection supervisor owns connect, TLS handshake, disconnect, backoff,
and DNS re-resolution. Session processing owns the application encoding:

```text
required authentication submitted
        |
        +-- fixed XML --------------------------> XML active
        +-- fixed Protobuf v1 ------------------> Protobuf active
        +-- negotiated --> XML active --> request pending
                                 ^           /         |
                                 |      rejection   acceptance
                                 +----------+          |
                                                Protobuf active

pending timeout / malformed stream --> close --> independent reconnect
```

Negotiated mode defaults to preferring XML to preserve arbitrary XML content.
Protobuf remains a required supported mode. Request only advertised supported
versions, reusing the server's transaction UID. After the request, suspend
client application writes, including XML heartbeats, but continue forwarding
ordinary received XML events. Wait at least 60 seconds; the selected default
is 65 seconds. Rejection returns to XML. Timeout closes because the encoding
is indeterminate. Acceptance switches both directions at the exact end of
the response; remaining bytes in that same read immediately enter the
Protobuf framer. These rules come from [the protocol][protocol].

Fixed modes do not initiate negotiation. Their required credential preamble
is separate from event framing and needs endpoint-specific interoperability
tests. A source UID remains stable across reconnects. TCP keepalive is
supported; TAK ping/pong and identity/SA messages are explicit settings,
disabled by default. One connection-scoped ping may be outstanding; the
official server's pong has UID `takPong` and does not echo the request UID.
Its response deadline begins when the final ping byte is submitted. Supply
monotonic time for intervals/deadlines and separate caller-provided UTC time
for generated event timestamps; the session has no hidden clock read.
Maintenance pauses during upgrades and never becomes downstream event data.

Optional identity XML is copied from an explicit caller-supplied event whose
UID matches the source. Send it once after credentials and before negotiation;
fixed Protobuf mode requires a lossless conversion. No location `(0, 0)` is
invented as a gateway identity. Do not advertise detail-extension decoding
without a corresponding tested decoder.

Each reconnect resets authentication, negotiation, and framing. Exponential
backoff with jitter uses bounds of 1–60 seconds and resets after a stable
session, rather than causing synchronized reconnect bursts.

## Loss-aware Protobuf normalization

[`pgsocks/cot`][codec] is a complete-message codec, not a TAK transport stack.
Its inspected contract rejects any `takControl`, including a combined
control/event envelope. Its XML interface also rejects some unknown core
attributes. Using it on all ingress would break the gateway's preservation
requirement, so incoming XML uses the independent preservation path.

For Protobuf, inspect the top-level envelope first. Control-only messages
update relevant state without output. Event-only messages normalize. Combined
envelopes process control and still normalize their event. Empty envelopes
and malformed wire data fail. XML control recognition requires the expected
message type and structure; negotiation responses also match the active
transaction. Unrelated `t-*` application events remain application events.

Remove recognized control fields using wire-aware slicing, preserving all
other original fields, ordering, and repeated singular-message fragments.
Rebuilding a generated model can change merge behavior or discard unknown
bytes. The pinned [server schema][server-envelope] has top-level
`submissionTime` and `creationTime` absent from the codec's schema; preserving
only field 2's `cotEvent` bytes would already lose real server data.

Decode the event-only envelope through `cot_decode_ex`, inspect unknown-field
diagnostics and detail conflicts/extensions, then encode mapped content
through `cot_encode_ex`. Keep decoded data immutable. The codec maps typed
details and residual `xmlDetail`, but unknown fields, binary detail
extensions, and typed/residual conflicts cannot all be represented completely
as standard XML. Two policies make that limit explicit:

| Policy | Result when detail or unknown envelope data cannot be mapped |
| --- | --- |
| `preserve-opaque` (default) | Emit valid mapped XML plus the exact event-only envelope in a preservation element |
| `reject` | Emit nothing and report incomplete conversion |

The gateway-defined preservation element under `<detail>` uses namespace
`urn:tak-gateway:preservation:1`, local name `protobuf`,
`protocolVersion="1"`, `encoding="base64"`, and
`reason="unmapped-fields"`. Match the namespace URI; an existing `gw` prefix
may already mean something else. Its body is base64 of the exact event-only
envelope, without transport headers or recognized controls. This is not an
official TAK extension and does not claim to decode unknown semantics.
Ordinary XML consumers can read mapped fields; informed consumers can recover
the bytes. Base64 expansion and the extra XML count toward the output limit.

A temporary codec view may omit unsupported binary details only when the
original bytes are preserved this way. Never mutate the decoded owner. Under
either policy, unrepresentable core values reject the event: do not replace
UIDs, timestamps, coordinates, or types to manufacture valid XML. A future
registered detail decoder can provide a semantic mapping without changing
the default XML output framing.

## Admission, fan-out, and record delivery

Admit completed normalization results fairly across sources. Preserve stream
session order and receiver-observed UDP order, without claiming sender order
for UDP or global causality across servers. Parallel normalization needs a
per-source sequencing barrier. An internal aggregate sequence defines the
same order at all accepting outputs; it is not inserted into the XML.

Forward duplicates and repeated UIDs. UID-based suppression would discard
legitimate updates, and timestamp sorting would reorder live traffic.
Persistent event state, snapshots for new clients, deduplication, and replay
are outside this design.

UDP sends one complete XML document per datagram, with no TAK header or
gateway fragmentation. The portable configured ceiling is 65,507 bytes and
may be reduced for path MTU. Oversize rejects only that output. IP
fragmentation or a successful send does not establish consumer receipt.

TCP supports listener clients and outbound collector connections. POSIX FIFO
and Windows byte-mode named pipes use the same default stream record:

```text
4-byte unsigned big-endian XML byte count N | exactly N XML bytes
```

Zero length is invalid. The length excludes its header; there is no NUL,
newline, banner, or source envelope. Compared with newline delimiting, this
works with formatted XML and gives consumers simple bounded reads. Optional
`xml-document` framing emits consecutive XML documents for consumers that
already parse structural boundaries. Framing is selected out of band.

Keep a write offset across short writes and never interleave records. A
failed stream discards its unfinished record; a new stream never starts with
the old record's suffix. Consumers discard incomplete records at EOF.
Listener clients receive future events and have independent bounded queues.
Downstream writes cannot inject events into upstreams.

A POSIX FIFO has one writer and one reader. Multiple readers divide bytes,
so independent consumers need TCP fan-out. Reject symlinks, ordinary files,
and incompatible existing objects at the FIFO path. Opening and writing
cannot block upstream processing. No reader and `EPIPE` are output failures;
handle `SIGPIPE` without terminating unrelated work. Reader replacement on
an already-open FIFO is unsupported. Windows uses restricted local pipe
permissions and asynchronous byte-mode I/O; Linux containers expose FIFOs
through a mounted directory, not native Windows pipe paths.

## Finite resources and failure policy

| Setting | Selected default |
| --- | ---: |
| Maximum input frame / output XML document | 8 MiB each |
| XML nesting depth | 128 |
| Retained-message budget | 256 MiB |
| Queue per output/client | 64 MiB and 4,096 records |
| Configured inputs / clients per TCP listener | 32 / 32 |
| Low-level delivery engine live-output cap | 1,024 |
| Connect / TLS handshake timeout | 10 seconds each |
| Partial-message timeout | 30 seconds |
| Negotiation-response timeout | 65 seconds |
| Graceful output drain deadline | 5 seconds |

These are gateway choices, not TAK requirements. Apply byte and record
limits together; many small records can exhaust bookkeeping before bytes.
Shared immutable message storage reduces copies, but slow-consumer references
still retain memory and count against the global budget. Bound parser work,
temporary conversion storage, nesting, extension expansion, and accepted
clients as well. The retained-message limit is not a claim that process RSS,
TLS-library state, or kernel socket buffers equal that number.

Default `drop-newest` rejects a complete newly offered record for the full
queue, increments counters, and leaves healthy sinks progressing. It never
evicts bytes from a partially written record. A stalled writer closes its
stream to recover framing. Optional `overflow="fail"` makes local queue loss
a fatal runtime failure; it cannot recover traffic missed during disconnects
or guarantee UDP delivery. The final XML size is checked after conversion
and preservation expansion. Error capture is off by default and, if enabled,
requires separate byte/file quotas, restrictive permissions, and retention.

## Configuration, diagnostics, and deployment

The executable separates offline configuration from running:

```sh
tak-gateway config --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 --web-bind 127.0.0.1:8080
tak-gateway validate
tak-gateway redact
tak-gateway run
```

A private persistent store is shared by CLI configuration and the optional
[web manager](web.md). An explicit TOML import supplies a gateway base; ZIP
inputs append and subsequent setters override it in order. Running reads the
saved snapshot instead of repeating arguments. See the [CLI contract](cli.md).
Unknown keys, invalid modes/ports, incompatible authentication/framing and
impossible budgets remain rejected. Managed first setup permits empty endpoint
collections; public library parsing retains its strict requirement. The owner
serializes live endpoint edits, preserving unrelated sockets and queues.
Global setting changes occur offline. The Docker entrypoint seeds optional
environment settings only for an uninitialized persistent store.

Passwords and key passphrases use protected files or named environment
references, with credential-file permission checks. Do not accept secret
literals in URLs or disclose values in errors, redacted output, status, or
normal logs. Errors identify keys and structured categories. Default output
addresses are loopback; downstream plaintext does not inherit upstream TLS
protection. Combining feeds combines their authorized data, so this gateway
is not an accredited cross-domain guard. Preserve releasability markings and
use separate instances for data that must remain separate.

Logs go to stderr, never event output. Per-source status distinguishes
transport, credentials submitted, protocol progress, reconnects, receive
times, controls, and rejects. Conversion status distinguishes preservation
fallbacks, unsupported extensions, and invalid cores. Output status records
availability, clients, queue usage, completed writes, drops, UDP oversize,
and write failures. Periodic status snapshots are atomically replaced even
while traffic is quiet. Process liveness, initialized local endpoints, and
remote-source availability are distinct; an idle multicast group is not
proof of failure.

Exit 0 for validation success or clean termination, 2 for usage/configuration
errors, and 3 for fatal local runtime failures. Temporary upstream failures
keep the process running. `SIGINT`/`SIGTERM` stop intake, leave memberships,
close upstreams, drain complete output records until the deadline, and report
remaining losses. The application runs in the foreground. A supervisor such
as systemd runs that same command; an internal daemon mode would duplicate
lifecycle behavior and complicate signal handling.

The gateway image must contain the executable/runtime libraries, use a
non-root identity and read-only root filesystem, receive credentials through
mounts, and execute the process directly. A Linux host-network profile is the
initial LAN multicast deployment; published ports do not apply in that mode.
Ordinary bridge networking is sufficient for server-only TLS. Multicast
through other Docker network drivers and Docker Desktop requires independent
verification. Restrict capabilities, pipe permissions, and writable runtime
mounts; never embed private keys in images.

## Verification and acceptance boundary

The [unit suite](../tests/unit/) checks public contracts without a server.
[Cucumber features](../features/) express user-observable framing,
normalization, sessions, delivery, and configuration workflows. Coverage must
include every split point for short frames, coalesced messages, split UTF-8
and varints, comments/CDATA with apparent end tags, malformed/truncated input,
DTD rejection, limits, independent sessions, and randomized parser inputs.
No error may emit partial XML.

Session tests cover credential-first writes, fixed modes, no offer, rejected
and accepted upgrades, mismatched controls, timeout, ordinary XML while
pending, and XML acceptance plus binary data in the same read. Normalization
tests cover combined envelopes, nested unknown fields, server envelope
timestamps, binary extensions, conflicts, exact base64 recovery, rejected
cores, and post-expansion size checks. Delivery tests force short writes,
full queues, dead sinks, duplicates, UDP oversize, and incomplete EOF.

TLS certificate failures, OS multicast/FIFO behavior, actual server routing,
and deployment supervision require adapter/integration tests. Live server
verification uses an independently configured TAK Server: provoke a known
event, then correlate it on the gateway's framed output. An authenticated REST
mission operation is one possible trigger; HTTP success alone is insufficient.
The repository does not provision a server or maintain a live-server harness.
XML/TLS, negotiated and fixed Protobuf, password profiles, and native Windows
pipes require verification in their deployment environments.

The implementation composes output framing and XML preservation, UDP/multicast
and authenticated TLS, TAK v1 negotiation, envelope separation, conversion and
opaque preservation, bounded fan-out, and platform adapters. Contract tests
remain independent of those implementation boundaries.

[protocol]: https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/commoncommo/core/impl/protobuf/protocol.txt
[transports]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/marti/service/TransportCotEvent.java
[codec]: https://github.com/pgsocks/cot/blob/a216ae386e8652ccb714bd6abf14ffd5333ff867/docs/api.md
[server-envelope]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-protobuf/src/main/proto/takmessage.proto
