#include "internal.h"
#include <limits.h>

bool tg_span(tg_bytes b)
{
	return b.data || !b.size;
}
bool tg_empty(const tg_buffer *b)
{
	return b && !b->data && !b->size;
}
char *tg_strdup(const char *s)
{
	if (!s)
		return nullptr;
	size_t n = strlen(s) + 1;
	char *p = static_cast<char *>(malloc(n));
	if (p)
		memcpy(p, s, n);
	return p;
}
bool tg_get_limits(const tg_limits *in, tg_limits *out)
{
	tg_limits defaults = TG_LIMITS_DEFAULT;
	*out = in ? *in : defaults;
	return out->max_message_bytes && out->max_message_bytes <= UINT32_MAX &&
	       out->max_xml_depth && out->retained_bytes && out->queue_bytes &&
	       out->queue_records && out->max_outputs;
}
bool tg_append(tg_vec *v, const void *p, size_t n)
{
	if (!n)
		return true;
	if (!p || n > SIZE_MAX - v->size - 1)
		return false;
	size_t need = v->size + n + 1;
	if (need > v->capacity) {
		size_t capacity = v->capacity ? v->capacity : 256;
		while (capacity < need && capacity <= SIZE_MAX / 2)
			capacity *= 2;
		if (capacity < need)
			capacity = need;
		void *memory = realloc(v->data, capacity);
		if (!memory)
			return false;
		v->data = static_cast<uint8_t *>(memory);
		v->capacity = capacity;
	}
	memcpy(v->data + v->size, p, n);
	v->size += n;
	v->data[v->size] = 0;
	return true;
}
bool tg_text(tg_vec *v, const char *s)
{
	return tg_append(v, s, strlen(s));
}
bool tg_printf(tg_vec *v, const char *format, ...)
{
	va_list args, copy;
	va_start(args, format);
	va_copy(copy, args);
	int n = vsnprintf(nullptr, 0, format, args);
	va_end(args);
	if (n < 0) {
		va_end(copy);
		return false;
	}
	char *p = static_cast<char *>(malloc(static_cast<size_t>(n) + 1));
	if (!p) {
		va_end(copy);
		return false;
	}
	vsnprintf(p, static_cast<size_t>(n) + 1, format, copy);
	va_end(copy);
	bool ok = tg_append(v, p, static_cast<size_t>(n));
	free(p);
	return ok;
}
void tg_vec_free(tg_vec *v)
{
	free(v->data);
	*v = {};
}
tg_status tg_copy(tg_bytes in, tg_buffer *out)
{
	if (!tg_empty(out) || !tg_span(in))
		return TG_INVALID;
	if (!in.size)
		return TG_OK;
	out->data = static_cast<uint8_t *>(malloc(in.size));
	if (!out->data)
		return TG_NO_MEMORY;
	memcpy(out->data, in.data, in.size);
	out->size = in.size;
	return TG_OK;
}
void tg_buffer_release(tg_buffer *buffer)
{
	/*
	 * Release only library-owned storage, then zero its descriptor.
	 */
	if (buffer) {
		free(buffer->data);
		*buffer = {};
	}
}
bool tg_utf8(tg_bytes b)
{
	if (!tg_span(b))
		return false;
	for (size_t i = 0; i < b.size;) {
		uint8_t c = b.data[i++];
		if (!c)
			return false;
		if (c < 0x80)
			continue;
		unsigned n;
		uint32_t value, minimum;
		if (c >= 0xc2 && c <= 0xdf) {
			n = 1;
			value = c & 31;
			minimum = 0x80;
		} else if (c >= 0xe0 && c <= 0xef) {
			n = 2;
			value = c & 15;
			minimum = 0x800;
		} else if (c >= 0xf0 && c <= 0xf4) {
			n = 3;
			value = c & 7;
			minimum = 0x10000;
		} else
			return false;
		if (n > b.size - i)
			return false;
		while (n--) {
			c = b.data[i++];
			if ((c & 0xc0) != 0x80)
				return false;
			value = (value << 6) | (c & 63);
		}
		if (value < minimum || value > 0x10ffff ||
			(value >= 0xd800 && value <= 0xdfff))
			return false;
	}
	return true;
}
bool tg_escape(tg_vec *v, const char *s, bool json)
{
	if (!s)
		s = "";
	for (; *s; ++s) {
		const char *p = nullptr;
		if (json) {
			if (*s == '"')
				p = "\\\"";
			else if (*s == '\\')
				p = "\\\\";
			else if (static_cast<uint8_t>(*s) < 32) {
				if (!tg_printf(v, "\\u%04x", (unsigned char)*s))
					return false;
				continue;
			}
		} else {
			if (*s == '&')
				p = "&amp;";
			else if (*s == '<')
				p = "&lt;";
			else if (*s == '>')
				p = "&gt;";
			else if (*s == '"')
				p = "&quot;";
			else if (*s == '\'')
				p = "&apos;";
		}
		if (!(p ? tg_text(v, p) : tg_append(v, s, 1)))
			return false;
	}
	return true;
}
