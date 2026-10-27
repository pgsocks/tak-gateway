# TAK protocol and API research

Sources inspected October 22, 2026 (America/Chicago). This record supports the
decisions in [design.md](design.md). Source inspection establishes protocol
definitions and code paths; live test results establish only the profiles run.
The public API and defaults are gateway design decisions unless attributed
below.

## Reproducible source baseline

| Source | Inspected revision | Use |
| --- | --- | --- |
| Official ATAK-Civ protocol | `9f6893dd657feacc35ec5de03dad721c2e44170e` | Wire framing, authentication order, negotiation, detail extensions |
| Official TAK Server | `5187abd46d827d37cfc5708805eced197a837e49` | Transport distinctions, server envelope, REST-generated event path |
| `pgsocks/cot` | `a216ae386e8652ccb714bd6abf14ffd5333ff867` | Complete-message codec and loss diagnostics |
| Expat prior art | `a76b1174c1ea52abb639a4c69d52b630a8a106a2` | Incremental C parser, byte positions, caller state, resource controls |

The inspected codec API document has Git blob
`656849ee584bdb97249886adac7762e7aea61654`, matching the earlier design input.
Pin source and distribution identities in interoperability checks; a branch
name or unversioned container tag does not reproduce these observations. Source
URLs below are immutable commit URLs. Standards and platform documentation
are explanatory references, not claims of newly executed platform tests.

## Protocol facts and their consequences

The official [protocol specification][protocol] distinguishes traditional
XML, mesh framing, and streaming framing. Traditional UDP carries one XML
event per datagram. Mesh TAK framing is magic byte `BF`, unsigned-varint
version, magic byte `BF`, then payload. Streaming framing is `BF`,
unsigned-varint payload length, then payload, with version selected outside
the frame. Consequently the same framer cannot interpret the mesh varint as
a length or infer version from a stream frame.

Traditional client stream writes include an XML declaration, newline, and
event, without arbitrary separators after the closing event. That upstream
format is separate from the gateway's downstream length-prefix decision.
The specification describes end-tag searching for legacy stream delimiting;
the gateway chooses structural recognition so preserved comments and CDATA
cannot create false boundaries.

The streaming negotiation section establishes:

- Required authentication is the first client message; a server may send
  XML while awaiting it. Denied authentication closes the connection.
- A client requests a version only after observing a server offer and
  reuses the server-generated transaction UID.
- After requesting an upgrade, the client stops further XML writes while
  continuing to process received XML. The response wait is at least one
  minute.
- Acceptance switches both directions immediately after the response.
  Rejection returns to XML. Timeout requires disconnect because the session
  encoding is indeterminate.
- Detail-extension support is an explicit decoding capability. Even an
  endpoint that advertises no extension may encounter unknown extensions;
  receivers should make incomplete decoding visible.

These facts motivate an explicit session state machine and tests that place
the acceptance response and first binary frame in one read. XML preference
by default, a 65-second response timeout, fixed-mode configuration names,
and reconnect backoff are gateway choices. A passive multicast listener does
not claim to implement the specification's transmitting mesh participant
announcement algorithm.

The specification caps header varints at ten bytes and describes a 64-bit
format, while spelling its effective range as `[0, 2^63 - 1]`. This wording
does not justify accepting an unbounded length. Gateway limits are much
smaller; tests must reject arithmetic overflow, excessive continuation bytes,
and any declared length above the configured maximum before allocation.

## Server transport and envelope evidence

[`TransportCotEvent.java`][transports] maps `cotmcast` to XML-only multicast,
`mcast` and `udp` to XML/Protobuf-capable datagram protocols, and distinguishes
`cottls`, `prototls`, and mixed `tls`. The names are configuration semantics,
not merely aliases for the same encrypted byte stream.

[`NioNettyTlsServerHandler.java`][tls-handler] initializes `PROTOTLS` directly
in Protobuf mode; its read path first passes bytes through authentication
handling and then chooses XML or Protobuf processing. Its writer likewise
selects encoding from protocol state. This supports testing fixed Protobuf
separately from an XML upgrade, rather than assuming every TLS connection
begins with negotiated XML event framing. Exact credential/protocol
combinations remain endpoint-specific interoperability tests.

[`AuthCot.java`][auth] models username, password, UID, and callsign. This is
TAK application authentication, distinct from TLS client authentication and
from REST authentication. The source does not establish a gateway-specific
authentication-success record; status should report observable progress.

[`SubmissionService.java`][submission] recognizes `t-x-c-t` as a ping and
`sendPong` replies on the source subscription with event type `t-x-c-t-r` and
fixed UID `takPong`. It does not echo the request UID. Consequently the gateway
must correlate maintenance by current connection and a single outstanding
ping, rather than require an unsupported request-token exchange. Optional
maintenance is disabled by default, suspends during negotiation, and consumes
the reply internally. Separate caller-supplied monotonic and UTC times avoid
using deadline-clock values as CoT timestamps in deterministic session tests.

The official [ATAK envelope][atak-envelope] permits `takControl` and
`cotEvent` independently. A combined envelope remains event-bearing.
The pinned [server envelope][server-envelope] also defines `submissionTime`
and `creationTime` as top-level fields 3 and 4. Removing controls must preserve
all other original wire fields; extracting and reserializing only `cotEvent`
would lose these real server fields and any future unknown fields.

## Codec capability and preservation boundary

The inspected [`cot` API contract][codec] accepts one complete XML document
or one unframed event-only `TakMessage`. It does not perform TAK framing,
networking, authentication, or negotiation. Control-only, combined
control/event, and empty envelopes are unsupported. Protobuf parsing follows
normal merging rules, so concatenated serializations cannot be recovered as
original message boundaries by the codec.

The codec's XML model intentionally rejects some unknown event/point
attributes, extra core children, and namespaces on core elements. It retains
arbitrary detail children semantically, but an XML round trip can change
whitespace, quoting, namespace placement, attribute order, and declaration
presence. The gateway's byte-preserving XML path must therefore be separate.

`cot_decode_ex` reports discarded unknown Protobuf fields and complete skipped
wire-byte counts, including nested fields. Success does not promise their
preservation. [`src/protobuf.cpp`][codec-protobuf] implements the skipping and
diagnostic counters; [`src/cot.cpp`][codec-core] enforces the envelope boundary.
The gateway must inspect diagnostics even when decoding succeeds.

The codec preserves binary detail extensions in its native representation,
but has no extension-to-XML decoder and rejects them on XML encoding. For
typed/residual conflicts, residual XML wins and the typed representation is
not emitted. These are reasonable codec conventions but would violate a
transparent gateway's completeness promise if treated as lossless conversion.
The gateway chooses explicit `preserve-opaque` or `reject` policy. Its
namespaced base64 preservation element is an application extension, not an
upstream standard or proof of semantic equivalence.

## Parser and native API prior art

Expat's [reference manual][expat] provides a mature procedural C model:
opaque parser ownership, callbacks with caller state, incremental `XML_Parse`,
`XML_StopParser`, current-byte-index queries, custom memory management through
`XML_ParserCreate_MM`, and explicit entity-processing controls. This supports
structural event recognition without imposing DOM ownership on consumers.

The relevant lesson is the separation of parser state from input-buffer
ownership and application policy. Byte indices can identify original spans;
parsed callback text is not an exact copy of the serialized XML. DTD/entity
policy, nesting limits, retained-byte limits, and final-document validation
still belong to the gateway. Choosing a mature parser does not itself prove
bounded memory or safe configuration. This research does not add Expat or
freeze a parser dependency; implementation must follow repository dependency
rules and demonstrate the required bounds.

The codec's native API is also direct prior art for counted byte spans,
library-owned output buffers with matching destroy operations, immutable
borrowed views, explicit status returns, and no exception crossing. The
gateway can use the same ownership vocabulary while keeping session and
delivery responsibilities separate.

## REST event generation for interoperability checks

The pinned server provides an evidence-backed trigger through mission
creation. [`BaseRestController.java`][rest-base] supplies `/Marti/api`.
[`MissionApi.java`][missions] exposes `PUT /missions/{name}` with query
parameters including `creatorUid` and `group`; mission creation can use an
empty body. A unique name and nonempty creator UID avoid collisions and make
cleanup/correlation explicit. The creator identity differs from the gateway
subscriber so creator suppression does not hide the announcement.

[`MissionServiceDefaultImpl.java`][mission-service] broadcasts
`MISSION_CREATE` after creation. [`DistributedSubscriptionManager.java`][subscriptions]
constructs a `t-x-m-n` CoT mission CREATE announcement, with a server-generated
event UID and mission fields including the mission name. Its delivery path
checks groups, explicitly includes `__ANON__`, and skips the creator client
UID. An interoperability check must correlate mission name/type rather than
assume the REST caller chooses the resulting event UID. Deleting the mission
uses the corresponding mission DELETE operation and can emit `t-x-m-d`.

This is not a general arbitrary-CoT injection endpoint. In particular,
[`CotApi.java`][cot-api]'s POST operation queries stored CoT by UIDs, while
injector configuration concerns detail injection. Neither should be described
as a generic event-push API without source and live evidence.

The configured HTTPS REST connector uses a trusted client certificate
authorized for the operation. [`security-context.xml`][rest-security] includes
the X.509 filter and restricts additional HTTP Basic requirements to other
paths; a mission request on the configured mTLS connector does not require
an invented bearer token or HTTP Basic login. This is separate from a TAK
streaming XML credential preamble. Keep chain and hostname verification
enabled for both interfaces.

When checking an independently configured server, REST success establishes
only that the operation succeeded: subscribe over XML/TLS before triggering,
then observe a matching server-generated event at the gateway's framed output.
Use logs, bounded deadlines, unique fixture names, and mission deletion to
make the result observable and leave the server clean. The repository retains
this protocol research but does not provision a server or maintain a live-server
harness.

The XML/TLS mission-trigger profile is a useful first interoperability
fixture. It does not prove arbitrary XML byte preservation through a server,
Protobuf negotiation, all detail extensions, password authentication,
multicast reachability, or native Windows pipe semantics.

## Standards and platform constraints

| Reference | Consequence for the design |
| --- | --- |
| [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293), TCP | Byte-stream delivery requires explicit downstream record boundaries |
| [Linux pipe(7)](https://man7.org/linux/man-pages/man7/pipe.7.html) | FIFO byte streams, short I/O, reader/writer behavior, and `SIGPIPE` need explicit handling |
| [RFC 8085](https://www.rfc-editor.org/rfc/rfc8085), UDP | Datagrams do not provide reliable delivery; large payloads and congestion require limits |
| [Windows named-pipe modes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-type-read-and-wait-modes) | Byte and message modes differ; portable framing uses byte mode |
| [Docker host networking](https://docs.docker.com/engine/network/drivers/host/) | Host network namespace and published-port behavior differ from bridge deployment |
| [systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html) | A supervisor can manage the foreground executable and its termination lifecycle |

The `u32be` output protocol, namespace URI, configuration schema, 8 MiB message
limit, queue budgets, loopback defaults, and drop-newest policy are gateway
choices. Revisit them when measurements or interoperability evidence justify
a change; do not attribute them to TAK or these platform references.

[protocol]: https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/commoncommo/core/impl/protobuf/protocol.txt
[transports]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/marti/service/TransportCotEvent.java
[tls-handler]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/marti/nio/netty/handlers/NioNettyTlsServerHandler.java
[auth]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/cot/model/AuthCot.java
[submission]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/marti/service/SubmissionService.java
[atak-envelope]: https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/commoncommo/core/impl/protobuf/takmessage.proto
[server-envelope]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-protobuf/src/main/proto/takmessage.proto
[codec]: https://github.com/pgsocks/cot/blob/a216ae386e8652ccb714bd6abf14ffd5333ff867/docs/api.md
[codec-protobuf]: https://github.com/pgsocks/cot/blob/a216ae386e8652ccb714bd6abf14ffd5333ff867/src/protobuf.cpp
[codec-core]: https://github.com/pgsocks/cot/blob/a216ae386e8652ccb714bd6abf14ffd5333ff867/src/cot.cpp
[expat]: https://github.com/libexpat/libexpat/blob/a76b1174c1ea52abb639a4c69d52b630a8a106a2/expat/doc/reference.html
[rest-base]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/takserver-war/src/main/java/com/bbn/marti/network/BaseRestController.java
[missions]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/takserver-war/src/main/java/com/bbn/marti/sync/api/MissionApi.java
[mission-service]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/takserver-war/src/main/java/com/bbn/marti/sync/service/MissionServiceDefaultImpl.java
[subscriptions]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/java/com/bbn/marti/service/DistributedSubscriptionManager.java
[cot-api]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/takserver-war/src/main/java/com/bbn/marti/sync/api/CotApi.java
[rest-security]: https://github.com/TAK-Product-Center/Server/blob/5187abd46d827d37cfc5708805eced197a837e49/src/takserver-core/src/main/resources/security-context.xml

## Implementation dependencies

The implementation uses OpenSSL 3.5.4 for TLS, certificate and hostname
verification, PEM keys, and PKCS#12 identities. A locally installed OpenSSL 3
can be selected for development. c-ares 1.34.5 provides asynchronous name
resolution driven by the caller's poll loop, avoiding a blocking resolver or
hidden resolver worker. Expat validates XML with DTDs disabled; a small lexical
boundary scanner prevents repeated parsing of growing tokens on short reads.
The existing pinned CoT codec provides the standard Protobuf mapping; gateway
code preserves the exact event-only wire bytes when mapping would lose data.

TOML parsing uses tomlc99 revision
`29076dfd095bbbbd50a3c1b2760d29f4b83e74ac`. The gateway applies its own strict
schema and reference validation over that parser. These C interfaces fit the
procedural ownership and error contracts without adding C++ object frameworks.
Each dependency is integrated in its own CPM subdirectory. Upstream code is
compiled without behavioral patches.

- [OpenSSL release source](https://github.com/openssl/openssl/tree/openssl-3.5.4)
- [c-ares release source](https://github.com/c-ares/c-ares/tree/v1.34.5)
- [tomlc99 source](https://github.com/cktan/tomlc99/tree/29076dfd095bbbbd50a3c1b2760d29f4b83e74ac)
