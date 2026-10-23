#include "internal.h"
#include <expat.h>
#include <cot/cot.h>

struct xml_scan {
	XML_Parser parser;
	tg_xml_info *info;
	unsigned depth;
	bool detail;
	bool control;
	bool failed;
};
static const char *attribute(const char **attrs, const char *key)
{
	for (size_t i = 0; attrs[i]; i += 2)
		if (!strcmp(attrs[i], key))
			return attrs[i + 1];
	return nullptr;
}
static void XMLCALL inspect_start(
	void *user, const char *name, const char **attrs)
{
	xml_scan *s = static_cast<xml_scan *>(user);
	++s->depth;
	if (s->depth == 1) {
		const char *uid = attribute(attrs, "uid");
		const char *type = attribute(attrs, "type");
		s->info->uid = tg_strdup(uid ? uid : "");
		s->info->type = tg_strdup(type ? type : "");
		if (!s->info->uid || !s->info->type)
			s->failed = true;
	}
	if (s->depth == 2)
		s->detail = !strcmp(name, "detail");
	if (s->depth == 3)
		s->control = s->detail && !strcmp(name, "TakControl");
	if (s->depth == 4 && s->control) {
		const char *version = attribute(attrs, "version");
		if (!strcmp(name, "TakProtocolSupport") && version &&
			!strcmp(version, "1")) {
			s->info->version = 1;
			s->info->control = 1;
		} else if (!strcmp(name, "TakRequest")) {
			s->info->control = 3;
		} else if (!strcmp(name, "TakResponse")) {
			s->info->control = 2;
			const char *v = attribute(attrs, "status");
			s->info->response = v && !strcmp(v, "true")    ? 1
					    : v && !strcmp(v, "false") ? -1
								       : 0;
		}
	}
}
static void XMLCALL inspect_end(void *user, const char *)
{
	--static_cast<xml_scan *>(user)->depth;
}
void tg_xml_clear(tg_xml_info *info)
{
	if (!info)
		return;
	free(info->uid);
	free(info->type);
	*info = {};
}
tg_status tg_xml(tg_bytes input, const tg_limits *limits, tg_xml_info *info)
{
	*info = {};
	tg_message message{};
	tg_status status =
		tg_datagram_decode(TG_DATAGRAM_XML, input, 0, limits, &message);
	tg_buffer_release(&message.bytes);
	if (status != TG_OK)
		return status;
	XML_Parser parser = XML_ParserCreateNS(nullptr, '\x1f');
	if (!parser)
		return TG_NO_MEMORY;
	xml_scan scan{parser, info, 0, false, false, false};
	XML_SetUserData(parser, &scan);
	XML_SetElementHandler(parser, inspect_start, inspect_end);
	size_t start = 0;
	while (start < input.size &&
		(input.data[start] == ' ' || input.data[start] == '\r' ||
			input.data[start] == '\n' || input.data[start] == '\t'))
		++start;
	if (input.size - start > INT32_MAX ||
		XML_Parse(parser, (const char *)input.data + start,
			(int)(input.size - start), 1) != XML_STATUS_OK)
		status = TG_MALFORMED;
	XML_ParserFree(parser);
	if (scan.failed)
		status = TG_NO_MEMORY;
	if (status < 0) {
		tg_xml_clear(info);
		return status;
	}
	if (info->control &&
		((info->control == 1 && strcmp(info->type, "t-x-takp-v")) ||
			(info->control == 2 &&
				strcmp(info->type, "t-x-takp-r")) ||
			(info->control == 3 &&
				strcmp(info->type, "t-x-takp-q"))))
		info->control = 0;
	if (!strcmp(info->type, "t-x-c-t"))
		info->control = 4;
	if (!strcmp(info->type, "t-x-c-t-r") && !strcmp(info->uid, "takPong"))
		info->control = 5;
	return TG_OK;
}
static bool varint(tg_bytes in, size_t *at, uint64_t *out)
{
	*out = 0;
	for (unsigned i = 0; i < 10 && *at < in.size; ++i) {
		uint8_t c = in.data[(*at)++];
		if (i == 9 && c > 1)
			return false;
		*out |= (uint64_t)(c & 127) << (7 * i);
		if (!(c & 128))
			return true;
	}
	return false;
}
static bool wire_skip(
	tg_bytes in, size_t *at, unsigned type, uint64_t tag, unsigned depth)
{
	uint64_t n;
	if (depth > 128)
		return false;
	switch (type) {
	case 0:
		return varint(in, at, &n);
	case 1:
		n = 8;
		break;
	case 2:
		if (!varint(in, at, &n))
			return false;
		break;
	case 5:
		n = 4;
		break;
	case 3:
		while (*at < in.size) {
			uint64_t key;
			if (!varint(in, at, &key) || !(key >> 3))
				return false;
			if ((key & 7) == 4)
				return (key >> 3) == tag;
			if (!wire_skip(in, at, key & 7, key >> 3, depth + 1))
				return false;
		}
		return false;
	default:
		return false;
	}
	if (n > in.size - *at)
		return false;
	*at += (size_t)n;
	return true;
}
static bool control_wire(tg_bytes input)
{
	for (size_t at = 0; at < input.size;) {
		uint64_t key;
		if (!varint(input, &at, &key) || !(key >> 3) ||
			(key >> 3) > 0x1fffffff ||
			!wire_skip(input, &at, key & 7, key >> 3, 0))
			return false;
	}
	return true;
}
tg_status tg_event_only(tg_bytes input, tg_buffer *out, int *control)
{
	tg_vec result{};
	bool event = false;
	*control = 0;
	if (!tg_span(input) || !input.size)
		return TG_MALFORMED;
	for (size_t at = 0; at < input.size;) {
		size_t start = at;
		uint64_t key;
		if (!varint(input, &at, &key) || !(key >> 3) ||
			(key >> 3) > 0x1fffffff ||
			!wire_skip(input, &at, key & 7, key >> 3, 0)) {
			tg_vec_free(&result);
			return TG_MALFORMED;
		}
		if (key == 10) {
			size_t payload = start;
			uint64_t field_key, length;
			if (!varint(input, &payload, &field_key) ||
				!varint(input, &payload, &length) ||
				!control_wire({input.data + payload,
					(size_t)length})) {
				tg_vec_free(&result);
				return TG_MALFORMED;
			}
			*control = 1;
			continue;
		}
		if (key == 18)
			event = true;
		if (!tg_append(&result, input.data + start, at - start)) {
			tg_vec_free(&result);
			return TG_NO_MEMORY;
		}
	}
	if (!event) {
		tg_vec_free(&result);
		return *control ? TG_NO_EVENT : TG_MALFORMED;
	}
	*out = {result.data, result.size};
	return TG_OK;
}
static tg_status codec_status(cot_status s)
{
	switch (s) {
	case COT_STATUS_OK:
		return TG_OK;
	case COT_STATUS_ALLOCATION_FAILURE:
		return TG_NO_MEMORY;
	case COT_STATUS_UNSUPPORTED_CONTENT:
		return TG_UNSUPPORTED;
	case COT_STATUS_INVALID_ARGUMENT:
		return TG_INVALID;
	default:
		return TG_MALFORMED;
	}
}
tg_status tg_binary_xml(tg_bytes input, tg_buffer *out)
{
	cot_decoded_event *owner = nullptr;
	cot_status status =
		cot_decode(COT_FORMAT_XML, input.data, input.size, &owner);
	cot_buffer binary{};
	if (status == COT_STATUS_OK)
		status = cot_encode(COT_FORMAT_PROTOBUF,
			cot_decoded_event_get(owner), &binary);
	tg_status s = codec_status(status);
	if (s == TG_OK)
		s = tg_copy({binary.data, binary.size}, out);
	cot_buffer_destroy(&binary);
	cot_decoded_event_destroy(owner);
	return s;
}
static bool append_base64(tg_vec *out, tg_bytes in)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklm"
				       "nopqrstuvwxyz0123456789+/";
	for (size_t i = 0; i < in.size; i += 3) {
		size_t n = in.size - i;
		uint32_t bits = (uint32_t)in.data[i] << 16;
		if (n > 1)
			bits |= (uint32_t)in.data[i + 1] << 8;
		if (n > 2)
			bits |= in.data[i + 2];
		char text[4] = {alphabet[bits >> 18],
			alphabet[(bits >> 12) & 63],
			n > 1 ? alphabet[(bits >> 6) & 63] : '=',
			n > 2 ? alphabet[bits & 63] : '='};
		if (!tg_append(out, text, 4))
			return false;
	}
	return true;
}
struct conflict_scan {
	const cot_detail *detail;
	unsigned depth;
	bool loss;
};
static void XMLCALL conflict_start(void *user, const char *name, const char **)
{
	conflict_scan *s = static_cast<conflict_scan *>(user);
	if (++s->depth != 2)
		return;
	const cot_detail *d = s->detail;
	if ((!strcmp(name, "contact") && d->contact) ||
		(!strcmp(name, "__group") && d->group) ||
		(!strcmp(name, "precisionlocation") && d->precision_location) ||
		(!strcmp(name, "status") && d->status) ||
		(!strcmp(name, "takv") && d->takv) ||
		(!strcmp(name, "track") && d->track))
		s->loss = true;
}
static void XMLCALL conflict_end(void *user, const char *)
{
	--static_cast<conflict_scan *>(user)->depth;
}
static bool detail_conflict(const cot_detail *d)
{
	if (!d || !d->xml.size)
		return false;
	XML_Parser parser = XML_ParserCreateNS(nullptr, '\x1f');
	if (!parser)
		return true;
	conflict_scan scan{d, 0, false};
	XML_SetUserData(parser, &scan);
	XML_SetElementHandler(parser, conflict_start, conflict_end);
	XML_Parse(parser, "<detail>", 8, 0);
	if (d->xml.size <= INT32_MAX)
		XML_Parse(parser, d->xml.data, (int)d->xml.size, 0);
	XML_Parse(parser, "</detail>", 9, 1);
	XML_ParserFree(parser);
	return scan.loss;
}
tg_status tg_normalize(tg_encoding encoding, tg_bytes input,
	tg_preservation_policy policy, const tg_limits *limits, tg_buffer *xml,
	tg_normalize_report *report)
{
	/*
	 * Preserve validated XML bytes and consume only exact controls.
	 * Scan Protobuf envelope without losing unknown/repeated event data;
	 * strip recognized controls, then compose the pinned cot codec. Reject
	 * invalid core data. Preserve unmapped bytes opaquely or reject loss.
	 * Enforce final XML limits after expansion; publish no partial output.
	 */
	if (!tg_empty(xml) || !report)
		return TG_INVALID;
	*report = {};
	tg_limits l;
	if (!tg_span(input) || !tg_get_limits(limits, &l) ||
		(policy != TG_PRESERVE_OPAQUE && policy != TG_REJECT_LOSSY))
		return TG_INVALID;
	if (input.size > l.max_message_bytes)
		return TG_LIMIT;
	if (encoding == TG_ENCODING_XML) {
		tg_xml_info info{};
		tg_status s = tg_xml(input, &l, &info);
		int control = info.control;
		tg_xml_clear(&info);
		if (s < 0)
			return s;
		if (control) {
			report->had_control = 1;
			return TG_NO_EVENT;
		}
		return tg_copy(input, xml);
	}
	if (encoding != TG_ENCODING_PROTOBUF_V1)
		return TG_INVALID;
	tg_buffer event{};
	int control = 0;
	tg_status result = tg_event_only(input, &event, &control);
	if (result == TG_NO_EVENT)
		report->had_control = control;
	if (result != TG_OK)
		return result;
	cot_decoded_event *owner = nullptr;
	cot_diagnostic diagnostic{};
	cot_status s = cot_decode_ex(COT_FORMAT_PROTOBUF, event.data,
		event.size, &owner, &diagnostic);
	cot_buffer output{};
	uint64_t unknown = diagnostic.unknown_protobuf_fields;
	bool lossy = unknown != 0;
	if (s == COT_STATUS_OK) {
		cot_event view = *cot_decoded_event_get(owner);
		cot_detail detail{};
		if (view.detail) {
			detail = *view.detail;
			lossy = lossy || detail_conflict(&detail);
			if (detail.extension_count) {
				lossy = true;
				detail.extensions = nullptr;
				detail.extension_count = 0;
			}
			view.detail = &detail;
		}
		s = cot_encode(COT_FORMAT_XML, &view, &output);
		if (s == COT_STATUS_UNSUPPORTED_CONTENT && view.detail &&
			detail.xml.size) {
			/* A conflicting residual subtree is retained in the
			 * opaque envelope. Keep the typed representation in
			 * normal XML. */
			detail.xml = {};
			lossy = true;
			s = cot_encode(COT_FORMAT_XML, &view, &output);
		}
	}
	result = codec_status(s);
	if (result == TG_OK && lossy && policy == TG_REJECT_LOSSY)
		result = TG_LOSSY;
	if (result == TG_OK && lossy) {
		tg_vec terminated{};
		if (!tg_append(&terminated, output.data, output.size)) {
			cot_buffer_destroy(&output);
			cot_decoded_event_destroy(owner);
			tg_buffer_release(&event);
			return TG_NO_MEMORY;
		}
		const char *text = (const char *)terminated.data;
		const char *close = strstr(text, "</detail>");
		const char *empty = strstr(text, "<detail/>");
		const char *end = strstr(text, "</event>");
		size_t at = close   ? (size_t)(close - text)
			    : empty ? (size_t)(empty - text)
			    : end   ? (size_t)(end - text)
				    : 0;
		bool wrap = !close;
		tg_vec full{};
		bool ok = at && tg_append(&full, output.data, at);
		if (wrap)
			ok = ok && tg_text(&full, "<detail>");
		ok = ok &&
		     tg_text(&full, "<gw_preserved:protobuf xmlns:gw_preserved="
				    "\"urn:tak-gateway:preservation:1\" "
				    "protocolVersion=\"1\" "
				    "encoding=\"base64\" "
				    "reason=\"unmapped-fields\">") &&
		     append_base64(&full, {event.data, event.size}) &&
		     tg_text(&full, "</gw_preserved:protobuf>");
		if (wrap)
			ok = ok && tg_text(&full, "</detail>");
		size_t skip = empty ? 9 : 0;
		ok = ok && tg_append(&full, output.data + at + skip,
				   output.size - at - skip);
		if (!ok)
			result = TG_NO_MEMORY;
		else if (full.size > l.max_message_bytes)
			result = TG_LIMIT;
		else {
			*xml = {full.data, full.size};
			full = {};
		}
		tg_vec_free(&full);
		tg_vec_free(&terminated);
	} else if (result == TG_OK) {
		if (output.size > l.max_message_bytes)
			result = TG_LIMIT;
		else
			result = tg_copy({output.data, output.size}, xml);
	}
	if (result == TG_OK)
		*report = {unknown, control, lossy ? 1 : 0};
	cot_buffer_destroy(&output);
	cot_decoded_event_destroy(owner);
	tg_buffer_release(&event);
	return result;
}
