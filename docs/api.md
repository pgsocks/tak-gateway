# TAK Gateway public contract

This is the version 1 source contract implemented by the gateway library and
foreground executable. Tests verify observable behavior; platform and deployment
verification boundaries are listed in the testing guide.

The authoritative declarations and operation-specific rules are in
[`gateway.h`](../include/tak_gateway/gateway.h).
[`gateway.hpp`](../include/tak_gateway/gateway.hpp) imports those same
procedural functions and types into `tak_gateway`; it adds no ownership model.
The C header supports C11 and C++17; the C++ view supports C++17. No stable
binary ABI or compatibility across arbitrary compiler options is promised.

## Responsibility and layers

The gateway receives independently authenticated TAK streams and UDP/multicast
datagrams, preserves complete XML CoT documents, converts TAK v1 Protobuf
events, and offers complete XML events to independent downstream outputs.
It forwards no source's event into another source. Duplicate and stale events
remain; there is no deduplication, state cache, durable replay, or end-to-end
acknowledgment. Output consumers receive the union authorized to the inputs.

Low-level interfaces support embedders with their own sockets and event loops.
Framers own partial input, sessions own TAK authentication and negotiation,
normalization owns complete-message conversion, and delivery owns bounded
output records. These handles acquire no sockets or threads. The runtime
composes them and adds TLS, DNS, UDP, multicast, reconnects and platform output
adapters. The [`design rationale`](design.md) explains these boundaries.

| Interface | Successful result | Failure boundary |
| --- | --- | --- |
| `tg_framer_*` | One owned, unframed message | Malformed stream fails until reset |
| `tg_datagram_decode` | One owned XML or mesh payload | Whole datagram rejected independently |
| `tg_normalize` | One owned XML event or `TG_NO_EVENT` | No partially converted XML |
| `tg_session_*` | Events and drainable permitted upstream writes | Reset on replacement connection |
| `tg_delivery_*` | Independent per-output admission | Visible bounded loss or fatal overflow |
| `tg_config_*` | Immutable schema-checked configuration | No network during validation |
| `tg_runtime_*` | Caller-driven foreground lifecycle | Independent upstream failures |

## Common ownership, errors and limits

All results use `tg_status`; exceptions never cross either public header.
Handles must be live objects from the matching creation function. Null
handles, except documented null-safe destruction, are invalid arguments. Stale
or fabricated handles are outside the contract. Independent handles can run
concurrently, but callers serialize every operation on each handle, including
snapshots and destruction. There are no user callbacks, global last-error
values or installed signal handlers. No function is async-signal-safe.

`tg_bytes` borrows bytes for the call. Null data is valid only for size zero.
`tg_buffer` owns returned storage; release it only with `tg_buffer_release`,
which clears the descriptor. Output buffers must initially be empty. Input
ownership never transfers, and input/output objects must not overlap. A
`tg_message` owns its `bytes` field; destroying its originating handle does
not invalidate the returned message.

Creation failures set output handles to null. Result-producing failures clear
their outputs as specified in the header. Framer/session errors may consume
a reported prefix before detection. Invalid arguments cause no state mutation.
`TG_MORE`, `TG_NO_EVENT` and `TG_DROPPED` are distinct nonnegative outcomes.
`TG_UNIMPLEMENTED` exclusively marks unfinished skeletons and is never success.

`TG_LIMITS_DEFAULT` supplies these positive inclusive limits:

| Field | Default | Meaning |
| --- | ---: | --- |
| `max_message_bytes` | 8,388,608 | Each input payload and final XML |
| `max_xml_depth` | 128 | Root element has depth one |
| `retained_bytes` | 268,435,456 | Shared XML payload bytes retained by delivery |
| `queue_bytes` | 67,108,864 | Complete encoded records per output |
| `queue_records` | 4,096 | Pending records per output |
| `max_outputs` | 1,024 | Live outputs, including TCP clients |

Null limits select defaults; nonnull limits require positive fields and
`max_message_bytes <= UINT32_MAX`. Per-output zero queue limits inherit
delivery limits. A queue smaller than a legal event visibly drops that record.
Framing overhead is additional to the input payload limit. The XML limit is
checked after conversion and preservation expansion.

Queue accounting charges each complete encoded record until its final byte is
consumed, including the four-byte length prefix. Shared retention counts each
admitted XML event once until its last output reference is released. These
counters bound retained message data, not allocator metadata or OS buffers.

## Framing and normalization

Use one framer per stream/session. Each feed reports its consumed prefix and
produces at most one message; resubmit the suffix. It can consume all input
and return `TG_MORE`. Partial UTF-8, markup, varints and payloads remain
buffered within limits. A framing failure requires reset. EOF at a boundary
succeeds; EOF within a document/header/payload is `TG_TRUNCATED`.
Successful finish is terminal until reset: repeated finish succeeds, while
feeding further bytes is `TG_BAD_STATE`.

XML framing structurally recognizes an unprefixed `<event>` root. Preserve its
optional BOM/declaration and all bytes through the root end. Discard transport
whitespace between stream documents. Whole XML datagrams retain surrounding
whitespace because their boundary is known. Preserve comments, CDATA,
processing instructions, namespaces and extension subtrees. Reject DTDs,
custom entities, external resources, malformed UTF-8/XML and excessive depth;
predefined and numeric entities remain legal. Concatenated documents are valid
stream input and invalid in a single datagram.

TAK v1 streams use `BF + varint(payload length) + payload`; mesh datagrams use
`BF + varint(version=1) + BF + payload`. Their varints have different meanings.
A stream length is positive, at most ten varint bytes, and bounded before
allocation. Unknown mesh versions return `TG_UNSUPPORTED`. Auto datagram mode
recognizes XML or validated mesh magic, never arbitrary non-XML bytes. An
OS-reported truncation rejects the whole datagram with no retained state.
An XML datagram ending within its document returns `TG_TRUNCATED` even when
the OS truncation flag is clear. With that flag clear, malformed mesh headers
and multiple XML documents return `TG_MALFORMED`.

Normalization accepts complete unframed messages. Preserve XML application
events byte-for-byte. Consume only recognized protocol controls, never all
application `t-*` events. XML negotiation uses exact
`event/detail/TakControl/TakProtocolSupport`, `TakRequest`, and `TakResponse`
elements with the expected control type. The session additionally correlates
its active negotiation transaction. Protobuf
control-only input returns `TG_NO_EVENT`; control plus event still yields its
event. Empty/malformed envelopes fail.

Conversion composes the complete-message [`pgsocks/cot` contract][cot-api].
Its input excludes transport headers and recognized top-level control fields.
Removing control preserves original event-related wire bytes, unknown fields
and repeated-field merge semantics. The normalizer reports discarded/unmapped
content instead of silently losing it.

`TG_PRESERVE_OPAQUE` maps representable fields and adds a detail element when
unknown fields, undecodable binary detail or typed/residual conflicts prevent
a complete mapping:

```xml
<gw:protobuf xmlns:gw="urn:tak-gateway:preservation:1"
             protocolVersion="1" encoding="base64"
             reason="unmapped-fields">BASE64_EVENT_ONLY_TAKMESSAGE</gw:protobuf>
```

The URI and local name identify this gateway extension; namespace collisions
may require another prefix. Its content recovers exact event-only envelope
bytes, without transport headers, authentication or recognized controls.
`reason="unmapped-fields"` covers each of these incomplete-mapping cases.
`TG_REJECT_LOSSY` returns `TG_LOSSY` with no XML. Invalid/unrepresentable core
values always fail; never invent coordinates, UID, type or timestamps. Opaque
retention does not claim interpretation of unknown binary semantics.
Syntactically valid Protobuf with unrepresentable core values, including
out-of-range latitude or nonfinite coordinates, returns `TG_UNSUPPORTED`;
malformed Protobuf/core wire encodings return `TG_MALFORMED`.

## Sessions, clocks and partial writes

`tg_session_create` starts a TAK session on an established transport. The
caller controls TLS and must verify a real peer before sending credentials.
Profiles are copied and copied passwords erased on destruction. Username and
password must both be present or absent. The credential document is escaped
XML and is the first client application write when required. Sending it is
not evidence of an authentication-success acknowledgment.

Caller timestamps are monotonic milliseconds in one epoch, nondecreasing per
handle. Backwards time is invalid without state mutation. This supports custom
event loops and deterministic tests without sleeps. Partial-message deadlines
start with the first uncompleted byte; trickling another byte does not extend
the deadline. Completion clears it.

`peek_write` borrows permitted pending bytes; `consume_write(n)` acknowledges
only bytes actually submitted by the caller's transport. Arbitrary short
writes are supported. The final credential byte sets `credentials_sent`;
the final negotiation-request byte starts its response deadline. Zero
consumption is legal; consuming more than the borrowed span changes no state.

Negotiated mode starts in XML. XML preference consumes offers without an
upgrade; Protobuf preference requests only advertised v1 and reuses the offer
UID. No offer means XML continues. A request follows credentials. While
pending, ordinary incoming XML events continue, but no further client XML
writes are permitted. Matching rejection resumes XML; matching acceptance
switches at the response's exact end, including binary bytes in the same read.
A mismatched active negotiation UID returns `TG_MALFORMED` and fails the
session. The response timeout is at least
60 seconds, runtime default 65; expiry closes the indeterminate session.

An offer received while a credential, identity or maintenance document is
queued or partly submitted is remembered. Finish that entire existing write
before exposing the upgrade request. Enter `UPGRADE_PENDING` only when the
request becomes the next permitted write. Never interleave the request with
an earlier document or strand a borrowed write suffix. Once the request is
next, further XML maintenance generation and transmission are suspended.

Fixed XML/Protobuf modes never initiate upgrades; their credential preamble
is separate from event encoding. Reset clears connection-specific state and
counters, then queues credentials first for a replacement connection. It
does not reconnect a socket. There is no arbitrary send-event API or default
synthetic map position. Optional maintenance is explicitly configured only.

`ping_interval_ms` and `ping_timeout_ms` are both zero by default, or both
positive to enable maintenance. `tg_session_tick` receives a separate UTC Unix
millisecond instant for generated CoT timestamps; monotonic milliseconds are
never mistaken for UTC. With pings enabled, UTC must be in
0..253402300779999, reserving twenty seconds for stale time. With pings disabled
that argument is ignored. No system clock is read by the low-level session.
The interval starts at create/reset and restarts when the final ping byte is
submitted. That final byte also starts its response deadline. A ping is queued
only after initial credentials/identity and in the current encoding. Upgrade
pending suspends its generation and transmission. At most one ping is queued
or outstanding. Official Server responds to `t-x-c-t` with `t-x-c-t-r`, UID
`takPong`, without echoing the ping UID: correlation is connection-scoped, not
an invented request-token exchange. Unsolicited pongs do not satisfy a future
ping, and no ping/pong appears in event output.
The upgrade request may follow a fully submitted ping immediately, without
waiting for its pong. That ping's response deadline remains armed during
negotiation and after acceptance. Its pong is recognized on the same
connection in the current receive encoding; negotiation neither satisfies nor
resets the ping deadline.

An optional copied `identity_xml` is a complete caller-supplied event whose UID
matches the profile. Queue it once per connection after credentials and before
negotiation. No identity is constructed from a default position, and supplied
times/coordinates are not refreshed or invented. Fixed Protobuf mode requires
lossless conversion or fails creation as `TG_LOSSY`. The runtime loads this
explicit event from a configured nonsecret file; continuously updated identity
and location providers require a separately designed extension.

## Delivery and stream boundaries

Add one delivery output for each live TCP/FIFO connection or UDP destination.
It starts empty and receives future publications. Delivery validates complete
XML, preserves duplicate events, and offers each event to every output. The
caller can release input bytes after publication returns. Per-source ordering
precedes aggregate admission; cross-source order is admission order without
timestamp sorting.

TCP and pipes default to `u32be`: four unsigned big-endian length bytes followed
by exactly that many UTF-8 XML bytes. Length excludes the header; add no NUL,
newline, banner, source wrapper or control record. Optional `xml-document`
mode requires consumers to parse document boundaries structurally. Neither
mode preserves packet/read/write-call boundaries. UDP is one complete XML
document without TAK headers, bounded to at most 65,507 bytes.

Writers obtain a head span and acknowledge actual bytes written through
`tg_delivery_consume`. Records never interleave. Datagram consumption is zero
or whole-datagram. Removing an output counts pending/partial records as drops;
a replacement never resumes a suffix or replays history. Consumers discard
incomplete records at EOF. FIFO reader replacement on a shared open FIFO is
unsupported; use separate TCP connections for fan-out.

Under `drop-newest`, full/oversize outputs drop independently and healthy
outputs retain a common admitted order. `TG_DROPPED` reports partial success.
Global retained-budget exhaustion returns `TG_LIMIT` without queue/sequence
mutation; under `fail` it also fails delivery. Under `fail`, a local loss
preflights and rejects the entire
admission, failing delivery while existing records remain drainable. Snapshot
and removal remain legal. Sequence increments for valid admission attempts,
including attempts dropped by all/no outputs; invalid input, global-limit
failure and fatal preflight do not advance it.

Delivery is live and best effort. Successful OS writes do not establish
consumer processing. UDP loss, local overflow, disconnects and reconnect gaps
remain explicit limitations.

## Version 1 configuration schema

`tg_config_parse` accepts UTF-8 TOML and an absolute base directory. It copies
configuration/resolves paths without file, environment, DNS or network access.
Reject unknown keys, duplicate IDs, invalid enums/ports, incompatible settings,
impossible limits and direct configured address/port loops. Static checks
cannot identify every DNS alias, NAT route or topology loop.
Syntax/UTF-8 errors return `TG_MALFORMED`; unsupported schema/protocol versions
return `TG_UNSUPPORTED`; other argument/schema/key/value defects are `TG_INVALID`.
Diagnostic keys are dotted paths with zero-based indices, for example
`inputs[0].port`. Empty diagnostics on success contain status zero and an
empty key; line/column are one-based when available.

`tg_config_validate` checks local references/secret permissions without changing
state or connecting. Runtime creation loads its own credentials. Disabled
endpoints require valid shape but not accessible credential files. Passwords
and passphrases use protected files or named environment variables, never
URLs or literal configuration values. Diagnostics identify keys/locations
without values. Redacted TOML replaces secret-reference paths/environment
names as well as values with `<redacted>` and is not executable configuration.

| Table/key | Values/default | Rules |
| --- | --- | --- |
| `version` | Required `1` | Reject other versions |
| `process.log_format` | `json` (default), `text` | stderr only |
| `process.log_level` | `info` (default), `debug`, `warning`, `error` | No payload/secret logging |
| `process.status_file` | Optional path | Atomic local JSON |
| `limits.max_message_bytes` | 8 MiB | Positive, <= `UINT32_MAX` |
| `limits.max_xml_depth` | 128 | Positive |
| `limits.retained_bytes` | 256 MiB | At least `max_message_bytes` |
| `limits.queue_bytes` | 64 MiB | Positive |
| `limits.queue_records` | 4,096 | Positive |
| `limits.max_inputs` | 32 | Positive |
| `limits.max_outputs` | 1,024 | Positive live delivery outputs |
| `normalization.protobuf_extra_fields` | `preserve-opaque` (default), `reject` | Explicit policy |
| `normalization.unrepresentable_core` | `reject` only | Never invent core values |
| `delivery.overflow` | `drop-newest` (default), `fail` | Local loss policy |

At least one enabled input/output is required. Each `[[inputs]]` has a unique
nonempty `id`, `enabled=true` by default, and `kind=tls`, `udp` or `multicast`.
Plaintext legacy TCP is optional future compatibility; version 1 never enables
it implicitly or falls back after TLS failure.

| TLS input key | Values/default | Rules |
| --- | --- | --- |
| `host` | Required name/IP | No embedded credentials |
| `port` | 8089 | 1..65535; port does not infer encoding |
| `uid` | Required nonempty UTF-8 | Stable across reconnects |
| `callsign` | Defaults to UID | For explicitly enabled writes |
| `protocol` | `negotiated` (default), `xml-only`, `protobuf-only` | Endpoint must support it |
| `prefer` | `xml` (default), `protobuf` | Negotiated mode only |
| `version` | Required `1` for protobuf-only | Omit in other modes |
| `tls.ca_file` | Required path | Chain verification required |
| `tls.server_name` | Defaults to host | Verification identity/SNI separate from address |
| `tls.cert_file` / `tls.key_file` | Optional paired PEM paths | Mutual TLS |
| `tls.key_password_file` / `tls.key_password_env` | Optional, mutually exclusive | Protected PEM passphrase |
| `tls.pkcs12_file` | Optional path | Alternative to PEM pair |
| `tls.pkcs12_password_file` / `tls.pkcs12_password_env` | Optional, mutually exclusive | Protected bundle passphrase |
| `auth.mode` | `none` (default), `password` | Server TLS always verified |
| `auth.username` | Required for password | Streaming XML, not HTTP Basic |
| `auth.password_file` / `auth.password_env` | Exactly one for password | Protected reference |
| `maintenance.ping_interval_ms` | 0 (disabled) | Positive with a positive timeout |
| `maintenance.ping_timeout_ms` | 0 (disabled) | Both ping settings zero or positive |
| `maintenance.identity_xml_file` | Optional nonsecret path | Explicit UID-matching event |

PEM and PKCS#12 profiles cannot combine. Client certificates and streaming
password authentication can coexist. Reject literal secrets, unknown insecure
TLS switches and plaintext fallbacks. POSIX password/private-key files must
have no group/other access; CA/certificates may be publicly readable. Windows
checks effective ACLs rather than treating Unix modes as equivalent.

Timing defaults: connect and handshake 10 seconds each, partial message 30,
negotiation 65, reconnect jitter between 1 and 60 seconds, backoff reset after
a stable 60-second session. DNS is re-resolved on reconnect. The initial TOML
schema keeps these defaults fixed; low-level callers can customize session
deadlines within their documented ranges. OS TCP keepalive is enabled; no
default TAK ping or synthetic identity position is transmitted.

| UDP/multicast key | Values/default | Rules |
| --- | --- | --- |
| `bind` | UDP: `0.0.0.0` | Local address |
| `port` | Required 1..65535 | No UDP login/subscription |
| `encoding` | `auto` (default), `xml`, `tak-v1` | Per-datagram |
| `allow_peers` | Optional IP/CIDR array | Filter, not authentication |
| `group` | Required multicast group | IPv4/IPv6 multicast |
| `interface` | Required for multicast | Explicit interface/scope |

Multicast is passive. Reject ambiguous memberships when destination/interface
metadata cannot separate them. Multiple groups use separate inputs. Server
static outputs/routing must actually send to configured listeners.

Each `[[outputs]]` has a unique nonempty `id`, `enabled=true` by default, and
`kind=tcp`, `udp` or `pipe`. Per-output queue settings inherit limits.

| Output key | Values/default | Rules |
| --- | --- | --- |
| `framing` | `u32be` (default), `xml-document` | TCP/pipe only |
| `queue_bytes` / `queue_records` | Inherit limits | Positive overrides |
| TCP `mode` | `listen` (default), `connect` | Independent reconnect for connect |
| TCP `bind` | `127.0.0.1` | Listen only |
| TCP `host` | Required for connect | Not accepted for listen |
| TCP `port` | Required 1..65535 | Explicit endpoint |
| TCP `max_clients` | 32 | Listen only; positive |
| UDP `host` | `127.0.0.1` | One XML document per datagram |
| UDP `port` | Required 1..65535 | No `framing` key |
| UDP `max_datagram_bytes` | 65,507 | 1..65507 after conversion |
| Pipe `path` | Required local path | FIFO/native Windows pipe |
| Pipe `create` | `false` | Explicit opt-in |
| Pipe `permissions` | `0600` | Restrictive POSIX octal mode |

POSIX output rejects regular files, symlinks and incompatible pipe objects,
uses nonblocking operations, and handles missing readers/EPIPE independently.
Require one writer and consumer. Windows uses local byte-mode pipes, restricted
security descriptors and asynchronous I/O, verified separately. A Linux FIFO
is not a native Windows named pipe. The first Docker gate uses TCP records;
platform support requires its own acceptance tests before a support claim.

Minimal fixture configuration:

```toml
version = 1

[process]
status_file = "/run/tak-gateway/status.json"

[[inputs]]
id = "fixture"
kind = "tls"
host = "127.0.0.1"
port = 8089
protocol = "xml-only"
uid = "gateway-fixture"

[inputs.tls]
server_name = "takserver"
ca_file = "/run/secrets/ca.pem"
cert_file = "/run/secrets/gateway.pem"
key_file = "/run/secrets/gateway.key"
key_password_file = "/run/secrets/gateway-key-password"

[[outputs]]
id = "capture"
kind = "tcp"
mode = "listen"
bind = "127.0.0.1"
port = 9000
framing = "u32be"
```

## Runtime status and lifecycle

Creation copies configuration and loads local credentials. Networking starts
on first poll. The runtime does not daemonize, install signal handlers, mutate
global trust/environment settings or create background workers. `poll(0)` is
nonblocking; positive values bound readiness waiting, aside from scheduling
and bounded processing. DNS/TLS must not stall other sources. Upstream failures
reconnect independently; fatal local initialization/overflow enters `FAILED`.

Snapshots are owned UTF-8 JSON, without payloads or secrets. Required shape:

```json
{
  "version": 1,
  "state": "RUNNING",
  "inputs": [{
    "id": "fixture",
    "state": "XML_ACTIVE",
    "credentials_sent": false,
    "protocol_offer_received": true,
    "controls_consumed": 1,
    "events_received": 0,
    "reconnects": 0
  }],
  "outputs": [{
    "id": "capture",
    "available": true,
    "clients": 0,
    "queued_records": 0,
    "queued_bytes": 0,
    "records_completed": 0,
    "records_dropped": 0,
    "udp_oversize": 0
  }]
}
```

Runtime states are `READY`, `RUNNING`, `DRAINING`, `STOPPED`, `FAILED`.
TLS input states are `DISCONNECTED`, `CONNECTING`, `TLS_HANDSHAKE`,
`AUTH_PREAMBLE`, `XML_ACTIVE`, `UPGRADE_PENDING`, `PROTOBUF_ACTIVE`, `BACKOFF`
and `FAILED`; UDP inputs use `LISTENING` or `FAILED`.
`protocol_offer_received` means a supported well-formed offer arrived on the
current connection. It resets on reconnect, applies in XML-only mode too,
and is more specific than an arbitrary control count. A listener's
`available=true` means bound/accepting; `clients` reports connected consumers.
Connect/pipe availability requires an established writer. Session counters
reset per connection; reconnect and output loss/completion counts accumulate.

The Docker gate waits for input `fixture` to be `XML_ACTIVE` with
`protocol_offer_received=true`, confirms output `capture` is available,
and connects the consumer. It then makes the authenticated, bodyless
`PUT /Marti/api/missions/{unique-name}` request. The server generates the event
UID; the resulting u32be XML must contain a mission change whose mission name
matches that unique name and whose type is `CREATE`. REST success alone does
not establish delivery, and the fixture does not assert a caller-chosen root
event UID.

Stop closes sources/leaves groups, then drains through subsequent polls until
the monotonic deadline. Zero closes immediately; repeats never extend the
original deadline. Destruction closes without draining and erases secrets.
Stop on `STOPPED` is harmless; stop on `FAILED` returns `TG_BAD_STATE` and
preserves failure status for inspection before destruction.
Main's signal handler sets an external flag; normal code calls stop. CLI exit
codes are 0 for success/clean stop, 2 for invalid configuration/usage and 3 for
fatal local runtime failures. Temporary upstream failure appears in status.
Configuration changes require restart; no seamless hot reload is promised.

## Caller examples

Decode and normalize a received datagram without acquiring a socket:

```c
tg_message message = {TG_ENCODING_NONE, {NULL, 0}};
tg_buffer xml = {NULL, 0};
tg_normalize_report report = {0};
tg_status status;

status = tg_datagram_decode(TG_DATAGRAM_AUTO, received, truncated, NULL,
	&message);
if (status == TG_OK)
	status = tg_normalize(message.encoding,
		(tg_bytes){message.bytes.data, message.bytes.size},
		TG_PRESERVE_OPAQUE, NULL, &xml, &report);
/* TG_OK: use xml; TG_NO_EVENT: control; otherwise record rejection. */
tg_buffer_release(&xml);
tg_buffer_release(&message.bytes);
```

This is C11; C++ uses `tg_bytes{...}` rather than a C compound literal.
`received` and `truncated` come from the caller's actual receive operation.

A custom stream loop repeatedly feeds the unconsumed suffix, normalizes each
application payload, publishes complete XML, then releases both owned results.
It drains session/output spans using actual OS write counts. Would-block
consumes zero. On transport error, discard the partial stream; reset upstream
session state or remove/re-add the downstream output for a new connection.

A runtime caller parses TOML with its base directory, optionally validates
local references, creates the runtime, then releases configuration. It polls
while watching an external stop flag, requests a five-second drain, continues
polling until `STOPPED`, then destroys the runtime. Redacted TOML and copied
snapshots have their own lifetimes and must be released independently.

## Evidence and verification boundary

Wire behavior follows the pinned [official TAK protocol][protocol] and
[TakMessage schema][takmessage]. TCP stream semantics follow [RFC 9293][tcp];
FIFO behavior follows [pipe(7)][pipe]. Opaque handles/incremental processing
use familiar conventions from [Expat][expat] and [zlib][zlib]; these are design
precedents; Expat also provides XML validation. Reporting consumed input and retaining
original spans avoids reconstructing XML through an event callback model.

Header compilation checks language compatibility. Native contract tests and
Cucumber scenarios exercise the implementation. Captured official-server
notifications verify REST-triggered delivery through XML/TLS and negotiated
Protobuf v1 to the gateway's TCP XML output. These checks do not establish native
Windows or full deployment support. See the [testing guide](testing.md).

[cot-api]: https://github.com/pgsocks/cot/blob/a216ae386e8652ccb714bd6abf14ffd5333ff867/docs/api.md
[protocol]: https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/commoncommo/core/impl/protobuf/protocol.txt
[takmessage]: https://github.com/TAK-Product-Center/atak-civ/blob/9f6893dd657feacc35ec5de03dad721c2e44170e/commoncommo/core/impl/protobuf/takmessage.proto
[tcp]: https://www.rfc-editor.org/rfc/rfc9293
[pipe]: https://man7.org/linux/man-pages/man7/pipe.7.html
[expat]: https://libexpat.github.io/doc/api/latest/#XML_Parse
[zlib]: https://zlib.net/manual.html
