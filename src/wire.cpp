#include "internal.h"
#include <expat.h>

struct tg_framer {
	tg_encoding encoding;
	tg_limits limits;
	tg_vec bytes;
	XML_Parser parser;
	uint32_t depth;
	bool done;
	bool failed;
	bool finished;
	tg_status error;
	uint64_t length;
	unsigned varbytes;
	bool header;
	size_t parsed;
	size_t token;
	unsigned lexical;
	uint8_t quote;
};
static void XMLCALL frame_start(void *user, const char *name, const char **)
{
	tg_framer *f = static_cast<tg_framer *>(user);
	if ((!f->depth && strcmp(name, "event")) ||
		++f->depth > f->limits.max_xml_depth) {
		f->error = f->depth > f->limits.max_xml_depth ? TG_LIMIT
							      : TG_MALFORMED;
		XML_StopParser(f->parser, XML_FALSE);
	}
}
static void XMLCALL frame_end(void *user, const char *)
{
	tg_framer *f = static_cast<tg_framer *>(user);
	f->done = --f->depth == 0;
}
static void XMLCALL forbidden(
	void *user, const char *, const char *, const char *, int)
{
	tg_framer *f = static_cast<tg_framer *>(user);
	f->error = TG_MALFORMED;
	XML_StopParser(f->parser, XML_FALSE);
}
static bool frame_parser(tg_framer *f)
{
	if (f->parser)
		XML_ParserFree(f->parser);
	f->parser = XML_ParserCreateNS(nullptr, '\x1f');
	if (!f->parser)
		return false;
	XML_SetReparseDeferralEnabled(f->parser, XML_FALSE);
	XML_SetUserData(f->parser, f);
	XML_SetElementHandler(f->parser, frame_start, frame_end);
	XML_SetStartDoctypeDeclHandler(f->parser, forbidden);
	XML_SetParamEntityParsing(f->parser, XML_PARAM_ENTITY_PARSING_NEVER);
	return true;
}
/* Feed complete lexical tokens so Expat never reparses growing tokens on
 * one-byte network reads. Expat remains responsible for XML validation.
 */
static bool parse_xml(tg_framer *f, size_t end)
{
	while (f->parsed < end) {
		size_t count = end - f->parsed;
		if (count > INT32_MAX)
			count = INT32_MAX;
		if (XML_Parse(f->parser,
			    (const char *)f->bytes.data + f->parsed, (int)count,
			    0) == XML_STATUS_ERROR) {
			if (!f->error)
				f->error = TG_MALFORMED;
			return false;
		}
		f->parsed += count;
	}
	return true;
}
static bool xml_byte(tg_framer *f, uint8_t c)
{
	size_t n = f->bytes.size;
	if ((n == 1 && c != '<' && c != 0xef) ||
		(n == 2 && f->bytes.data[0] == 0xef && c != 0xbb) ||
		(n == 3 && f->bytes.data[0] == 0xef && c != 0xbf)) {
		f->error = TG_MALFORMED;
		return false;
	}
	if (!f->lexical) {
		if (c != '<')
			return true;
		if (!parse_xml(f, n - 1))
			return false;
		f->lexical = 1;
		f->token = n - 1;
		return true;
	}
	size_t length = n - f->token;
	const uint8_t *token = f->bytes.data + f->token;
	if (f->lexical == 1) {
		if (length == 2 && c == '?')
			f->lexical = 3;
		else if (length == 2 && c != '!')
			f->lexical = 2;
		else if (length >= 3 && token[1] == '!') {
			if (length <= 4 && !memcmp(token, "<!--", length)) {
				if (length == 4)
					f->lexical = 4;
			} else if (length <= 9 &&
				   !memcmp(token, "<![CDATA[", length)) {
				if (length == 9)
					f->lexical = 5;
			} else {
				f->error = TG_MALFORMED;
				return false;
			}
		}
	}
	bool end = false;
	if (f->lexical == 2) {
		if (f->quote) {
			if (c == f->quote)
				f->quote = 0;
		} else if (c == '\'' || c == '"') {
			f->quote = c;
		} else if (c == '>') {
			end = true;
		}
	} else if (f->lexical == 3) {
		end = length >= 3 && c == '>' && token[length - 2] == '?';
	} else if (f->lexical == 4) {
		end = length >= 7 && c == '>' && token[length - 2] == '-' &&
		      token[length - 3] == '-';
	} else if (f->lexical == 5) {
		end = length >= 12 && c == '>' && token[length - 2] == ']' &&
		      token[length - 3] == ']';
	}
	if (!end)
		return true;
	f->lexical = 0;
	return parse_xml(f, n);
}
tg_status tg_framer_create(
	tg_encoding encoding, const tg_limits *limits, tg_framer **out)
{
	/*
	 * Validate/copy limits; allocate independent bounded parser
	 * state.
	 */
	if (!out)
		return TG_INVALID;
	*out = nullptr;
	tg_limits l;
	if (!tg_get_limits(limits, &l) ||
		(encoding != TG_ENCODING_XML &&
			encoding != TG_ENCODING_PROTOBUF_V1))
		return TG_INVALID;
	tg_framer *f = static_cast<tg_framer *>(calloc(1, sizeof(*f)));
	if (!f)
		return TG_NO_MEMORY;
	f->encoding = encoding;
	f->limits = l;
	if (encoding == TG_ENCODING_XML && !frame_parser(f)) {
		free(f);
		return TG_NO_MEMORY;
	}
	*out = f;
	return TG_OK;
}
tg_status tg_framer_reset(tg_framer *f)
{
	/* Discard partial bytes/failure and restore original encoding. */
	if (!f)
		return TG_INVALID;
	tg_vec_free(&f->bytes);
	f->depth = 0;
	f->done = false;
	f->failed = false;
	f->finished = false;
	f->error = TG_OK;
	f->length = 0;
	f->varbytes = 0;
	f->header = false;
	f->parsed = 0;
	f->token = 0;
	f->lexical = 0;
	f->quote = 0;
	if (f->encoding == TG_ENCODING_XML && !frame_parser(f)) {
		f->failed = true;
		return TG_NO_MEMORY;
	}
	return TG_OK;
}
tg_status tg_framer_feed(
	tg_framer *f, tg_bytes input, size_t *consumed, tg_message *message)
{
	/*
	 * Validate arguments without mutation. Incrementally recognize
	 * one
	 * structural XML document or bounded TAK stream frame; retain partial
	 * state, report exact consumption, and fail closed on malformed input.
	 */
	if (consumed)
		*consumed = 0;
	if (!f || !consumed || !message || message->bytes.data ||
		message->bytes.size || !tg_span(input))
		return TG_INVALID;
	*message = {};
	if (f->failed || f->finished)
		return TG_BAD_STATE;
	for (size_t i = 0; i < input.size; ++i) {
		uint8_t c = input.data[i];
		*consumed = i + 1;
		if (f->encoding == TG_ENCODING_XML) {
			if (!f->bytes.size && (c == ' ' || c == '\n' ||
						      c == '\t' || c == '\r'))
				continue;
			if (f->bytes.size == f->limits.max_message_bytes) {
				f->error = TG_LIMIT;
				break;
			}
			if (!tg_append(&f->bytes, &c, 1)) {
				f->error = TG_NO_MEMORY;
				break;
			}
			if (!xml_byte(f, c))
				break;
		} else if (!f->header) {
			if (!f->varbytes && !f->bytes.size) {
				if (c != 0xbf) {
					f->error = TG_MALFORMED;
					break;
				}
				if (!tg_append(&f->bytes, &c, 1)) {
					f->error = TG_NO_MEMORY;
					break;
				}
				continue;
			}
			if (f->varbytes == 10 || (f->varbytes == 9 && c > 1)) {
				f->error = TG_MALFORMED;
				break;
			}
			f->length |= (uint64_t)(c & 127) << (7 * f->varbytes++);
			if (c & 128)
				continue;
			if (!f->length) {
				f->error = TG_MALFORMED;
				break;
			}
			if (f->length > f->limits.max_message_bytes) {
				f->error = TG_LIMIT;
				break;
			}
			f->bytes.size = 0;
			f->header = true;
			continue;
		} else {
			if (!tg_append(&f->bytes, &c, 1)) {
				f->error = TG_NO_MEMORY;
				break;
			}
			f->done = f->bytes.size == f->length;
		}
		if (f->done) {
			message->encoding = f->encoding;
			message->bytes = {f->bytes.data, f->bytes.size};
			f->bytes = {};
			tg_status s = tg_framer_reset(f);
			if (s < 0) {
				tg_buffer_release(&message->bytes);
				*message = {};
				return s;
			}
			return TG_OK;
		}
	}
	if (f->error) {
		f->failed = true;
		return f->error;
	}
	return TG_MORE;
}
bool tg_framer_partial(const tg_framer *f)
{
	return f && (f->bytes.size || f->varbytes || f->header);
}
tg_status tg_framer_finish(tg_framer *f)
{
	/*
	 * Distinguish clean terminal EOF, partial input, and failed
	 * state.
	 */
	if (!f)
		return TG_INVALID;
	if (f->failed)
		return TG_BAD_STATE;
	if (f->bytes.size || f->varbytes || f->header) {
		f->failed = true;
		return TG_TRUNCATED;
	}
	f->finished = true;
	return TG_OK;
}
void tg_framer_destroy(tg_framer *f)
{
	/* Free parser state and buffered bytes; accept NULL. */
	if (!f)
		return;
	if (f->parser)
		XML_ParserFree(f->parser);
	tg_vec_free(&f->bytes);
	free(f);
}
tg_status tg_datagram_decode(tg_datagram_mode mode, tg_bytes input,
	int truncated, const tg_limits *limits, tg_message *message)
{
	/*
	 * Reject OS truncation atomically. Classify one XML document or
	 * BF/version/BF mesh envelope, validate bounds, and copy payload only.
	 * Never join datagrams or interpret the mesh version as a frame length.
	 */
	if (!message || !tg_empty(&message->bytes))
		return TG_INVALID;
	*message = {};
	tg_limits l;
	if (!tg_span(input) || !tg_get_limits(limits, &l) ||
		mode < TG_DATAGRAM_AUTO || mode > TG_DATAGRAM_TAK_V1 ||
		(truncated != 0 && truncated != 1))
		return TG_INVALID;
	if (truncated)
		return TG_TRUNCATED;
	if (!input.size)
		return TG_MALFORMED;
	if (input.data[0] == 0xbf && mode != TG_DATAGRAM_XML) {
		size_t i = 1;
		uint64_t version = 0;
		unsigned count = 0;
		for (; i < input.size; ++i) {
			uint8_t c = input.data[i];
			if (count == 10 || (count == 9 && c > 1))
				return TG_MALFORMED;
			version |= (uint64_t)(c & 127) << (7 * count++);
			if (!(c & 128))
				break;
		}
		if (i + 2 >= input.size || input.data[++i] != 0xbf)
			return TG_MALFORMED;
		if (version != 1)
			return TG_UNSUPPORTED;
		++i;
		if (input.size - i > l.max_message_bytes)
			return TG_LIMIT;
		tg_status s = tg_copy(
			{input.data + i, input.size - i}, &message->bytes);
		if (s == TG_OK)
			message->encoding = TG_ENCODING_PROTOBUF_V1;
		return s;
	}
	if (mode == TG_DATAGRAM_TAK_V1)
		return TG_MALFORMED;
	if (input.size > l.max_message_bytes)
		return TG_LIMIT;
	tg_framer *f = nullptr;
	tg_status s = tg_framer_create(TG_ENCODING_XML, &l, &f);
	if (s < 0)
		return s;
	size_t used = 0;
	tg_message framed{};
	s = tg_framer_feed(f, input, &used, &framed);
	if (s == TG_MORE)
		s = tg_framer_finish(f) == TG_OK ? TG_MALFORMED : TG_TRUNCATED;
	if (s == TG_OK) {
		for (size_t i = used; i < input.size; ++i) {
			uint8_t c = input.data[i];
			if (c != ' ' && c != '\n' && c != '\t' && c != '\r')
				s = TG_MALFORMED;
		}
	}
	tg_buffer_release(&framed.bytes);
	tg_framer_destroy(f);
	if (s != TG_OK)
		return s;
	s = tg_copy(input, &message->bytes);
	if (s == TG_OK)
		message->encoding = TG_ENCODING_XML;
	return s;
}
