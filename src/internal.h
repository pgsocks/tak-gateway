#ifndef TG_INTERNAL_H
#define TG_INTERNAL_H
#include <tak_gateway/gateway.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

struct tg_vec {
	uint8_t *data;
	size_t size;
	size_t capacity;
};
bool tg_append(tg_vec *, const void *, size_t);
bool tg_text(tg_vec *, const char *);
bool tg_printf(tg_vec *, const char *, ...);
void tg_vec_free(tg_vec *);
tg_status tg_copy(tg_bytes, tg_buffer *);
bool tg_span(tg_bytes);
bool tg_empty(const tg_buffer *);
bool tg_get_limits(const tg_limits *, tg_limits *);
char *tg_strdup(const char *);
bool tg_utf8(tg_bytes);
bool tg_escape(tg_vec *, const char *, bool json = false);
struct tg_xml_info {
	char *uid;
	char *type;
	int control;
	int version;
	int response;
};
bool tg_framer_partial(const tg_framer *);
tg_status tg_xml(tg_bytes, const tg_limits *, tg_xml_info *);
void tg_xml_clear(tg_xml_info *);
tg_status tg_event_only(tg_bytes, tg_buffer *, int *);
tg_status tg_binary_xml(tg_bytes, tg_buffer *);
#endif
