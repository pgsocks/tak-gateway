#include "internal.h"
#include <cot/cot.h>

struct upstream_write {
	tg_buffer bytes;
	size_t offset;
	int kind;
	upstream_write *next;
};
struct tg_session {
	tg_session_options options;
	tg_limits limits;
	tg_session_info info;
	tg_framer *framer;
	bool binary;
	uint64_t now;
	uint64_t partial_start;
	bool partial;
	bool request_sent;
	uint64_t request_time;
	uint64_t last_ping;
	uint64_t ping_time;
	size_t peeked;
	char *transaction;
	upstream_write *head;
	upstream_write *tail;
};
static void wipe(char *p)
{
	if (!p)
		return;
	volatile char *v = p;
	for (size_t i = 0, n = strlen(p); i < n; ++i)
		v[i] = 0;
	free(p);
}
static void session_state(tg_session *s)
{
	if (s->info.state == TG_SESSION_FAILED)
		return;
	if (s->head && s->head->kind == 1)
		s->info.state = TG_SESSION_AUTH;
	else if ((s->head && s->head->kind == 3) || s->request_sent)
		s->info.state = TG_SESSION_UPGRADE_PENDING;
	else
		s->info.state =
			s->binary ? TG_SESSION_PROTOBUF : TG_SESSION_XML;
}
static tg_status queue_bytes(tg_session *s, tg_bytes input, int kind)
{
	upstream_write *w =
		static_cast<upstream_write *>(calloc(1, sizeof(*w)));
	if (!w)
		return TG_NO_MEMORY;
	tg_status status = tg_copy(input, &w->bytes);
	if (status < 0) {
		free(w);
		return status;
	}
	w->kind = kind;
	if (s->tail)
		s->tail->next = w;
	else
		s->head = w;
	s->tail = w;
	session_state(s);
	return TG_OK;
}
static tg_status queue_event(tg_session *s, tg_bytes xml, int kind)
{
	tg_vec wire{};
	tg_buffer binary{};
	bool ok;
	if (s->binary) {
		tg_status status = tg_binary_xml(xml, &binary);
		if (status < 0)
			return status == TG_NO_MEMORY ? status : TG_LOSSY;
		uint8_t magic = 0xbf;
		ok = tg_append(&wire, &magic, 1);
		uint64_t n = binary.size;
		do {
			uint8_t c = (uint8_t)(n & 127);
			n >>= 7;
			if (n)
				c |= 128;
			ok = ok && tg_append(&wire, &c, 1);
		} while (n);
		ok = ok && tg_append(&wire, binary.data, binary.size);
	} else {
		size_t start = 0;
		if (xml.size >= 3 && !memcmp(xml.data, "\xef\xbb\xbf", 3))
			start = 3;
		while (start < xml.size &&
			(xml.data[start] == ' ' || xml.data[start] == '\t' ||
				xml.data[start] == '\r' ||
				xml.data[start] == '\n'))
			++start;
		if (xml.size - start >= 5 &&
			!memcmp(xml.data + start, "<?xml", 5)) {
			while (start + 1 < xml.size &&
				!(xml.data[start] == '?' &&
					xml.data[start + 1] == '>'))
				++start;
			start += 2;
			while (start < xml.size &&
				(xml.data[start] == '\r' ||
					xml.data[start] == '\n' ||
					xml.data[start] == ' '))
				++start;
		}
		size_t end = xml.size;
		while (end > start && (xml.data[end - 1] == ' ' ||
					      xml.data[end - 1] == '\r' ||
					      xml.data[end - 1] == '\n' ||
					      xml.data[end - 1] == '\t'))
			--end;
		ok = tg_text(&wire,
			     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") &&
		     tg_append(&wire, xml.data + start, end - start);
	}
	tg_status result = ok ? queue_bytes(s, {wire.data, wire.size}, kind)
			      : TG_NO_MEMORY;
	tg_vec_free(&wire);
	tg_buffer_release(&binary);
	return result;
}
static tg_status initial_writes(tg_session *s)
{
	if (s->options.username) {
		tg_vec auth{};
		bool ok = tg_text(&auth, "<auth><cot username=\"") &&
			  tg_escape(&auth, s->options.username) &&
			  tg_text(&auth, "\" password=\"") &&
			  tg_escape(&auth, s->options.password) &&
			  tg_text(&auth, "\" uid=\"") &&
			  tg_escape(&auth, s->options.uid) &&
			  tg_text(&auth, "\" callsign=\"") &&
			  tg_escape(&auth, s->options.callsign) &&
			  tg_text(&auth, "\"/></auth>");
		tg_status status =
			ok ? queue_bytes(s, {auth.data, auth.size}, 1)
			   : TG_NO_MEMORY;
		if (auth.data)
			memset(auth.data, 0, auth.size);
		tg_vec_free(&auth);
		if (status < 0)
			return status;
	}
	if (s->options.identity_xml.size)
		return queue_event(s, s->options.identity_xml, 2);
	return TG_OK;
}
static void clear_writes(tg_session *s)
{
	while (s->head) {
		upstream_write *w = s->head;
		s->head = w->next;
		if (w->bytes.data)
			memset(w->bytes.data, 0, w->bytes.size);
		tg_buffer_release(&w->bytes);
		free(w);
	}
	s->tail = nullptr;
	s->peeked = 0;
}
tg_status tg_session_create(const tg_session_options *o,
	const tg_limits *limits, uint64_t now, tg_session **out)
{
	/*
	 * Validate/copy profile and protected credentials. Create one
	 * session/framer and queue escaped authentication before other writes.
	 */
	if (!out)
		return TG_INVALID;
	*out = nullptr;
	tg_limits l;
	if (!o || !tg_get_limits(limits, &l))
		return TG_INVALID;
	if (o->version != 1)
		return TG_UNSUPPORTED;
	if (o->protocol < TG_PROTOCOL_NEGOTIATED ||
		o->protocol > TG_PROTOCOL_PROTOBUF_ONLY ||
		o->preference < TG_PREFER_XML ||
		o->preference > TG_PREFER_PROTOBUF ||
		(o->protocol != TG_PROTOCOL_NEGOTIATED &&
			o->preference != TG_PREFER_XML) ||
		!o->uid || !*o->uid || o->response_timeout_ms < 60000 ||
		!o->partial_timeout_ms || (!!o->username != !!o->password) ||
		(o->username && (!*o->username || !*o->password)) ||
		(!!o->ping_interval_ms != !!o->ping_timeout_ms) ||
		!tg_span(o->identity_xml))
		return TG_INVALID;
	const char *strings[] = {o->uid, o->callsign, o->username, o->password};
	for (const char *str : strings) {
		if (str && !tg_utf8({(const uint8_t *)str, strlen(str)}))
			return TG_INVALID;
	}
	if (o->identity_xml.size) {
		tg_xml_info info{};
		tg_status status = tg_xml(o->identity_xml, &l, &info);
		bool match = status == TG_OK && !strcmp(info.uid, o->uid) &&
			     !info.control;
		tg_xml_clear(&info);
		if (!match)
			return status < 0 ? status : TG_INVALID;
	}
	tg_session *s = static_cast<tg_session *>(calloc(1, sizeof(*s)));
	if (!s)
		return TG_NO_MEMORY;
	s->options = *o;
	s->limits = l;
	s->now = now;
	s->last_ping = now;
	s->options.uid = tg_strdup(o->uid);
	s->options.callsign = tg_strdup(o->callsign ? o->callsign : o->uid);
	s->options.username = tg_strdup(o->username);
	s->options.password = tg_strdup(o->password);
	s->options.identity_xml = {};
	tg_buffer identity{};
	tg_status result = tg_copy(o->identity_xml, &identity);
	s->options.identity_xml = {identity.data, identity.size};
	s->binary = o->protocol == TG_PROTOCOL_PROTOBUF_ONLY;
	if (!s->options.uid || !s->options.callsign ||
		(o->username && (!s->options.username || !s->options.password)))
		result = TG_NO_MEMORY;
	if (result == TG_OK)
		result = tg_framer_create(
			s->binary ? TG_ENCODING_PROTOBUF_V1 : TG_ENCODING_XML,
			&l, &s->framer);
	if (result == TG_OK)
		result = initial_writes(s);
	if (result < 0) {
		tg_session_destroy(s);
		return result;
	}
	session_state(s);
	*out = s;
	return TG_OK;
}
static tg_status upgrade_request(tg_session *s, tg_bytes offer)
{
	cot_decoded_event *owner = nullptr;
	cot_status status =
		cot_decode(COT_FORMAT_XML, offer.data, offer.size, &owner);
	cot_buffer xml{};
	if (status == COT_STATUS_OK) {
		cot_event event = *cot_decoded_event_get(owner);
		cot_detail detail{};
		const char *body =
			"<TakControl><TakRequest version=\"1\"/></TakControl>";
		event.type = {"t-x-takp-q", 10};
		detail.xml = {body, strlen(body)};
		event.detail = &detail;
		status = cot_encode(COT_FORMAT_XML, &event, &xml);
	}
	tg_status result = status == COT_STATUS_OK
				   ? queue_event(s, {xml.data, xml.size}, 3)
				   : TG_MALFORMED;
	cot_buffer_destroy(&xml);
	cot_decoded_event_destroy(owner);
	return result;
}
tg_status tg_session_receive(tg_session *s, tg_bytes input, uint64_t now,
	size_t *consumed, tg_message *message)
{
	/*
	 * Check time/state, frame input and consume exact controls. Match
	 * negotiation UID/version; pause writes while pending, retain ordinary
	 * events, and switch at acceptance before processing its binary suffix.
	 * Track partial deadlines and separate control from combined envelopes.
	 */
	if (consumed)
		*consumed = 0;
	if (!s || !consumed || !message || !tg_empty(&message->bytes) ||
		!tg_span(input) || now < s->now)
		return TG_INVALID;
	*message = {};
	if (s->info.state == TG_SESSION_FAILED)
		return TG_BAD_STATE;
	s->now = now;
	while (*consumed < input.size) {
		tg_message framed{};
		size_t used = 0;
		tg_status result = tg_framer_feed(s->framer,
			{input.data + *consumed, input.size - *consumed}, &used,
			&framed);
		*consumed += used;
		if (result < 0) {
			s->info.state = TG_SESSION_FAILED;
			return result;
		}
		if (result == TG_MORE) {
			if (!s->partial)
				s->partial_start = now;
			s->partial = tg_framer_partial(s->framer);
			return TG_MORE;
		}
		s->partial = false;
		tg_xml_info info{};
		tg_buffer normalized{};
		tg_normalize_report report{};
		if (s->binary) {
			tg_buffer event{};
			int control = 0;
			result = tg_event_only(
				{framed.bytes.data, framed.bytes.size}, &event,
				&control);
			tg_buffer_release(&framed.bytes);
			framed.bytes = event;
			if (control)
				++s->info.controls_consumed;
			if (result == TG_NO_EVENT)
				continue;
			if (result == TG_OK &&
				tg_normalize(TG_ENCODING_PROTOBUF_V1,
					{event.data, event.size},
					TG_PRESERVE_OPAQUE, &s->limits,
					&normalized, &report) == TG_OK)
				tg_xml({normalized.data, normalized.size},
					&s->limits, &info);
		} else
			result = tg_xml({framed.bytes.data, framed.bytes.size},
				&s->limits, &info);
		if (result < 0) {
			tg_buffer_release(&framed.bytes);
			tg_buffer_release(&normalized);
			tg_xml_clear(&info);
			s->info.state = TG_SESSION_FAILED;
			return result;
		}
		int control = info.control;
		if (control)
			++s->info.controls_consumed;
		if (control == 1) {
			bool first = !s->info.protocol_offer_received;
			s->info.protocol_offer_received = 1;
			if (first &&
				s->options.protocol == TG_PROTOCOL_NEGOTIATED &&
				s->options.preference == TG_PREFER_PROTOBUF) {
				free(s->transaction);
				s->transaction = tg_strdup(info.uid);
				tg_bytes offer{
					framed.bytes.data, framed.bytes.size};
				result = s->transaction
						 ? upgrade_request(s, offer)
						 : TG_NO_MEMORY;
			}
		} else if (control == 2 && s->request_sent) {
			if (!s->transaction ||
				strcmp(s->transaction, info.uid) ||
				!info.response)
				result = TG_MALFORMED;
			else {
				s->request_sent = false;
				if (info.response == 1) {
					tg_framer_destroy(s->framer);
					s->framer = nullptr;
					s->binary = true;
					result = tg_framer_create(
						TG_ENCODING_PROTOBUF_V1,
						&s->limits, &s->framer);
				}
				session_state(s);
			}
		} else if (control == 5 && s->info.ping_outstanding) {
			s->info.ping_outstanding = 0;
			++s->info.pongs_received;
		}
		tg_xml_clear(&info);
		tg_buffer_release(&normalized);
		if (result < 0) {
			tg_buffer_release(&framed.bytes);
			s->info.state = TG_SESSION_FAILED;
			return result;
		}
		if (!control) {
			*message = framed;
			++s->info.events_received;
			return TG_OK;
		}
		tg_buffer_release(&framed.bytes);
	}
	return TG_NO_EVENT;
}
tg_status tg_session_peek_write(tg_session *s, tg_bytes *bytes)
{
	/* Borrow the next permitted write without changing its offset. */
	if (!bytes)
		return TG_INVALID;
	*bytes = {};
	if (!s)
		return TG_INVALID;
	if (s->info.state == TG_SESSION_FAILED)
		return TG_BAD_STATE;
	if (!s->head) {
		s->peeked = 0;
		return TG_MORE;
	}
	*bytes = {s->head->bytes.data + s->head->offset,
		s->head->bytes.size - s->head->offset};
	s->peeked = bytes->size;
	return TG_OK;
}
tg_status tg_session_consume_write(tg_session *s, size_t count, uint64_t now)
{
	/*
	 * Validate count against last peek and advance only submitted
	 * bytes. Start response timing on the final request byte; retain order.
	 */
	if (!s || now < s->now || count > s->peeked)
		return TG_INVALID;
	if (s->info.state == TG_SESSION_FAILED)
		return TG_BAD_STATE;
	s->now = now;
	if (!count)
		return TG_OK;
	upstream_write *w = s->head;
	w->offset += count;
	s->peeked = 0;
	if (w->offset != w->bytes.size)
		return TG_OK;
	if (w->kind == 1)
		s->info.credentials_sent = 1;
	if (w->kind == 3) {
		s->request_sent = true;
		s->request_time = now;
	}
	if (w->kind == 4) {
		s->info.ping_outstanding = 1;
		++s->info.pings_sent;
		s->last_ping = s->ping_time = now;
	}
	s->head = w->next;
	if (!s->head)
		s->tail = nullptr;
	memset(w->bytes.data, 0, w->bytes.size);
	tg_buffer_release(&w->bytes);
	free(w);
	session_state(s);
	return TG_OK;
}
tg_status tg_session_tick(tg_session *s, uint64_t now, uint64_t utc)
{
	/*
	 * Check absolute partial/negotiation deadlines without extending
	 * them on trickle input. Schedule explicitly configured maintenance in
	 * the current encoding; suspend all such writes during negotiation.
	 */
	if (!s || now < s->now ||
		(s->options.ping_interval_ms &&
			utc > UINT64_C(253402300779999)))
		return TG_INVALID;
	if (s->info.state == TG_SESSION_FAILED)
		return TG_BAD_STATE;
	s->now = now;
	if ((s->partial &&
		    now - s->partial_start >= s->options.partial_timeout_ms) ||
		(s->request_sent && now - s->request_time >=
					    s->options.response_timeout_ms) ||
		(s->info.ping_outstanding &&
			now - s->ping_time >= s->options.ping_timeout_ms)) {
		s->info.state = TG_SESSION_FAILED;
		return TG_TIMEOUT;
	}
	if (!s->options.ping_interval_ms || s->head || s->request_sent ||
		s->info.ping_outstanding ||
		now - s->last_ping < s->options.ping_interval_ms)
		return TG_OK;
	cot_event event{};
	event.type = {"t-x-c-t", 7};
	event.uid = {s->options.uid, strlen(s->options.uid)};
	event.how = {"m-g", 3};
	event.send_time_ms = event.start_time_ms = utc;
	event.stale_time_ms = utc + 20000;
	event.point.ce = event.point.le = COT_POINT_UNKNOWN_METRES;
	cot_buffer xml{};
	cot_status cs = cot_encode(COT_FORMAT_XML, &event, &xml);
	tg_status status = cs == COT_STATUS_OK
				   ? queue_event(s, {xml.data, xml.size}, 4)
				   : TG_UNSUPPORTED;
	cot_buffer_destroy(&xml);
	return status;
}
tg_status tg_session_reset(tg_session *s, uint64_t now)
{
	/*
	 * Reset all connection progress and queue credentials first
	 * again.
	 */
	if (!s || now < s->now)
		return TG_INVALID;
	clear_writes(s);
	free(s->transaction);
	s->transaction = nullptr;
	tg_framer_destroy(s->framer);
	s->framer = nullptr;
	s->binary = s->options.protocol == TG_PROTOCOL_PROTOBUF_ONLY;
	s->info = {};
	s->partial = false;
	s->request_sent = false;
	s->now = s->last_ping = now;
	tg_status status = tg_framer_create(
		s->binary ? TG_ENCODING_PROTOBUF_V1 : TG_ENCODING_XML,
		&s->limits, &s->framer);
	if (status == TG_OK)
		status = initial_writes(s);
	if (status < 0)
		s->info.state = TG_SESSION_FAILED;
	else
		session_state(s);
	return status;
}
tg_status tg_session_snapshot(const tg_session *s, tg_session_info *info)
{
	/* Copy nonsecret state/counters without changing the session. */
	if (!info)
		return TG_INVALID;
	*info = {};
	if (!s)
		return TG_INVALID;
	*info = s->info;
	for (upstream_write *w = s->head; w; w = w->next)
		info->pending_write_bytes += w->bytes.size - w->offset;
	return TG_OK;
}
void tg_session_destroy(tg_session *s)
{
	/* Erase copied secrets and free pending writes/parser/profile. */
	if (!s)
		return;
	clear_writes(s);
	tg_framer_destroy(s->framer);
	wipe((char *)s->options.password);
	free((char *)s->options.username);
	free((char *)s->options.uid);
	free((char *)s->options.callsign);
	free((void *)s->options.identity_xml.data);
	free(s->transaction);
	free(s);
}
