#include "managed_runtime.h"
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

static void *config_alloc(tg_config *c, size_t n)
{
	config_block *b = static_cast<config_block *>(malloc(sizeof(*b)));
	if (!b)
		return nullptr;
	b->memory = calloc(1, n ? n : 1);
	if (!b->memory) {
		free(b);
		return nullptr;
	}
	b->next = c->blocks;
	c->blocks = b;
	return b->memory;
}
const char *tg_config_string(tg_config *c, const char *s)
{
	if (!s)
		return nullptr;
	char *p = static_cast<char *>(config_alloc(c, strlen(s) + 1));
	if (p)
		strcpy(p, s);
	return p;
}
static bool absolute(const char *path)
{
	if (!path || !*path)
		return false;
#ifdef _WIN32
	return (path[0] && path[1] == ':' &&
		       (path[2] == '/' || path[2] == '\\')) ||
	       (path[0] == '\\' && path[1] == '\\');
#else
	return *path == '/';
#endif
}
static const char *resolve_path(tg_config *c, const char *path)
{
	if (!path || absolute(path))
		return path;
	tg_vec full{};
	if (!tg_printf(&full, "%s/%s", c->base, path))
		return nullptr;
	const char *result = tg_config_string(c, (const char *)full.data);
	tg_vec_free(&full);
	return result;
}
static tg_status error(tg_diagnostic *d, tg_status s, const char *key)
{
	if (d) {
		*d = {};
		d->status = s;
		snprintf(d->key, sizeof(d->key), "%s", key ? key : "");
	}
	return s;
}
struct reader {
	tg_config *c;
	tg_diagnostic *d;
	tg_status status;
};
static bool schema(
	reader *r, toml_table_t *t, const char *allowed, const char *prefix)
{
	if (!t)
		return true;
	for (int i = 0;; ++i) {
		const char *key = toml_key_in(t, i);
		if (!key)
			break;
		bool found = false;
		for (const char *p = allowed; *p;) {
			const char *end = strchr(p, ' ');
			size_t n = end ? (size_t)(end - p) : strlen(p);
			if (strlen(key) == n && !memcmp(key, p, n))
				found = true;
			p += n;
			if (*p)
				++p;
		}
		if (!found) {
			char path[512];
			snprintf(path, sizeof(path), "%s%s%s", prefix,
				*prefix ? "." : "", key);
			r->status = error(r->d, TG_INVALID, path);
			return false;
		}
	}
	return true;
}
static void invalid(reader *r, const char *prefix, const char *key,
	tg_status s = TG_INVALID)
{
	if (r->status != TG_OK)
		return;
	char path[512];
	snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", key);
	r->status = error(r->d, s, path);
}
static const char *string_in(reader *r, toml_table_t *t, const char *key,
	const char *prefix, const char *fallback = nullptr, bool path = false)
{
	if (!t || !toml_key_exists(t, key))
		return tg_config_string(r->c, fallback);
	toml_datum_t v = toml_string_in(t, key);
	if (!v.ok || !v.u.s || !*v.u.s ||
		!tg_utf8({(const uint8_t *)v.u.s, strlen(v.u.s)})) {
		if (v.ok)
			free(v.u.s);
		invalid(r, prefix, key);
		return nullptr;
	}
	const char *s = tg_config_string(r->c, v.u.s);
	free(v.u.s);
	if (!s) {
		r->status = error(r->d, TG_NO_MEMORY, prefix);
		return nullptr;
	}
	return path ? resolve_path(r->c, s) : s;
}
static int64_t number(reader *r, toml_table_t *t, const char *key,
	const char *prefix, int64_t fallback, int64_t minimum, int64_t maximum)
{
	if (!t || !toml_key_exists(t, key))
		return fallback;
	toml_datum_t v = toml_int_in(t, key);
	if (!v.ok || v.u.i < minimum || v.u.i > maximum) {
		invalid(r, prefix, key);
		return fallback;
	}
	return v.u.i;
}
static bool boolean_in(reader *r, toml_table_t *t, const char *key,
	const char *prefix, bool fallback)
{
	if (!t || !toml_key_exists(t, key))
		return fallback;
	toml_datum_t v = toml_bool_in(t, key);
	if (!v.ok) {
		invalid(r, prefix, key);
		return fallback;
	}
	return v.u.b != 0;
}
static bool equal(const char *a, const char *b)
{
	return a && !strcmp(a, b);
}
static bool choice(const char *value, const char *a, const char *b,
	const char *c = nullptr)
{
	return equal(value, a) || equal(value, b) || (c && equal(value, c));
}
static toml_table_t *table(
	reader *r, toml_table_t *t, const char *key, const char *prefix)
{
	toml_table_t *result = t ? toml_table_in(t, key) : nullptr;
	if (t && toml_key_exists(t, key) && !result)
		invalid(r, prefix, key);
	return result;
}
static bool valid_peer(const char *text)
{
	if (!text || strlen(text) >= 128)
		return false;
	char address[128];
	strcpy(address, text);
	char *slash = strchr(address, '/');
	if (slash)
		*slash++ = 0;
	uint8_t bytes[16];
	unsigned maximum = inet_pton(AF_INET, address, bytes) == 1    ? 32
			   : inet_pton(AF_INET6, address, bytes) == 1 ? 128
								      : 0;
	if (!maximum)
		return false;
	if (!slash)
		return true;
	if (!*slash)
		return false;
	for (const char *p = slash; *p; ++p)
		if (*p < '0' || *p > '9')
			return false;
	char *end = nullptr;
	unsigned long bits = strtoul(slash, &end, 10);
	return !*end && bits <= maximum;
}
static void read_input(reader *r, toml_table_t *t, size_t index)
{
	tg_input_config *in = &r->c->inputs[index];
	char p[64];
	snprintf(p, sizeof(p), "inputs[%zu]", index);
	if (!schema(r, t,
		    "id name kind enabled host bind port encoding "
		    "group interface "
		    "allow_peers protocol prefer version uid callsign tls auth "
		    "maintenance",
		    p))
		return;
	in->id = string_in(r, t, "id", p);
	in->name = string_in(r, t, "name", p, in->id);
	in->kind = string_in(r, t, "kind", p);
	in->enabled = boolean_in(r, t, "enabled", p, true);
	if (!in->id)
		invalid(r, p, "id");
	if (!choice(in->kind, "tls", "udp", "multicast")) {
		invalid(r, p, "kind");
		return;
	}
	for (size_t j = 0; j < index; ++j)
		if (in->id && equal(in->id, r->c->inputs[j].id))
			invalid(r, p, "id");
	bool tls = equal(in->kind, "tls");
	in->port = (uint16_t)number(r, t, "port", p, tls ? 8089 : 0, 1, 65535);
	if (!in->port)
		invalid(r, p, "port");
	in->host = string_in(r, t, "host", p);
	in->bind = string_in(r, t, "bind", p, "0.0.0.0");
	in->group = string_in(r, t, "group", p);
	in->interface_name = string_in(r, t, "interface", p);
	if (equal(in->kind, "multicast") && (!in->group || !in->interface_name))
		invalid(r, p, "interface");
	const char *encoding = string_in(r, t, "encoding", p, "auto");
	if (!choice(encoding, "auto", "xml", "tak-v1"))
		invalid(r, p, "encoding");
	in->encoding = equal(encoding, "xml")	   ? TG_DATAGRAM_XML
		       : equal(encoding, "tak-v1") ? TG_DATAGRAM_TAK_V1
						   : TG_DATAGRAM_AUTO;
	toml_array_t *peers = toml_array_in(t, "allow_peers");
	if (toml_key_exists(t, "allow_peers") && !peers)
		invalid(r, p, "allow_peers");
	if (peers) {
		in->peer_count = (size_t)toml_array_nelem(peers);
		in->allow_peers = static_cast<const char **>(
			config_alloc(r->c, in->peer_count * sizeof(char *)));
		if (!in->allow_peers) {
			r->status = TG_NO_MEMORY;
			return;
		}
		for (size_t i = 0; i < in->peer_count; ++i) {
			toml_datum_t v = toml_string_at(peers, (int)i);
			if (!v.ok) {
				invalid(r, p, "allow_peers");
				continue;
			}
			if (!valid_peer(v.u.s))
				invalid(r, p, "allow_peers");
			in->allow_peers[i] = tg_config_string(r->c, v.u.s);
			free(v.u.s);
		}
	}
	if (!tls) {
		const char *forbidden_keys[] = {"tls", "auth", "maintenance",
			"host", "protocol", "prefer", "version", "uid",
			"callsign"};
		for (const char *key : forbidden_keys)
			if (toml_key_exists(t, key))
				invalid(r, p, key);
		return;
	}
	if (!in->host || strchr(in->host, '@') || strstr(in->host, "://"))
		invalid(r, p, "host");
	const char *udp_keys[] = {
		"bind", "group", "interface", "encoding", "allow_peers"};
	for (const char *key : udp_keys)
		if (toml_key_exists(t, key))
			invalid(r, p, key);
	in->session.uid = string_in(r, t, "uid", p);
	in->session.callsign = string_in(r, t, "callsign", p, in->session.uid);
	if (!in->session.uid)
		invalid(r, p, "uid");
	in->session.response_timeout_ms = 65000;
	in->session.partial_timeout_ms = 30000;
	in->session.version = 1;
	const char *protocol = string_in(r, t, "protocol", p, "negotiated");
	if (!choice(protocol, "negotiated", "xml-only", "protobuf-only"))
		invalid(r, p, "protocol");
	in->session.protocol =
		equal(protocol, "xml-only")	   ? TG_PROTOCOL_XML_ONLY
		: equal(protocol, "protobuf-only") ? TG_PROTOCOL_PROTOBUF_ONLY
						   : TG_PROTOCOL_NEGOTIATED;
	const char *prefer = string_in(r, t, "prefer", p, "xml");
	if (!choice(prefer, "xml", "protobuf"))
		invalid(r, p, "prefer");
	in->session.preference =
		equal(prefer, "protobuf") ? TG_PREFER_PROTOBUF : TG_PREFER_XML;
	if (in->session.protocol != TG_PROTOCOL_NEGOTIATED &&
		toml_key_exists(t, "prefer"))
		invalid(r, p, "prefer");
	if (in->session.protocol == TG_PROTOCOL_PROTOBUF_ONLY) {
		if (!toml_key_exists(t, "version"))
			invalid(r, p, "version");
		if (number(r, t, "version", p, 0, 0, INT64_MAX) != 1)
			invalid(r, p, "version", TG_UNSUPPORTED);
	} else if (toml_key_exists(t, "version"))
		invalid(r, p, "version");
	toml_table_t *ssl = table(r, t, "tls", p);
	char sp[96];
	snprintf(sp, sizeof(sp), "%s.tls", p);
	if (!schema(r, ssl,
		    "ca_file server_name verify_hostname cert_file key_file "
		    "key_password_file key_password_env pkcs12_file "
		    "pkcs12_password_file "
		    "pkcs12_password_env",
		    sp))
		return;
	in->ca_file = string_in(r, ssl, "ca_file", sp, nullptr, true);
	in->server_name = string_in(r, ssl, "server_name", sp, in->host);
	in->verify_hostname = boolean_in(r, ssl, "verify_hostname", sp, true);
	in->cert_file = string_in(r, ssl, "cert_file", sp, nullptr, true);
	in->key_file = string_in(r, ssl, "key_file", sp, nullptr, true);
	in->pkcs12_file = string_in(r, ssl, "pkcs12_file", sp, nullptr, true);
	in->key_password_file =
		string_in(r, ssl, "key_password_file", sp, nullptr, true);
	in->key_password_env = string_in(r, ssl, "key_password_env", sp);
	in->pkcs12_password_file =
		string_in(r, ssl, "pkcs12_password_file", sp, nullptr, true);
	in->pkcs12_password_env = string_in(r, ssl, "pkcs12_password_env", sp);
	if (!in->ca_file)
		invalid(r, sp, "ca_file");
	if (!!in->cert_file != !!in->key_file ||
		(in->pkcs12_file && (in->cert_file || in->key_file)))
		invalid(r, sp, "cert_file");
	if ((in->key_password_env && in->key_password_file) ||
		((in->key_password_env || in->key_password_file) &&
			!in->key_file))
		invalid(r, sp, "key_password_file");
	if ((in->pkcs12_password_env && in->pkcs12_password_file) ||
		((in->pkcs12_password_env || in->pkcs12_password_file) &&
			!in->pkcs12_file))
		invalid(r, sp, "pkcs12_password_file");
	toml_table_t *auth = table(r, t, "auth", p);
	snprintf(sp, sizeof(sp), "%s.auth", p);
	if (!schema(r, auth, "mode username password_file password_env", sp))
		return;
	const char *mode = string_in(r, auth, "mode", sp, "none");
	in->username = string_in(r, auth, "username", sp);
	in->password_file =
		string_in(r, auth, "password_file", sp, nullptr, true);
	in->password_env = string_in(r, auth, "password_env", sp);
	if (!choice(mode, "none", "password") ||
		(equal(mode, "password") &&
			(!in->username ||
				(!!in->password_file == !!in->password_env))) ||
		(equal(mode, "none") && (in->username || in->password_env ||
						in->password_file)))
		invalid(r, sp, "mode");
	toml_table_t *maintenance = table(r, t, "maintenance", p);
	snprintf(sp, sizeof(sp), "%s.maintenance", p);
	if (!schema(r, maintenance,
		    "ping_interval_ms ping_timeout_ms identity_xml_file", sp))
		return;
	in->session.ping_interval_ms = (uint64_t)number(
		r, maintenance, "ping_interval_ms", sp, 0, 0, INT64_MAX);
	in->session.ping_timeout_ms = (uint64_t)number(
		r, maintenance, "ping_timeout_ms", sp, 0, 0, INT64_MAX);
	if (!!in->session.ping_interval_ms != !!in->session.ping_timeout_ms)
		invalid(r, sp, "ping_interval_ms");
	in->identity_file = string_in(
		r, maintenance, "identity_xml_file", sp, nullptr, true);
}
static void read_output(reader *r, toml_table_t *t, size_t index)
{
	tg_output_config *o = &r->c->outputs[index];
	char p[64];
	snprintf(p, sizeof(p), "outputs[%zu]", index);
	if (!schema(r, t,
		    "id name kind enabled mode host bind port framing "
		    "max_clients "
		    "max_datagram_bytes path create permissions queue_bytes "
		    "queue_records",
		    p))
		return;
	o->id = string_in(r, t, "id", p);
	o->name = string_in(r, t, "name", p, o->id);
	o->kind = string_in(r, t, "kind", p);
	o->enabled = boolean_in(r, t, "enabled", p, true);
	if (!o->id)
		invalid(r, p, "id");
	if (!choice(o->kind, "tcp", "udp", "pipe")) {
		invalid(r, p, "kind");
		return;
	}
	for (size_t j = 0; j < index; ++j)
		if (o->id && equal(o->id, r->c->outputs[j].id))
			invalid(r, p, "id");
	o->mode = string_in(r, t, "mode", p, "listen");
	o->bind = string_in(r, t, "bind", p, "127.0.0.1");
	o->host = string_in(
		r, t, "host", p, equal(o->kind, "udp") ? "127.0.0.1" : nullptr);
	o->path = string_in(r, t, "path", p, nullptr, true);
	o->port = (uint16_t)number(r, t, "port", p, 0, 1, 65535);
	o->max_clients =
		(size_t)number(r, t, "max_clients", p, 32, 1, INT32_MAX);
	o->create = boolean_in(r, t, "create", p, false);
	const char *framing = string_in(r, t, "framing", p, "u32be");
	if (!choice(framing, "u32be", "xml-document"))
		invalid(r, p, "framing");
	o->delivery.framing = equal(framing, "u32be") ? TG_FRAMING_U32BE
						      : TG_FRAMING_XML_DOCUMENT;
	o->delivery.queue_bytes =
		(size_t)number(r, t, "queue_bytes", p, 0, 1, INT64_MAX);
	o->delivery.queue_records =
		(size_t)number(r, t, "queue_records", p, 0, 1, INT64_MAX);
	if (equal(o->kind, "udp")) {
		o->delivery.kind = TG_OUTPUT_DATAGRAM;
		o->delivery.framing = TG_FRAMING_XML_DOCUMENT;
		o->delivery.max_datagram_bytes = (size_t)number(
			r, t, "max_datagram_bytes", p, 65507, 1, 65507);
		const char *keys[] = {"mode", "bind", "framing", "max_clients",
			"path", "create", "permissions"};
		for (const char *key : keys)
			if (toml_key_exists(t, key))
				invalid(r, p, key);
	} else if (equal(o->kind, "tcp")) {
		if (!choice(o->mode, "listen", "connect") ||
			(equal(o->mode, "connect") &&
				(!o->host || toml_key_exists(t, "bind"))) ||
			(equal(o->mode, "listen") &&
				toml_key_exists(t, "host")))
			invalid(r, p, "mode");
		const char *keys[] = {
			"max_datagram_bytes", "path", "create", "permissions"};
		for (const char *key : keys)
			if (toml_key_exists(t, key))
				invalid(r, p, key);
		if (equal(o->mode, "connect") &&
			toml_key_exists(t, "max_clients"))
			invalid(r, p, "max_clients");
	} else {
		if (!o->path)
			invalid(r, p, "path");
		const char *permissions =
			string_in(r, t, "permissions", p, "0600");
		char *end = nullptr;
		unsigned long value =
			permissions ? strtoul(permissions, &end, 8) : 0;
		if (!permissions || *end || value > 0777 || (value & 0007))
			invalid(r, p, "permissions");
		o->permissions = (unsigned)value;
		const char *keys[] = {"port", "mode", "host", "bind",
			"max_clients", "max_datagram_bytes"};
		for (const char *key : keys)
			if (toml_key_exists(t, key))
				invalid(r, p, key);
	}
	if (!equal(o->kind, "pipe") && !o->port)
		invalid(r, p, "port");
}
static tg_status config_parse(tg_bytes input, const char *base,
	tg_config **out, tg_diagnostic *diagnostic, bool managed)
{
	/*
	 * Parse strict UTF-8 TOML and validate schema, cross-field rules,
	 * IDs, bounds, and obvious loops. Resolve relative paths but perform no
	 * filesystem/environment/network access. Copy immutable config on
	 * success.
	 */
	if (diagnostic)
		*diagnostic = {};
	if (out)
		*out = nullptr;
	if (!out || !tg_span(input) || !absolute(base))
		return error(diagnostic, TG_INVALID, "config");
	if (!tg_utf8(input) || input.size > 8 * 1024 * 1024)
		return error(diagnostic, TG_MALFORMED, "config");
	tg_config *c = static_cast<tg_config *>(calloc(1, sizeof(*c)));
	if (!c)
		return error(diagnostic, TG_NO_MEMORY, "config");
	c->original = static_cast<char *>(config_alloc(c, input.size + 1));
	c->base = (char *)tg_config_string(c, base);
	if (!c->original || !c->base) {
		tg_config_destroy(c);
		return TG_NO_MEMORY;
	}
	memcpy(c->original, input.data, input.size);
	char message[200]{};
	char *copy = tg_strdup(c->original);
	if (!copy) {
		tg_config_destroy(c);
		return TG_NO_MEMORY;
	}
	c->table = toml_parse(copy, message, sizeof(message));
	free(copy);
	if (!c->table) {
		tg_config_destroy(c);
		error(diagnostic, TG_MALFORMED, "config");
		if (diagnostic) {
			unsigned line = 0;
			if (sscanf(message, "line %u", &line) == 1)
				diagnostic->line = line;
		}
		return TG_MALFORMED;
	}
	c->managed = managed;
	reader r{c, diagnostic, TG_OK};
	toml_table_t *t = c->table;
	schema(&r, t,
		"version process limits normalization delivery inputs outputs",
		"");
	int64_t version = number(&r, t, "version", "", 0, 0, INT64_MAX);
	if (!version)
		invalid(&r, "", "version");
	else if (version != 1)
		invalid(&r, "", "version", TG_UNSUPPORTED);
	tg_limits defaults = TG_LIMITS_DEFAULT;
	c->limits = defaults;
	toml_table_t *limits = table(&r, t, "limits", "");
	schema(&r, limits,
		"max_message_bytes max_xml_depth retained_bytes queue_bytes "
		"queue_records max_inputs max_outputs",
		"limits");
	c->limits.max_message_bytes =
		(size_t)number(&r, limits, "max_message_bytes", "limits",
			defaults.max_message_bytes, 1, UINT32_MAX);
	c->limits.max_xml_depth = (uint32_t)number(&r, limits, "max_xml_depth",
		"limits", defaults.max_xml_depth, 1, UINT32_MAX);
	c->limits.retained_bytes = (size_t)number(&r, limits, "retained_bytes",
		"limits", defaults.retained_bytes, 1, INT64_MAX);
	c->limits.queue_bytes = (size_t)number(&r, limits, "queue_bytes",
		"limits", defaults.queue_bytes, 1, INT64_MAX);
	c->limits.queue_records = (size_t)number(&r, limits, "queue_records",
		"limits", defaults.queue_records, 1, INT64_MAX);
	c->limits.max_outputs = (size_t)number(&r, limits, "max_outputs",
		"limits", defaults.max_outputs, 1, INT64_MAX);
	c->max_inputs = (size_t)number(
		&r, limits, "max_inputs", "limits", 32, 1, INT32_MAX);
	if (c->limits.retained_bytes < c->limits.max_message_bytes)
		invalid(&r, "limits", "retained_bytes");
	toml_table_t *process = table(&r, t, "process", "");
	schema(&r, process, "log_format log_level status_file", "process");
	c->log_format = string_in(&r, process, "log_format", "process", "json");
	c->log_level = string_in(&r, process, "log_level", "process", "info");
	c->status_file =
		string_in(&r, process, "status_file", "process", nullptr, true);
	if (!choice(c->log_format, "json", "text"))
		invalid(&r, "process", "log_format");
	if (!choice(c->log_level, "info", "debug", "warning") &&
		!equal(c->log_level, "error"))
		invalid(&r, "process", "log_level");
	toml_table_t *normal = table(&r, t, "normalization", "");
	schema(&r, normal, "protobuf_extra_fields unrepresentable_core",
		"normalization");
	const char *preserve = string_in(&r, normal, "protobuf_extra_fields",
		"normalization", "preserve-opaque");
	if (!choice(preserve, "preserve-opaque", "reject"))
		invalid(&r, "normalization", "protobuf_extra_fields");
	c->preservation = equal(preserve, "reject") ? TG_REJECT_LOSSY
						    : TG_PRESERVE_OPAQUE;
	if (!equal(string_in(&r, normal, "unrepresentable_core",
			   "normalization", "reject"),
		    "reject"))
		invalid(&r, "normalization", "unrepresentable_core");
	toml_table_t *delivery = table(&r, t, "delivery", "");
	schema(&r, delivery, "overflow", "delivery");
	const char *overflow =
		string_in(&r, delivery, "overflow", "delivery", "drop-newest");
	if (!choice(overflow, "drop-newest", "fail"))
		invalid(&r, "delivery", "overflow");
	c->overflow = equal(overflow, "fail") ? TG_OVERFLOW_FAIL
					      : TG_OVERFLOW_DROP_NEWEST;
	toml_array_t *inputs = toml_array_in(t, "inputs");
	toml_array_t *outputs = toml_array_in(t, "outputs");
	c->input_count = inputs ? (size_t)toml_array_nelem(inputs) : 0;
	c->output_count = outputs ? (size_t)toml_array_nelem(outputs) : 0;
	if ((!managed && !c->input_count) ||
		(toml_key_exists(t, "inputs") && !inputs) ||
		c->input_count > c->max_inputs ||
		(c->input_count && toml_array_kind(inputs) != 't'))
		invalid(&r, "", "inputs");
	if ((!managed && !c->output_count) ||
		(toml_key_exists(t, "outputs") && !outputs) ||
		c->output_count > c->limits.max_outputs ||
		(c->output_count && toml_array_kind(outputs) != 't'))
		invalid(&r, "", "outputs");
	if (r.status == TG_OK) {
		c->inputs = static_cast<tg_input_config *>(
			config_alloc(c, c->input_count * sizeof(*c->inputs)));
		c->outputs = static_cast<tg_output_config *>(
			config_alloc(c, c->output_count * sizeof(*c->outputs)));
		if (!c->inputs || !c->outputs)
			r.status = error(diagnostic, TG_NO_MEMORY, "config");
	}
	bool enabled_input = false, enabled_output = false;
	if (r.status == TG_OK) {
		for (size_t i = 0; i < c->input_count; ++i) {
			read_input(&r, toml_table_at(inputs, (int)i), i);
			enabled_input |= c->inputs[i].enabled;
		}
		for (size_t i = 0; i < c->output_count; ++i) {
			read_output(&r, toml_table_at(outputs, (int)i), i);
			enabled_output |= c->outputs[i].enabled;
		}
		if (!managed && !enabled_input)
			invalid(&r, "", "inputs");
		if (!managed && !enabled_output)
			invalid(&r, "", "outputs");
	}
	if (r.status == TG_OK)
		for (size_t i = 0; i < c->input_count; ++i) {
			tg_input_config *in = &c->inputs[i];
			for (size_t j = 0; j < c->output_count; ++j) {
				tg_output_config *o = &c->outputs[j];
				if (in->enabled && o->enabled &&
					in->port == o->port &&
					equal(o->kind, "udp") &&
					!equal(in->kind, "tls") &&
					(equal(in->bind, o->host) ||
						equal(in->bind, "0.0.0.0") ||
						equal(in->group, o->host)))
					invalid(&r, "outputs", "host");
			}
		}
	if (r.status != TG_OK) {
		tg_config_destroy(c);
		return r.status;
	}
	*out = c;
	if (diagnostic)
		*diagnostic = {};
	return TG_OK;
}
#ifdef _WIN32
static bool protected_acl(HANDLE file)
{
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	PSID owner = nullptr;
	PACL acl = nullptr;
	if (GetSecurityInfo(file, SE_FILE_OBJECT,
		    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		    &owner, nullptr, &acl, nullptr,
		    &descriptor) != ERROR_SUCCESS)
		return false;
	bool safe = acl && owner;
	for (DWORD i = 0; safe && i < acl->AceCount; ++i) {
		void *entry = nullptr;
		if (!GetAce(acl, i, &entry)) {
			safe = false;
			break;
		}
		ACE_HEADER *header = static_cast<ACE_HEADER *>(entry);
		if (header->AceFlags & INHERIT_ONLY_ACE)
			continue;
		if (header->AceType == ACCESS_DENIED_ACE_TYPE)
			continue;
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
			safe = false;
			break;
		}
		ACCESS_ALLOWED_ACE *ace =
			static_cast<ACCESS_ALLOWED_ACE *>(entry);
		PSID sid = &ace->SidStart;
		if (EqualSid(owner, sid) ||
			IsWellKnownSid(sid, WinLocalSystemSid) ||
			IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
			continue;
		TRUSTEE_A trustee{};
		BuildTrusteeWithSidA(&trustee, sid);
		ACCESS_MASK rights = 0;
		if (GetEffectiveRightsFromAclA(acl, &trustee, &rights) !=
				ERROR_SUCCESS ||
			(rights & (FILE_READ_DATA | FILE_WRITE_DATA |
					  FILE_APPEND_DATA | GENERIC_READ |
					  GENERIC_WRITE | GENERIC_ALL)))
			safe = false;
	}
	LocalFree(descriptor);
	return safe;
}
#endif
tg_status tg_read_file(
	const char *path, size_t maximum, bool secret, tg_buffer *out)
{
	if (!path || !out)
		return TG_INVALID;
#ifdef _WIN32
	HANDLE handle =
		CreateFileA(path, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return TG_IO;
	BY_HANDLE_FILE_INFORMATION info{};
	bool valid = GetFileInformationByHandle(handle, &info) &&
		     !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		     (!secret || protected_acl(handle));
	if (!valid) {
		CloseHandle(handle);
		return TG_IO;
	}
	int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
	if (fd < 0) {
		CloseHandle(handle);
		return TG_IO;
	}
	FILE *f = _fdopen(fd, "rb");
	if (!f) {
		_close(fd);
		return TG_IO;
	}
#else
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return TG_IO;
	struct stat st{};
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
		(uint64_t)st.st_size > maximum ||
		(secret && (st.st_mode & 0077))) {
		close(fd);
		return TG_IO;
	}
	FILE *f = fdopen(fd, "rb");
	if (!f) {
		close(fd);
		return TG_IO;
	}
#endif
	tg_vec bytes{};
	uint8_t buffer[4096];
	tg_status status = TG_OK;
	while (!feof(f)) {
		size_t n = fread(buffer, 1, sizeof(buffer), f);
		if (n > maximum - bytes.size) {
			status = TG_LIMIT;
			break;
		}
		if (!tg_append(&bytes, buffer, n)) {
			status = TG_NO_MEMORY;
			break;
		}
		if (ferror(f)) {
			status = TG_IO;
			break;
		}
	}
	fclose(f);
	if (status == TG_OK) {
		*out = {bytes.data, bytes.size};
		bytes = {};
	}
	tg_vec_free(&bytes);
	return status;
}
tg_status tg_config_validate(const tg_config *c, tg_diagnostic *d)
{
	/*
	 * Validate referenced credentials/permissions and named secret
	 * environment variables without network I/O or sensitive diagnostics.
	 */
	if (d)
		*d = {};
	if (!c)
		return error(d, TG_INVALID, "config");
	for (size_t i = 0; i < c->input_count; ++i) {
		const tg_input_config *in = &c->inputs[i];
		if (!in->enabled || !equal(in->kind, "tls"))
			continue;
		const char *paths[] = {in->ca_file, in->cert_file, in->key_file,
			in->pkcs12_file, in->key_password_file,
			in->pkcs12_password_file, in->password_file,
			in->identity_file};
		const char *keys[] = {"tls.ca_file", "tls.cert_file",
			"tls.key_file", "tls.pkcs12_file",
			"tls.key_password_file", "tls.pkcs12_password_file",
			"auth.password_file", "maintenance.identity_xml_file"};
		for (size_t j = 0; j < sizeof(paths) / sizeof(*paths); ++j) {
			if (!paths[j])
				continue;
			tg_buffer file{};
			tg_status s = tg_read_file(paths[j],
				c->limits.max_message_bytes, j >= 2 && j <= 6,
				&file);
			if (file.data)
				memset(file.data, 0, file.size);
			tg_buffer_release(&file);
			if (s != TG_OK) {
				char key[128];
				snprintf(key, sizeof(key), "inputs[%zu].%s", i,
					keys[j]);
				return error(d, s, key);
			}
		}
		const char *envs[] = {in->key_password_env,
			in->pkcs12_password_env, in->password_env};
		const char *names[] = {"tls.key_password_env",
			"tls.pkcs12_password_env", "auth.password_env"};
		for (size_t j = 0; j < 3; ++j)
			if (envs[j] &&
				(!getenv(envs[j]) || !*getenv(envs[j]))) {
				char key[128];
				snprintf(key, sizeof(key), "inputs[%zu].%s", i,
					names[j]);
				return error(d, TG_IO, key);
			}
	}
	return TG_OK;
}
static bool emit_string(tg_vec *out, const char *key, const char *value)
{
	return tg_printf(out, "%s = \"", key) && tg_escape(out, value, true) &&
	       tg_text(out, "\"\n");
}
static bool secret_key(const char *key)
{
	return strstr(key, "password") || equal(key, "key_file") ||
	       equal(key, "pkcs12_file");
}
static bool setting(tg_vec *out, bool redact, const char *key,
	const char *value)
{
	if (!value)
		return true;
	return emit_string(out, key,
		redact && secret_key(key) ? "<redacted>" : value);
}
static bool serialize_input(tg_vec *out, const tg_input_config *in,
	bool redact)
{
	bool ok = tg_text(out, "\n[[inputs]]\n") &&
		  setting(out, redact, "id", in->id) &&
		  setting(out, redact, "name", in->name) &&
		  setting(out, redact, "kind", in->kind) &&
		  tg_printf(out, "enabled = %s\nport = %u\n",
			  in->enabled ? "true" : "false", in->port);
	if (!equal(in->kind, "tls")) {
		ok = ok && setting(out, redact, "bind", in->bind) &&
		     setting(out, redact, "group", in->group) &&
		     setting(out, redact, "interface", in->interface_name) &&
		     setting(out, redact, "encoding",
			     in->encoding == TG_DATAGRAM_XML	  ? "xml"
			     : in->encoding == TG_DATAGRAM_TAK_V1 ? "tak-v1"
								  : "auto") &&
		     tg_text(out, "allow_peers = [");
		for (size_t j = 0; ok && j < in->peer_count; ++j)
			ok = tg_text(out, j ? ", \"" : "\"") &&
			     tg_escape(out, in->allow_peers[j], true) &&
			     tg_text(out, "\"");
		return ok && tg_text(out, "]\n");
	}
	const tg_session_options *s = &in->session;
	ok = ok && setting(out, redact, "host", in->host) &&
	     setting(out, redact, "uid", s->uid) &&
	     setting(out, redact, "callsign", s->callsign) &&
	     setting(out, redact, "protocol",
		     s->protocol == TG_PROTOCOL_XML_ONLY ? "xml-only"
		     : s->protocol == TG_PROTOCOL_PROTOBUF_ONLY
			     ? "protobuf-only"
			     : "negotiated");
	if (s->protocol == TG_PROTOCOL_PROTOBUF_ONLY)
		ok = ok && tg_text(out, "version = 1\n");
	if (s->protocol == TG_PROTOCOL_NEGOTIATED)
		ok = ok && setting(out, redact, "prefer",
				   s->preference == TG_PREFER_XML ? "xml"
								  : "protobuf");
	ok = ok && tg_text(out, "\n[inputs.tls]\n") &&
	     tg_printf(out, "verify_hostname = %s\n",
		in->verify_hostname ? "true" : "false") &&
	     setting(out, redact, "ca_file", in->ca_file) &&
	     setting(out, redact, "server_name", in->server_name) &&
	     setting(out, redact, "cert_file", in->cert_file) &&
	     setting(out, redact, "key_file", in->key_file) &&
	     setting(out, redact, "key_password_file", in->key_password_file) &&
	     setting(out, redact, "key_password_env", in->key_password_env) &&
	     setting(out, redact, "pkcs12_file", in->pkcs12_file) &&
	     setting(out, redact, "pkcs12_password_file",
		in->pkcs12_password_file) &&
	     setting(out, redact, "pkcs12_password_env",
		in->pkcs12_password_env) &&
	     tg_text(out, "\n[inputs.auth]\n") &&
	     setting(out, redact, "mode", in->username ? "password" : "none") &&
	     setting(out, redact, "username", in->username) &&
	     setting(out, redact, "password_file", in->password_file) &&
	     setting(out, redact, "password_env", in->password_env) &&
	     tg_printf(out,
		     "\n[inputs.maintenance]\nping_interval_ms = %llu\n"
		     "ping_timeout_ms = %llu\n",
		     (unsigned long long)s->ping_interval_ms,
		     (unsigned long long)s->ping_timeout_ms) &&
	     setting(out, redact, "identity_xml_file", in->identity_file);
	return ok;
}
static bool serialize_output(
	tg_vec *out, const tg_output_config *o, const tg_limits *limits,
		bool redact)
{
	bool ok = tg_text(out, "\n[[outputs]]\n") &&
		  setting(out, redact, "id", o->id) &&
		  setting(out, redact, "name", o->name) && setting(out, redact,
			"kind", o->kind) &&
		  tg_printf(out,
			  "enabled = %s\nqueue_bytes = %zu\n"
			  "queue_records = %zu\n",
			  o->enabled ? "true" : "false",
			  o->delivery.queue_bytes ? o->delivery.queue_bytes
						  : limits->queue_bytes,
			  o->delivery.queue_records ? o->delivery.queue_records
						    : limits->queue_records);
	if (equal(o->kind, "udp"))
		return ok && setting(out, redact, "host", o->host) &&
		       tg_printf(out, "port = %u\nmax_datagram_bytes = %zu\n",
			       o->port, o->delivery.max_datagram_bytes);
	ok = ok &&
	     setting(out, redact, "framing",
		     o->delivery.framing == TG_FRAMING_U32BE ? "u32be"
							     : "xml-document");
	if (equal(o->kind, "pipe"))
		return ok && setting(out, redact, "path", o->path) &&
		       tg_printf(out, "create = %s\npermissions = \"0%o\"\n",
			       o->create ? "true" : "false", o->permissions);
	ok = ok && setting(out, redact, "mode", o->mode) &&
	     tg_printf(out, "port = %u\n", o->port);
	if (equal(o->mode, "connect"))
		return ok && setting(out, redact, "host", o->host);
	return ok && setting(out, redact, "bind", o->bind) &&
	       tg_printf(out, "max_clients = %zu\n", o->max_clients);
}
static tg_status config_serialize(const tg_config *c, tg_buffer *out,
	tg_diagnostic *d, bool redact)
{
	if (d)
		*d = {};
	if (!c || !tg_empty(out))
		return error(d, TG_INVALID, "config");
	tg_vec text{};
	bool ok = tg_text(&text, "version = 1\n") &&
		  (c->input_count || tg_text(&text, "inputs = []\n")) &&
		  (c->output_count || tg_text(&text, "outputs = []\n")) &&
		  tg_text(&text, "\n[process]\n") &&
		  setting(&text, redact, "log_format", c->log_format) &&
		  setting(&text, redact, "log_level", c->log_level) &&
		  setting(&text, redact, "status_file", c->status_file) &&
		  tg_printf(&text,
			  "\n[limits]\nmax_message_bytes = %zu\n"
			  "max_xml_depth = %u\nretained_bytes = "
			  "%zu\nqueue_bytes = %zu\n"
			  "queue_records = %zu\nmax_inputs = %zu\nmax_outputs "
			  "= %zu\n",
			  c->limits.max_message_bytes, c->limits.max_xml_depth,
			  c->limits.retained_bytes, c->limits.queue_bytes,
			  c->limits.queue_records, c->max_inputs,
			  c->limits.max_outputs) &&
		  tg_text(&text, "\n[normalization]\n") &&
		  setting(&text, redact, "protobuf_extra_fields",
			  c->preservation == TG_PRESERVE_OPAQUE
				  ? "preserve-opaque"
				  : "reject") &&
		  setting(&text, redact, "unrepresentable_core", "reject") &&
		  tg_text(&text, "\n[delivery]\n") &&
		  setting(&text, redact, "overflow",
			  c->overflow == TG_OVERFLOW_FAIL ? "fail"
							  : "drop-newest");
	for (size_t i = 0; ok && i < c->input_count; ++i)
		ok = serialize_input(&text, &c->inputs[i], redact);
	for (size_t i = 0; ok && i < c->output_count; ++i)
		ok = serialize_output(&text, &c->outputs[i], &c->limits,
			redact);
	if (!ok) {
		tg_vec_free(&text);
		return error(d, TG_NO_MEMORY, "config");
	}
	*out = {text.data, text.size};
	return TG_OK;
}
void tg_config_destroy(tg_config *c)
{
	/*
	 * Release copied immutable settings. Runtimes own their copies.
	 */
	if (!c)
		return;
	if (c->table)
		toml_free(c->table);
	while (c->blocks) {
		config_block *b = c->blocks;
		c->blocks = b->next;
		free(b->memory);
		free(b);
	}
	free(c);
}


tg_status tg_config_parse(tg_bytes bytes, const char *base, tg_config **out,
	tg_diagnostic *diagnostic)
{
	return config_parse(bytes, base, out, diagnostic, false);
}

tg_status tg_config_parse_managed(tg_bytes bytes, const char *base,
	tg_config **out, tg_diagnostic *diagnostic)
{
	return config_parse(bytes, base, out, diagnostic, true);
}

tg_status tg_config_redacted(const tg_config *config, tg_buffer *out,
	tg_diagnostic *diagnostic)
{
	return config_serialize(config, out, diagnostic, true);
}

tg_status tg_config_export(const tg_config *config, tg_buffer *out,
	tg_diagnostic *diagnostic)
{
	return config_serialize(config, out, diagnostic, false);
}
