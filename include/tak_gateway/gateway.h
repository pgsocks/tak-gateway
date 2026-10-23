#ifndef TAK_GATEWAY_GATEWAY_H
#define TAK_GATEWAY_GATEWAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Proposed source contract, version 1; not a stable binary ABI.
 *
 * All functions are synchronous and report errors through tg_status. They do
 * not throw across this interface or use a global last-error value. Each
 * handle requires caller serialization, including read-only operations and
 * destruction. Independent handles may be used concurrently. No operation
 * invokes a user callback. No function is async-signal-safe.
 *
 * Unless stated otherwise, pointers are nonnull, inputs are borrowed only for
 * the call, input/output objects must not overlap, and an output buffer must
 * initially be {NULL, 0}. Handles must be live objects from the matching
 * create function; arbitrary, stale, and already-destroyed handles are not
 * validated. Invalid argument values are rejected before state mutation.
 * Allocation failures report TG_NO_MEMORY. Unimplemented skeletons report
 * TG_UNIMPLEMENTED; that status never represents successful behavior.
 * Every creating function sets its output handle to NULL on failure.
 */

/* Operation results. Nonnegative results describe successful processing. */
typedef enum tg_status {
	TG_OK = 0,              /* The documented result is available. */
	TG_MORE = 1,            /* No complete result; supply/drain bytes. */
	TG_NO_EVENT = 2,        /* Valid control-only input, no event. */
	TG_DROPPED = 3,         /* Report describes per-output losses. */
	TG_INVALID = -1,        /* Invalid argument or configuration value. */
	TG_MALFORMED = -2,      /* Invalid XML/Protobuf/TOML/wire framing. */
	TG_UNSUPPORTED = -3,    /* Unsupported version/extension/policy. */
	TG_LIMIT = -4,          /* Resource/size limit reached. */
	TG_TRUNCATED = -5,      /* Datagram truncation or EOF within a frame. */
	TG_NO_MEMORY = -6,      /* Allocation failed. */
	TG_BAD_STATE = -7,      /* Operation is not allowed in this state. */
	TG_TIMEOUT = -8,        /* Partial message or negotiation timed out. */
	TG_IO = -9,             /* Local file/socket/TLS failure. */
	TG_LOSSY = -10,         /* Incomplete conversion forbidden by policy. */
	TG_UNIMPLEMENTED = -11  /* Explicit design/test-stage skeleton. */
} tg_status;

/* Borrowed bytes; data may be NULL exactly when size is zero. */
typedef struct tg_bytes {
	const uint8_t *data;
	size_t size;
} tg_bytes;

/* Owned allocation. Release only with tg_buffer_release, never free/delete. */
typedef struct tg_buffer {
	uint8_t *data;
	size_t size;
} tg_buffer;

/*
 * Release an owned buffer and set both fields to zero. NULL and an empty
 * buffer are harmless. Do not fabricate descriptors or release borrowed
 * pointers. Other functions never take ownership of an input tg_buffer.
 */
void tg_buffer_release(tg_buffer *buffer);

/* Payload encoding after transport framing has been removed. */
typedef enum tg_encoding {
	TG_ENCODING_NONE = 0,       /* Empty result only; not an input mode. */
	TG_ENCODING_XML = 1,        /* One UTF-8 CoT event document. */
	TG_ENCODING_PROTOBUF_V1 = 2 /* One unframed v1 TakMessage envelope. */
} tg_encoding;

/* Owned message; release bytes with tg_buffer_release(&message.bytes). */
typedef struct tg_message {
	tg_encoding encoding;
	tg_buffer bytes;
} tg_message;

/*
 * Positive inclusive limits. max_message_bytes must be <= UINT32_MAX.
 * max_message_bytes bounds each input payload and final normalized XML;
 * framing headers are additional. XML root depth is one. retained_bytes
 * bounds retained event payload bytes in a delivery handle, counted once per
 * admitted event regardless of the number of output references. Queue byte
 * accounting includes framing and charges a complete record until consumed.
 * max_outputs counts live downstream connections, including listener clients.
 */
typedef struct tg_limits {
	size_t max_message_bytes;
	uint32_t max_xml_depth;
	size_t retained_bytes;
	size_t queue_bytes;
	size_t queue_records;
	size_t max_outputs;
} tg_limits;

/* C11/C++17 initializer: 8 MiB, depth 128, 256 MiB, 64 MiB, 4096, 1024. */
#define TG_LIMITS_DEFAULT \
	{8388608u, 128u, 268435456u, 67108864u, 4096u, 1024u}

/* Opaque incremental framer; one instance per stream/session. */
typedef struct tg_framer tg_framer;

/*
 * Create an empty framer. encoding is XML or PROTOBUF_V1; limits may be NULL
 * for TG_LIMITS_DEFAULT and are copied. No files, sockets, or timers are used.
 * Set *out to NULL on failure. XML and binary stream state never mix.
 */
tg_status tg_framer_create(tg_encoding encoding, const tg_limits *limits,
	tg_framer **out);

/*
 * Consume at most one message from input. Set *consumed to the exact prefix
 * retained/processed; caller resubmits the suffix. TG_OK returns an owned
 * message, TG_MORE an empty message. Empty input is permitted. On errors,
 * message is empty, consumed describes the processed prefix, and the framer
 * fails until reset. Invalid arguments consume zero and do not fail it.
 *
 * XML is structurally delimited, validated UTF-8, and rooted in an unprefixed
 * <event>. Preserve original document bytes from its optional BOM/declaration
 * through the root end; discard inter-document XML whitespace. Reject DTDs,
 * custom entities, external resources, invalid nesting and excess depth.
 * Predefined/numeric XML entities remain permitted. Comments, CDATA and
 * processing instructions do not terminate a document. A stream header is
 * BF + unsigned varint payload length, with at most 10 varint bytes; zero,
 * overflow, invalid magic and lengths above the limit fail before allocation.
 * A protobuf framer does not validate the envelope body.
 * Work is O(consumed bytes); retained storage is bounded by the message limit.
 */
tg_status tg_framer_feed(tg_framer *framer, tg_bytes input, size_t *consumed,
	tg_message *message);

/*
 * Signal stream EOF. TG_OK means no partial document/header/payload remains;
 * XML transport whitespace alone is a clean boundary. TG_TRUNCATED fails the
 * framer when a partial message remains. Failed framers return TG_BAD_STATE.
 * After successful finish, repeated finish is TG_OK; feed is TG_BAD_STATE
 * until reset. Finish never produces an application message.
 */
tg_status tg_framer_finish(tg_framer *framer);

/* Discard buffered bytes/failure state; retain encoding and limits. */
tg_status tg_framer_reset(tg_framer *framer);

/* Release the framer and all partial bytes. NULL is harmless. */
void tg_framer_destroy(tg_framer *framer);

/* Accepted UDP encodings. AUTO validates magic before accepting binary. */
typedef enum tg_datagram_mode {
	TG_DATAGRAM_AUTO = 0,
	TG_DATAGRAM_XML = 1,
	TG_DATAGRAM_TAK_V1 = 2
} tg_datagram_mode;

/*
 * Decode exactly one received datagram, with no retained state. truncated is
 * 0/1 from the transport; 1 rejects the whole datagram as TG_TRUNCATED. XML
 * must contain exactly one complete event with optional surrounding XML
 * whitespace; its entire datagram bytes are preserved. TAK mesh uses
 * BF + varint(version=1) + BF + nonempty envelope, not a stream length header.
 * Unknown versions return TG_UNSUPPORTED. Enforce limits before copying;
 * NULL limits use defaults. Return an owned unframed message on TG_OK and an
 * empty message otherwise. Binary envelope validation belongs to normalize.
 * XML EOF within a document is TG_TRUNCATED; malformed mesh headers and
 * multiple XML documents are TG_MALFORMED when the truncation flag is clear.
 */
tg_status tg_datagram_decode(tg_datagram_mode mode, tg_bytes input,
	int truncated, const tg_limits *limits, tg_message *message);

/* Behavior when an event cannot be represented completely by standard XML. */
typedef enum tg_preservation_policy {
	TG_PRESERVE_OPAQUE = 0, /* Map fields and retain original bytes. */
	TG_REJECT_LOSSY = 1     /* TG_LOSSY, with no partial XML. */
} tg_preservation_policy;

/*
 * Per-call conversion observations; zero on any error. Counts refer to the
 * original envelope, including nested unknown fields; preserved_opaque is
 * 0/1. had_control is 0/1 and does not imply that no event was present.
 */
typedef struct tg_normalize_report {
	uint64_t unknown_fields;
	int had_control;
	int preserved_opaque;
} tg_normalize_report;

/*
 * Normalize one complete unframed message without changing input. XML event
 * bytes are preserved exactly; protocol controls return TG_NO_EVENT with an
 * empty result. Consume only recognized TAK controls, never every t-* type.
 * For Protobuf, separate takControl from cotEvent and normalize an event even
 * when both are present. Envelope scanning preserves original event-related
 * wire bytes and repeated-field merge semantics. Empty/malformed envelopes
 * fail. No sockets, session changes, logging or external resources are used.
 *
 * Use the pgsocks/cot standard mapping. Under PRESERVE_OPAQUE, unmapped fields,
 * undecodable binary detail and lossy typed/residual conflicts add a detail
 * element in namespace urn:tak-gateway:preservation:1. Its local name is
 * protobuf; protocolVersion="1", encoding="base64", reason="unmapped-fields".
 * Content recovers the exact event-only TakMessage bytes, with
 * recognized control fields and transport headers removed. Choose a prefix
 * that does not collide with existing namespaces. Under REJECT_LOSSY, return
 * TG_LOSSY. Invalid/unrepresentable core values always fail; never invent
 * replacement UIDs, times, coordinates or types. Check size after fallback.
 * Syntactically valid Protobuf with unrepresentable core values (including
 * out-of-range latitude or nonfinite coordinates) returns TG_UNSUPPORTED.
 * Malformed Protobuf/core wire encodings return TG_MALFORMED.
 *
 * limits may be NULL. TG_OK returns owned XML and a report; TG_NO_EVENT returns
 * an empty buffer and report.had_control=1. All errors clear both outputs.
 * O(input + output bytes), bounded by limits; may allocate conversion memory.
 */
tg_status tg_normalize(tg_encoding encoding, tg_bytes input,
	tg_preservation_policy policy, const tg_limits *limits, tg_buffer *xml,
	tg_normalize_report *report);

/* Selected server event mode; independent of the TLS endpoint's port number. */
typedef enum tg_protocol_mode {
	TG_PROTOCOL_NEGOTIATED = 0,
	TG_PROTOCOL_XML_ONLY = 1,
	TG_PROTOCOL_PROTOBUF_ONLY = 2
} tg_protocol_mode;

/* Negotiated mode preference; fixed modes require PREFER_XML (unused). */
typedef enum tg_protocol_preference {
	TG_PREFER_XML = 0,
	TG_PREFER_PROTOBUF = 1
} tg_protocol_preference;

/*
 * Transport-free session profile. Strings are UTF-8 NUL-terminated and copied
 * on create. uid is nonempty; callsign may be NULL to use uid. Username and
 * password must both be NULL (no credential preamble) or both nonempty.
 * Credentials are XML-escaped and never emitted as event output. The caller
 * must verify a real network transport's TLS identity before sending them.
 * version must be 1 (other versions return TG_UNSUPPORTED).
 * response_timeout_ms >=60000; partial_timeout_ms >0.
 *
 * Maintenance defaults off: ping_interval_ms and ping_timeout_ms are both zero
 * or both positive. One connection-scoped ping may be outstanding. identity_xml
 * is empty by default, otherwise one complete valid CoT XML event with uid
 * equal to this profile's uid. It is copied, queued once after credentials and
 * before negotiation on each create/reset, and never synthesized from (0,0).
 * Fixed Protobuf profiles require lossless conversion of this supplied XML;
 * otherwise creation returns TG_LOSSY. identity bytes obey message limits.
 */
typedef struct tg_session_options {
	tg_protocol_mode protocol;
	tg_protocol_preference preference;
	uint32_t version;
	uint64_t response_timeout_ms;
	uint64_t partial_timeout_ms;
	const char *uid;
	const char *callsign;
	const char *username;
	const char *password;
	uint64_t ping_interval_ms;
	uint64_t ping_timeout_ms;
	tg_bytes identity_xml;
} tg_session_options;

/* Observable states of one already-established transport's TAK session. */
typedef enum tg_session_state {
	TG_SESSION_AUTH = 0,             /* Credential bytes remain to send. */
	TG_SESSION_XML = 1,
	TG_SESSION_UPGRADE_PENDING = 2,  /* Request is next write or sent. */
	TG_SESSION_PROTOBUF = 3,
	TG_SESSION_FAILED = 4
} tg_session_state;

/* Copied, nonsecret status. No payload or credential values are exposed. */
typedef struct tg_session_info {
	tg_session_state state;
	int credentials_sent;
	int protocol_offer_received;
	uint64_t controls_consumed;
	uint64_t events_received;
	size_t pending_write_bytes;
	int ping_outstanding;
	uint64_t pings_sent;
	uint64_t pongs_received;
} tg_session_info;

/* Opaque per-connection session; does not own or acquire the TLS transport. */
typedef struct tg_session tg_session;

/*
 * Create a session for an established transport. Copy profile and limits
 * (NULL limits use defaults); queue required credentials as the first client
 * application bytes. No authentication-success acknowledgment is invented.
 * now_ms and subsequent timestamps are caller-supplied monotonic milliseconds
 * in one epoch, nondecreasing per handle. Backwards time returns TG_INVALID
 * without changing state. Set *out=NULL on failure. No I/O or sleeping occurs.
 */
tg_status tg_session_create(const tg_session_options *options,
	const tg_limits *limits, uint64_t now_ms, tg_session **out);

/*
 * Consume stream bytes, processing controls and returning at most one owned
 * application message. Same consumed/output rules as framer_feed. Normalizing
 * the returned payload is a separate operation. Recognized XML controls are
 * consumed. Combined Protobuf control/event envelopes yield event-only bytes.
 *
 * Negotiation starts in XML, requests only advertised v1 when preferred, and
 * reuses the offer UID. The request follows any credential preamble. While
 * pending, ordinary XML events still emerge. A matching rejection resumes XML;
 * acceptance changes framing at the response's exact end, including binary
 * bytes in the same input span. Mismatched responses, malformed framing, and
 * size failures fail this session until reset. No offer means XML continues.
 * A response with a mismatched active negotiation UID returns TG_MALFORMED.
 * Fixed modes neither wait for offers nor request upgrades. XML-only consumes
 * supported offers without changing encoding. Updates partial-input timers.
 * A failed session returns TG_BAD_STATE. Invalid inputs do not fail it.
 * Remember an offer received during a queued/partially submitted maintenance
 * write, but finish that entire write before exposing the upgrade request.
 * The same ordering applies to credentials and explicit identity. Enter
 * UPGRADE_PENDING only when the request becomes the next permitted write;
 * never insert it into an earlier document or strand an earlier suffix.
 */
tg_status tg_session_receive(tg_session *session, tg_bytes input,
	uint64_t now_ms, size_t *consumed, tg_message *message);

/*
 * Borrow the remaining prefix of the next permitted upstream write. TG_MORE
 * returns an empty span if none exists. Bytes stay valid until consume_write,
 * reset or destruction; receives/ticks must not invalidate an outstanding
 * span except when they fail the session. No application events are relayed.
 */
tg_status tg_session_peek_write(tg_session *session, tg_bytes *bytes);

/*
 * Advance by bytes actually sent, 0..last peeked size. Larger counts are
 * TG_INVALID with no mutation. A zero count is permitted. Submitting the final
 * credential byte sets credentials_sent (not authenticated). Submitting the
 * final upgrade-request byte starts the response timeout. Never acknowledge
 * unsent bytes. Does not perform transport I/O.
 */
tg_status tg_session_consume_write(tg_session *session, size_t count,
	uint64_t now_ms);

/*
 * Check partial-input, negotiation and ping-response deadlines. Expiry at
 * elapsed >= configured timeout returns TG_TIMEOUT and fails the session.
 * Pending negotiation never silently falls back on timeout.
 * Partial-input time starts at its first byte and is not extended by later
 * fragments. Completing that frame clears its partial-input deadline.
 *
 * If ping maintenance is enabled and due, queue one protocol ping after
 * initial credential/identity writes, encoded in the current event mode.
 * Suspend generation/transmission while an upgrade is pending; never queue a
 * second ping while one is pending/outstanding. Interval starts at create/reset
 * and restarts when the last ping byte is submitted. Its response timeout also
 * starts at that final byte. Pings/pongs are consumed as controls.
 * An upgrade may begin immediately after a ping is fully submitted, without
 * awaiting its pong. That ping's response deadline stays armed during pending
 * negotiation and after acceptance. Recognize its pong in the current receive
 * encoding on the same connection; negotiation does not satisfy/reset it.
 *
 * utc_epoch_ms supplies Unix UTC milliseconds for a newly generated ping,
 * independently of monotonic now_ms. With maintenance enabled it must be in
 * 0..253402300779999 (1970..9999, reserving a 20-second stale interval);
 * otherwise TG_INVALID with no mutation. It is ignored when pings are off.
 * Use the supplied UTC instant; never interpret monotonic time as UTC.
 * Server pong type t-x-c-t-r/uid takPong does not echo the request UID.
 * Correlate only with the current connection and its one outstanding ping.
 * Return TG_OK when no deadline has expired. No I/O or system clock reads.
 */
tg_status tg_session_tick(tg_session *session, uint64_t now_ms,
	uint64_t utc_epoch_ms);

/*
 * Begin a replacement connection with the same copied profile. Discard all
 * bytes/negotiation progress, reset counters, queue credentials first again.
 * This is not an instruction to reconnect a socket. now_ms remains monotonic.
 */
tg_status tg_session_reset(tg_session *session, uint64_t now_ms);

/* Copy nonsecret state into info; clear info on invalid-argument errors. */
tg_status tg_session_snapshot(const tg_session *session,
	tg_session_info *info);

/* Free session and securely erase its copied credentials. NULL is harmless. */
void tg_session_destroy(tg_session *session);

/* Default stream framing is U32BE. XML_DOCUMENT has no separator/banner. */
typedef enum tg_output_framing {
	TG_FRAMING_U32BE = 0,
	TG_FRAMING_XML_DOCUMENT = 1
} tg_output_framing;

/* A live downstream byte-stream connection or one datagram destination. */
typedef enum tg_output_kind {
	TG_OUTPUT_STREAM = 0,
	TG_OUTPUT_DATAGRAM = 1
} tg_output_kind;

/* Overload behavior. FAIL rejects an entire admission and fails delivery. */
typedef enum tg_overflow_policy {
	TG_OVERFLOW_DROP_NEWEST = 0,
	TG_OVERFLOW_FAIL = 1
} tg_overflow_policy;

/*
 * One live consumer's settings. Zero queue values inherit delivery limits.
 * Datagram framing must be XML_DOCUMENT and max_datagram_bytes is 1..65507.
 * Stream max_datagram_bytes must be zero. Queue accounting includes a U32BE
 * prefix when selected. A too-small queue remains valid and drops oversized
 * records visibly. No socket, FIFO, or OS handle is stored in this struct.
 */
typedef struct tg_output_options {
	tg_output_kind kind;
	tg_output_framing framing;
	size_t max_datagram_bytes;
	size_t queue_bytes;
	size_t queue_records;
} tg_output_options;

/* Nonzero identity, unique until the owning delivery handle is destroyed. */
typedef uint64_t tg_output_id;

/*
 * Result of one valid publish. Sequence starts at one and increments once per
 * valid admission attempt, including attempts dropped by all/no outputs.
 * accepted_outputs + dropped_outputs equals the number of live outputs.
 * Invalid input/global-limit/fatal preflight errors leave all fields zero.
 */
typedef struct tg_publish_report {
	uint64_t aggregate_sequence;
	size_t accepted_outputs;
	size_t dropped_outputs;
} tg_publish_report;

/*
 * Cumulative delivery statistics. queued_bytes charges whole encoded records
 * until fully consumed. retained_bytes counts shared XML payloads once.
 * udp_oversize is output-specific rejection count. ID zero snapshots totals;
 * per-output retained_bytes is zero, since shared retention is global.
 */
typedef struct tg_delivery_info {
	size_t queued_records;
	size_t queued_bytes;
	size_t retained_bytes;
	uint64_t records_completed;
	uint64_t records_dropped;
	uint64_t udp_oversize;
	int failed;
} tg_delivery_info;

/* Opaque bounded fan-out engine; one aggregate order for all live outputs. */
typedef struct tg_delivery tg_delivery;

/* Copy limits/policy and create no outputs. NULL limits use defaults. */
tg_status tg_delivery_create(const tg_limits *limits,
	tg_overflow_policy policy, tg_delivery **out);

/*
 * Add a live output with an empty queue and copied options. It receives only
 * future publications. Return a nonzero id; failure sets *id=0. TG_LIMIT when
 * max_outputs is reached. The runtime adds one output per listener client.
 */
tg_status tg_delivery_add_output(tg_delivery *delivery,
	const tg_output_options *options, tg_output_id *id);

/*
 * Remove an output and count all queued/partially written records as drops.
 * Never resume its suffix on a replacement connection. Unknown/zero ids are
 * TG_INVALID. Removing an output remains allowed after fatal overflow.
 */
tg_status tg_delivery_remove_output(tg_delivery *delivery, tg_output_id id);

/*
 * Validate and offer one complete XML event to every live output, preserving
 * byte content and duplicate events. Copy/retain owned storage before return;
 * caller retains its input. U32BE prefix is a 4-byte big-endian byte count,
 * excludes itself, and adds no NUL/newline. UDP is one whole XML datagram.
 *
 * DROP_NEWEST rejects each full/oversize destination independently. TG_DROPPED
 * reports any loss (or no outputs), while healthy outputs retain order and
 * receive their records. A shared retained-budget failure rejects the whole
 * event as TG_LIMIT without changing queues or sequence. FAIL preflights all
 * outputs, rejects the whole admission on a local loss, reports TG_LIMIT,
 * marks delivery failed, and preserves existing queues for draining. Failed
 * engines reject further publish/add_output with TG_BAD_STATE.
 * A shared-budget TG_LIMIT also marks delivery failed under FAIL policy.
 *
 * Publish is O(XML bytes + output count). Diagnostic counters expose losses;
 * no retransmission, deduplication, upstream writes, or durability is implied.
 */
tg_status tg_delivery_publish(tg_delivery *delivery, tg_bytes xml,
	tg_publish_report *report);

/*
 * Borrow the remaining bytes of an output's head record; TG_MORE and an empty
 * span mean no record. No record interleaving. Span remains valid across
 * publications and operations on other outputs, until consume/remove on this
 * output or delivery destruction. Drain remains allowed after fatal overflow.
 */
tg_status tg_delivery_peek(tg_delivery *delivery, tg_output_id id,
	tg_bytes *bytes);

/*
 * Advance 0..last peeked size bytes actually written. Datagram consumption is
 * zero or the entire datagram only. Invalid counts preserve queue state.
 * Completing a record releases its queue charge and shared payload reference.
 * This neither writes to an OS object nor proves consumer processing.
 */
tg_status tg_delivery_consume(tg_delivery *delivery, tg_output_id id,
	size_t count);

/* Copy totals (id=0) or a live output's counters; clear info on error. */
tg_status tg_delivery_snapshot(const tg_delivery *delivery, tg_output_id id,
	tg_delivery_info *info);

/* Discard queues and release storage; do not drain. NULL is harmless. */
void tg_delivery_destroy(tg_delivery *delivery);

/*
 * Optional nonsecret error location. key is NUL-terminated, truncated to fit.
 * line/column are one-based or zero when unavailable. No payload, password,
 * passphrase, or secret value is included. Entire struct is zero on success.
 */
typedef struct tg_diagnostic {
	tg_status status;
	size_t line;
	size_t column;
	char key[128];
} tg_diagnostic;

/* Immutable validated-shape TOML configuration, independent of runtime. */
typedef struct tg_config tg_config;

/*
 * Parse strict UTF-8 TOML schema version 1 described in docs/api.md. Copy all
 * configuration; resolve relative file paths against absolute base_dir.
 * No file reads, getenv, DNS, socket or TLS work occurs. Reject unknown keys,
 * duplicate IDs, invalid ports/modes, impossible limits and direct configured
 * ingress/output loops. No plaintext ingress/password fallback is inferred.
 * out is set NULL on failure. diagnostic may be NULL; otherwise clear/set it.
 * TOML syntax/UTF-8 errors are TG_MALFORMED; unsupported schema/protocol
 * versions are TG_UNSUPPORTED; other schema/key/value defects are TG_INVALID.
 * Invalid arguments are TG_INVALID. Diagnostic
 * keys use dotted paths and zero-based array indices, e.g. inputs[0].port.
 */
tg_status tg_config_parse(tg_bytes toml, const char *base_dir, tg_config **out,
	tg_diagnostic *diagnostic);

/*
 * Check referenced files, protected secret permissions, named environment
 * variables and credential readability. Never connect, resolve DNS, enroll a
 * certificate or modify a file/environment variable. Nonsecret CA/certificate
 * files may be publicly readable; POSIX password/private-key files must have
 * no group/other access. Windows uses effective ACL checks. Secret paths are
 * configuration data; secret contents never appear in diagnostics. Results
 * are point-in-time checks, not immunity to later filesystem changes.
 */
tg_status tg_config_validate(const tg_config *config,
	tg_diagnostic *diagnostic);

/*
 * Return owned UTF-8 TOML with explicit defaults. Redact secret literals and
 * secret-reference values (file paths/environment names), replacing them with
 * "<redacted>". No secret file/environment reads. Not an executable config.
 * Empty output on error. Diagnostic is optional.
 */
tg_status tg_config_redacted(const tg_config *config, tg_buffer *toml,
	tg_diagnostic *diagnostic);

/* Release copied configuration; not associated runtimes. NULL is harmless. */
void tg_config_destroy(tg_config *config);

/* Foreground runtime lifecycle, driven only by its caller's poll calls. */
typedef enum tg_runtime_state {
	TG_RUNTIME_READY = 0,
	TG_RUNTIME_RUNNING = 1,
	TG_RUNTIME_DRAINING = 2,
	TG_RUNTIME_STOPPED = 3,
	TG_RUNTIME_FAILED = 4
} tg_runtime_state;

/* Opaque network-owning runtime, not a daemon or background worker. */
typedef struct tg_runtime tg_runtime;

/*
 * Copy immutable configuration, validate/load credentials, and create READY
 * runtime. Caller may destroy config afterward. May read local credential
 * files and named environment variables, but opens no network endpoints until
 * poll. Set *out=NULL on failure; diagnostic may be NULL. No global TLS trust
 * settings, logging streams, environment variables or signal handlers change.
 */
tg_status tg_runtime_create(const tg_config *config, tg_runtime **out,
	tg_diagnostic *diagnostic);

/*
 * Initialize/drive adapters, framing, sessions, normalization and delivery.
 * Wait for readiness at most max_wait_ms (zero means nonblocking), aside from
 * scheduler delay and bounded processing. DNS/connection/TLS progress must not
 * block other inputs. Upstream failures reconnect independently and return
 * TG_OK with snapshot diagnostics; fatal local errors return TG_IO/TG_LIMIT
 * and enter FAILED. No event/log bytes are mixed. STOPPED returns TG_OK and
 * performs no further I/O. FAILED returns TG_BAD_STATE. Serialized per runtime.
 */
tg_status tg_runtime_poll(tg_runtime *runtime, uint32_t max_wait_ms);

/*
 * Stop intake and enter DRAINING, closing upstreams and leaving groups. Drain
 * complete queued records through subsequent poll calls for at most drain_ms
 * monotonic milliseconds; then close outputs, count remaining drops and enter
 * STOPPED. drain_ms=0 closes immediately. Repeated calls preserve the original
 * deadline and return TG_OK. No sleeping; not safe in a signal handler.
 * STOPPED remains STOPPED. FAILED returns TG_BAD_STATE without state change.
 */
tg_status tg_runtime_stop(tg_runtime *runtime, uint32_t drain_ms);

/*
 * Copy an owned UTF-8 JSON status snapshot, even when no events arrive. See
 * docs/api.md for fields, including per-source protocol_offer_received used
 * by XML/TLS interoperability readiness. No remote REST call is performed.
 * Empty output on failure. The snapshot exposes neither bodies nor secrets.
 */
tg_status tg_runtime_snapshot(const tg_runtime *runtime, tg_buffer *json);

/* Close all endpoints without draining and erase secrets. NULL is harmless. */
void tg_runtime_destroy(tg_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
