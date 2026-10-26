#include "cli_options.h"
#include "config_internal.h"
#include <openssl/crypto.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

enum node_kind {
	node_scalar,
	node_table,
	node_array
};

struct option_node {
	node_kind kind;
	char *key;
	char *raw;
	option_node *children;
	option_node *next;
};

struct option_tree {
	size_t count;
	tg_status status;
};

struct path_part {
	char key[128];
	size_t index;
	bool array;
};

struct scalar_option {
	const char *flag;
	const char *path;
	bool text;
};

static const scalar_option scalar_options[] = {
	{"--status-file", "process.status_file", true},
	{"--log-level", "process.log_level", true},
	{"--log-format", "process.log_format", true},
	{"--max-message-bytes", "limits.max_message_bytes", false},
	{"--max-xml-depth", "limits.max_xml_depth", false},
	{"--retained-bytes", "limits.retained_bytes", false},
	{"--queue-bytes", "limits.queue_bytes", false},
	{"--queue-records", "limits.queue_records", false},
	{"--max-inputs", "limits.max_inputs", false},
	{"--max-outputs", "limits.max_outputs", false},
	{"--protobuf-extra-fields", "normalization.protobuf_extra_fields",
		true},
	{"--overflow", "delivery.overflow", true}
};

static tg_status cli_error(tg_diagnostic *d, tg_status status, const char *key)
{
	if (d) {
		*d = {};
		d->status = status;
		snprintf(d->key, sizeof(d->key), "cli.%s", key);
	}
	return status;
}

static void node_free(option_tree *tree, option_node *node)
{
	while (node) {
		option_node *next = node->next;

		node_free(tree, node->children);
		free(node->key);
		if (node->raw)
			OPENSSL_cleanse(node->raw, strlen(node->raw));
		free(node->raw);
		free(node);
		--tree->count;
		node = next;
	}
}

static option_node *node_create(option_tree *tree, node_kind kind,
	const char *key = nullptr, const char *raw = nullptr)
{
	if (tree->count == 8192) {
		tree->status = TG_LIMIT;
		return nullptr;
	}
	option_node *node = static_cast<option_node *>(
		calloc(1, sizeof(*node)));

	if (!node) {
		tree->status = TG_NO_MEMORY;
		return nullptr;
	}
	++tree->count;
	node->kind = kind;
	node->key = tg_strdup(key);
	node->raw = tg_strdup(raw);
	if ((key && !node->key) || (raw && !node->raw)) {
		node_free(tree, node);
		tree->status = TG_NO_MEMORY;
		return nullptr;
	}
	return node;
}

static option_node *read_table(option_tree *, const toml_table_t *,
	const char *, unsigned);
static option_node *read_array(option_tree *, const toml_array_t *,
	const char *, unsigned);

static option_node *read_table(option_tree *tree, const toml_table_t *table,
	const char *key, unsigned depth)
{
	if (depth > 32) {
		tree->status = TG_LIMIT;
		return nullptr;
	}
	option_node *node = node_create(tree, node_table, key);

	if (!node)
		return nullptr;
	option_node **tail = &node->children;

	for (int i = 0; tree->status == TG_OK; ++i) {
		const char *name = toml_key_in(table, i);

		if (!name)
			break;
		const char *raw = toml_raw_in(table, name);
		const toml_table_t *child = toml_table_in(table, name);
		const toml_array_t *array = toml_array_in(table, name);

		if (raw)
			*tail = node_create(tree, node_scalar, name, raw);
		else if (child)
			*tail = read_table(tree, child, name, depth + 1);
		else if (array)
			*tail = read_array(tree, array, name, depth + 1);
		else
			tree->status = TG_MALFORMED;
		if (*tail)
			tail = &(*tail)->next;
	}
	if (tree->status < 0) {
		node_free(tree, node);
		return nullptr;
	}
	return node;
}

static option_node *read_array(option_tree *tree, const toml_array_t *array,
	const char *key, unsigned depth)
{
	if (depth > 32) {
		tree->status = TG_LIMIT;
		return nullptr;
	}
	option_node *node = node_create(tree, node_array, key);

	if (!node)
		return nullptr;
	option_node **tail = &node->children;

	for (int i = 0; tree->status == TG_OK &&
		i < toml_array_nelem(array); ++i) {
		const char *raw = toml_raw_at(array, i);
		const toml_table_t *child = toml_table_at(array, i);
		const toml_array_t *nested = toml_array_at(array, i);

		if (raw)
			*tail = node_create(tree, node_scalar, nullptr, raw);
		else if (child)
			*tail = read_table(tree, child, nullptr, depth + 1);
		else if (nested)
			*tail = read_array(tree, nested, nullptr, depth + 1);
		else
			tree->status = TG_MALFORMED;
		if (*tail)
			tail = &(*tail)->next;
	}
	if (tree->status < 0) {
		node_free(tree, node);
		return nullptr;
	}
	return node;
}

/* Bound parser recursion before upstream recursive parsing can begin. */
static bool lexical_bounds(tg_bytes bytes)
{
	char brackets[32];
	size_t depth = 0;
	unsigned dots = 0;
	char quote = 0;
	bool multiline = false, comment = false;

	for (size_t i = 0; i < bytes.size; ++i) {
		char c = (char)bytes.data[i];

		if (comment) {
			if (c == '\n' || c == '\r') {
				comment = false;
				dots = 0;
			}
			continue;
		}
		if (quote) {
			if (quote == '"' && c == '\\') {
				if (i + 1 < bytes.size)
					++i;
				continue;
			}
			if (c != quote)
				continue;
			if (!multiline) {
				quote = 0;
				continue;
			}
			size_t run = 1;

			while (run < bytes.size - i &&
				bytes.data[i + run] == (uint8_t)quote)
				++run;
			if (run > 5)
				return false;
			if (run >= 3)
				quote = 0;
			i += run - 1;
			continue;
		}
		if (c == '#') {
			comment = true;
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			multiline = i + 2 < bytes.size &&
				bytes.data[i + 1] == (uint8_t)c &&
				bytes.data[i + 2] == (uint8_t)c;
			if (multiline)
				i += 2;
			continue;
		}
		if (c == '[' || c == '{') {
			if (depth == sizeof(brackets))
				return false;
			brackets[depth++] = c;
		} else if (c == ']' || c == '}') {
			if (!depth || brackets[--depth] !=
				(c == ']' ? '[' : '{'))
				return false;
		} else if (c == '.' && ++dots >= 32) {
			return false;
		}
		if (c == '=' || c == ',' || c == '\n' || c == '\r' ||
			c == '[' || c == ']' || c == '{' || c == '}')
			dots = 0;
	}
	return true;
}

static option_node *parse_tree(option_tree *tree, tg_bytes bytes)
{
	if (bytes.size > 8 * 1024 * 1024 || !tg_utf8(bytes) ||
		!lexical_bounds(bytes)) {
		tree->status = TG_LIMIT;
		return nullptr;
	}
	char *copy = static_cast<char *>(malloc(bytes.size + 1));

	if (!copy) {
		tree->status = TG_NO_MEMORY;
		return nullptr;
	}
	if (bytes.size)
		memcpy(copy, bytes.data, bytes.size);
	copy[bytes.size] = 0;
	char error[256];
	toml_table_t *table = toml_parse(copy, error, sizeof(error));

	OPENSSL_cleanse(copy, bytes.size);
	free(copy);
	if (!table) {
		tree->status = TG_MALFORMED;
		return nullptr;
	}
	option_node *root = read_table(tree, table, nullptr, 0);

	toml_free(table);
	return root;
}

static option_node **child_slot(option_node *table, const char *key)
{
	option_node **slot = &table->children;

	while (*slot && strcmp((*slot)->key, key))
		slot = &(*slot)->next;
	return slot;
}

static bool emit_string(tg_vec *text, const char *value)
{
	return tg_text(text, "\"") && tg_escape(text, value, true) &&
		tg_text(text, "\"");
}

static bool serialize(tg_vec *text, const option_node *node, bool root)
{
	if (node->kind == node_scalar)
		return tg_text(text, node->raw);
	bool table = node->kind == node_table;

	if (!root && !tg_text(text, table ? "{" : "["))
		return false;
	for (const option_node *child = node->children; child;
		child = child->next) {
		if (child != node->children &&
			!tg_text(text, root ? "\n" : ","))
			return false;
		if (table && (!emit_string(text, child->key) ||
			!tg_text(text, "=")))
			return false;
		if (!serialize(text, child, false) ||
			text->size > 8 * 1024 * 1024)
			return false;
	}
	return root || tg_text(text, table ? "}" : "]");
}

static bool bounded_depth(const option_node *node, unsigned depth)
{
	if (depth > 32)
		return false;
	for (const option_node *child = node->children; child;
		child = child->next)
		if (!bounded_depth(child, depth + 1))
			return false;
	return true;
}

static option_node *parse_value(option_tree *tree, const char *raw)
{
	tg_vec source{};

	if (!*raw || !tg_text(&source, "value=") ||
		!tg_text(&source, raw)) {
		tree->status = *raw ? TG_NO_MEMORY : TG_MALFORMED;
		tg_vec_free(&source);
		return nullptr;
	}
	option_node *root = parse_tree(tree, {source.data, source.size});

	if (source.data)
		OPENSSL_cleanse(source.data, source.size);
	tg_vec_free(&source);
	if (!root)
		return nullptr;
	option_node *value = root->children;

	if (!value || value->next || strcmp(value->key, "value")) {
		node_free(tree, root);
		tree->status = TG_MALFORMED;
		return nullptr;
	}
	root->children = nullptr;
	node_free(tree, root);
	return value;
}

static bool key_character(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool parse_path(const char *path, path_part *parts, size_t *count)
{
	*count = 0;
	if (!path || !*path || strlen(path) > 4096)
		return false;
	const char *p = path;

	while (*p && *count < 32) {
		path_part *part = &parts[(*count)++];

		if (*p == '[') {
			if (*count == 1)
				return false;
			part->array = true;
			++p;
			const char *start = p;

			while (*p >= '0' && *p <= '9') {
				part->index = part->index * 10 +
					(size_t)(*p - '0');
				if (part->index >= 8192)
					return false;
				++p;
			}
			if (p == start || *p++ != ']')
				return false;
		} else {
			const char *start = p;

			while (key_character(*p))
				++p;
			size_t n = (size_t)(p - start);

			if (!n || n >= sizeof(part->key))
				return false;
			memcpy(part->key, start, n);
		}
		if (*p == '.') {
			++p;
			if (!*p || *p == '[')
				return false;
		} else if (*p && *p != '[') {
			return false;
		}
	}
	return !*p && *count;
}

static option_node **index_slot(option_node *array, size_t index)
{
	option_node **slot = &array->children;

	for (size_t i = 0; i < index; ++i) {
		if (!*slot)
			return nullptr;
		slot = &(*slot)->next;
	}
	return slot;
}

/* Consume replacement on both success and failure. */
static bool assign(option_tree *tree, option_node *root, const char *path,
	option_node *replacement)
{
	path_part parts[32]{};
	size_t count = 0;
	bool ok = parse_path(path, parts, &count);
	option_node *current = root;
	option_node **slot = nullptr;

	for (size_t i = 0; ok && i < count; ++i) {
		path_part *part = &parts[i];

		if ((part->array && current->kind != node_array) ||
			(!part->array && current->kind != node_table)) {
			ok = false;
			break;
		}
		slot = part->array ? index_slot(current, part->index) :
			child_slot(current, part->key);
		if (!slot) {
			ok = false;
			break;
		}
		if (i + 1 == count) {
			char *key = part->array ? nullptr :
				tg_strdup(part->key);

			if (!part->array && !key) {
				tree->status = TG_NO_MEMORY;
				ok = false;
				break;
			}
			free(replacement->key);
			replacement->key = key;
			option_node *old = *slot;

			if (old) {
				replacement->next = old->next;
				old->next = nullptr;
				node_free(tree, old);
			}
			*slot = replacement;
			return true;
		}
		if (!*slot)
			*slot = node_create(tree,
				parts[i + 1].array ? node_array : node_table,
				part->array ? nullptr : part->key);
		if (!*slot) {
			ok = false;
			break;
		}
		current = *slot;
	}
	node_free(tree, replacement);
	if (tree->status == TG_OK)
		tree->status = TG_INVALID;
	return false;
}

static bool absolute_path(const char *path)
{
	if (!path || !*path)
		return false;
	if (*path == '/')
		return true;
#ifdef _WIN32
	return (strlen(path) > 2 && path[1] == ':' &&
		(path[2] == '/' || path[2] == '\\')) ||
		(path[0] == '\\' && path[1] == '\\');
#else
	return false;
#endif
}

static bool full_path(tg_vec *out, const char *cwd, const char *path)
{
	return (absolute_path(path) ||
		(tg_text(out, cwd) && tg_text(out, "/"))) &&
		tg_text(out, path);
}

static const scalar_option *scalar_flag(const char *flag)
{
	for (const scalar_option &option : scalar_options)
		if (!strcmp(option.flag, flag))
			return &option;
	return nullptr;
}

static bool string_setting(tg_vec *source, const char *key, const char *value)
{
	return emit_string(source, key) && tg_text(source, "=") &&
		emit_string(source, value) && tg_text(source, ",");
}

static bool valid_port(const char *text, unsigned *port)
{
	unsigned value = 0;

	if (!*text)
		return false;
	for (; *text; ++text) {
		if (*text < '0' || *text > '9' || value > 65535 / 10)
			return false;
		value = value * 10 + (unsigned)(*text - '0');
		if (value > 65535)
			return false;
	}
	*port = value;
	return value != 0;
}

static bool output_setting(option_tree *tree, option_node *root,
	const char *argument)
{
	const char *equal = strchr(argument, '=');
	const char *uri = equal ? equal + 1 : "";
	size_t id_length = equal ? (size_t)(equal - argument) : 0;
	char id[256];
	tg_vec source{};
	bool ok = id_length && id_length < sizeof(id) && *uri &&
		!strpbrk(uri, "@?#%\r\n\t");

	if (!ok) {
		tree->status = TG_INVALID;
		return false;
	}
	memcpy(id, argument, id_length);
	id[id_length] = 0;
	for (size_t i = 0; i < id_length; ++i)
		if ((unsigned char)id[i] < 32 || id[i] == 127)
			ok = false;
	ok = ok && tg_text(&source, "{") &&
		string_setting(&source, "id", id);
	const char *address = nullptr;
	const char *kind = nullptr;
	const char *field = nullptr;
	bool connect = false;

	if (!strncmp(uri, "tcp://", 6)) {
		address = uri + 6;
		kind = "tcp";
		field = "bind";
	} else if (!strncmp(uri, "tcp-connect://", 14)) {
		address = uri + 14;
		kind = "tcp";
		field = "host";
		connect = true;
	} else if (!strncmp(uri, "udp://", 6)) {
		address = uri + 6;
		kind = "udp";
		field = "host";
	} else if (!strncmp(uri, "pipe:///", 8)) {
		ok = ok && string_setting(&source, "kind", "pipe") &&
			string_setting(&source, "path", uri + 7) &&
			tg_text(&source, "create=true}");
	} else {
		ok = false;
	}
	if (address) {
		char *host = tg_strdup(address);
		char *port_text = nullptr;
		unsigned port = 0;

		ok = ok && host && !strpbrk(address, " /\\");
		if (ok && *host == '[') {
			char *close = strchr(host, ']');

			ok = close && close[1] == ':' && close > host + 1;
			if (ok) {
				*close = 0;
				port_text = close + 2;
				memmove(host, host + 1, strlen(host));
				unsigned char bytes[16];

				ok = inet_pton(AF_INET6, host, bytes) == 1;
			}
		} else if (ok) {
			port_text = strchr(host, ':');
			ok = port_text && !strchr(port_text + 1, ':');
			if (ok)
				*port_text++ = 0;
		}
		ok = ok && *host && !strpbrk(host, "[]") && port_text &&
			valid_port(port_text, &port) &&
			string_setting(&source, "kind", kind) &&
			string_setting(&source, field, host);
		if (connect)
			ok = ok && string_setting(&source, "mode", "connect");
		ok = ok && tg_printf(&source, "port=%u}", port);
		free(host);
	}
	option_node *replacement = ok ?
		parse_value(tree, (const char *)source.data) : nullptr;

	tg_vec_free(&source);
	if (!replacement) {
		if (tree->status == TG_OK)
			tree->status = TG_INVALID;
		return false;
	}
	option_node **slot = child_slot(root, "outputs");

	if (!*slot)
		*slot = node_create(tree, node_array, "outputs");
	if (!*slot || (*slot)->kind != node_array) {
		node_free(tree, replacement);
		if (tree->status == TG_OK)
			tree->status = TG_INVALID;
		return false;
	}
	option_node **output = &(*slot)->children;

	for (; *output; output = &(*output)->next) {
		if ((*output)->kind != node_table)
			continue;
		option_node *name = *child_slot(*output, "id");
		char *text = nullptr;
		bool match = name && name->kind == node_scalar &&
			toml_rtos(name->raw, &text) == 0 && !strcmp(text, id);

		free(text);
		if (match)
			break;
	}
	free(replacement->key);
	replacement->key = nullptr;
	if (*output) {
		option_node *old = *output;

		replacement->next = old->next;
		old->next = nullptr;
		node_free(tree, old);
	}
	*output = replacement;
	return true;
}

static bool append_import(option_tree *tree, option_node *root,
	tg_buffer imported)
{
	option_node *addition = parse_tree(tree,
		{imported.data, imported.size});

	if (!addition)
		return false;
	option_node *inputs = *child_slot(addition, "inputs");
	option_node **destination = child_slot(root, "inputs");
	bool ok = inputs && inputs->kind == node_array &&
		(!*destination || (*destination)->kind == node_array);

	if (ok) {
		if (!*destination)
			*destination = node_create(tree, node_array, "inputs");
		ok = *destination != nullptr;
	}
	if (ok) {
		option_node **tail = &(*destination)->children;

		while (*tail)
			tail = &(*tail)->next;
		*tail = inputs->children;
		inputs->children = nullptr;
	}
	node_free(tree, addition);
	if (!ok && tree->status == TG_OK)
		tree->status = TG_INVALID;
	return ok;
}

tg_status tg_cli_configure(int argc, const char *const *argv, const char *cwd,
	tg_config **out, tg_import_files **files, tg_diagnostic *diagnostic)
{
	if (diagnostic)
		*diagnostic = {};
	if (argc < 0 || argc > 1024 || argc % 2 || (argc && !argv) ||
		!absolute_path(cwd) || !out || *out || !files || *files)
		return cli_error(diagnostic, TG_INVALID, "arguments");
	const char *config_path = nullptr, *import_path = nullptr;
	size_t argument_bytes = 0;

	for (int i = 0; i < argc; i += 2) {
		if (!argv[i] || !argv[i + 1] || !*argv[i + 1])
			return cli_error(diagnostic, TG_INVALID, "arguments");
		argument_bytes += strlen(argv[i]) + strlen(argv[i + 1]);
		if (argument_bytes > 8 * 1024 * 1024)
			return cli_error(diagnostic, TG_LIMIT, "arguments");
		if (!strcmp(argv[i], "--config") && !config_path)
			config_path = argv[i + 1];
		else if (!strcmp(argv[i], "--import-zip") && !import_path)
			import_path = argv[i + 1];
		else if (strcmp(argv[i], "--set") &&
			strcmp(argv[i], "--output") && !scalar_flag(argv[i]))
			return cli_error(diagnostic, TG_INVALID, "arguments");
	}
	tg_vec path{}, base{};
	tg_buffer source{};
	tg_status status = tg_text(&base, cwd) ? TG_OK : TG_NO_MEMORY;

	if (status == TG_OK && config_path) {
		if (!full_path(&path, cwd, config_path))
			status = TG_NO_MEMORY;
		else
			status = tg_read_file((const char *)path.data,
				8 * 1024 * 1024, false, &source);
		char *slash = path.data ? strrchr((char *)path.data, '/') :
			nullptr;
#ifdef _WIN32
		char *back = path.data ? strrchr((char *)path.data, '\\') :
			nullptr;

		if (back && (!slash || back > slash))
			slash = back;
#endif
		if (status == TG_OK && slash) {
			bool root = slash == (char *)path.data;
#ifdef _WIN32
			root = root || (slash == (char *)path.data + 2 &&
				((char *)path.data)[1] == ':');
#endif
			slash[root ? 1 : 0] = 0;
			tg_vec_free(&base);
			if (!tg_text(&base, (const char *)path.data))
				status = TG_NO_MEMORY;
		}
	}
	option_tree tree{};
	option_node *root = nullptr;
	const char *key = "config";
	tg_import_files *imported_files = nullptr;

	if (status == TG_OK) {
		const char defaults[] = "version=1\n";
		tg_bytes text = config_path ?
			tg_bytes{source.data, source.size} :
			tg_bytes{(const uint8_t *)defaults, strlen(defaults)};

		root = parse_tree(&tree, text);
		status = tree.status;
	}
	tg_buffer_release(&source);
	tg_vec_free(&path);
	if (status == TG_OK && import_path) {
		key = "import";
		tg_buffer imported{};

		if (!full_path(&path, cwd, import_path))
			status = TG_NO_MEMORY;
		else
			status = tg_import_zip((const char *)path.data,
				&imported, &imported_files, diagnostic);
		if (status == TG_OK && !append_import(&tree, root, imported))
			status = tree.status;
		tg_buffer_release(&imported);
		tg_vec_free(&path);
	}
	for (int i = 0; status == TG_OK && i < argc; i += 2) {
		const char *flag = argv[i], *value = argv[i + 1];
		const scalar_option *scalar = scalar_flag(flag);

		if (!strcmp(flag, "--output")) {
			key = "output";
			if (!output_setting(&tree, root, value))
				status = tree.status;
		} else if (!strcmp(flag, "--set") || scalar) {
			key = "set";
			tg_vec path_text{}, raw{};
			const char *assignment = scalar ? scalar->path : value;
			const char *equal = scalar ? nullptr :
				strchr(value, '=');
			bool ok = scalar || (equal && equal != value);

			if (ok && scalar) {
				ok = tg_text(&path_text, assignment);
				if (scalar->text)
					ok = ok && emit_string(&raw, value);
				else {
					for (const char *p = value; *p; ++p)
						if (*p < '0' || *p > '9')
							ok = false;
					ok = ok && tg_text(&raw, value);
				}
			} else if (ok) {
				ok = tg_append(&path_text, value,
					(size_t)(equal - value)) &&
					tg_text(&raw, equal + 1);
			}
			option_node *replacement = nullptr;

			if (ok && raw.data)
				replacement = parse_value(&tree,
					(const char *)raw.data);

			if (!replacement || !assign(&tree, root,
				(const char *)path_text.data, replacement))
				status = tree.status < 0 ? tree.status :
					TG_INVALID;
			tg_vec_free(&path_text);
			if (raw.data)
				OPENSSL_cleanse(raw.data, raw.size);
			tg_vec_free(&raw);
		}
	}
	tg_vec serialized{};

	if (status == TG_OK && !bounded_depth(root, 0)) {
		key = "limit";
		status = TG_LIMIT;
	}
	if (status == TG_OK && !serialize(&serialized, root, true))
		status = serialized.size > 8 * 1024 * 1024 ? TG_LIMIT :
			TG_NO_MEMORY;
	if (status == TG_OK)
		status = tg_config_parse({serialized.data, serialized.size},
			(const char *)base.data, out, diagnostic);
	node_free(&tree, root);
	tg_vec_free(&serialized);
	tg_vec_free(&base);
	if (status < 0) {
		tg_import_zip_destroy(imported_files);
		if (!diagnostic || diagnostic->status == TG_OK)
			return cli_error(diagnostic, status, key);
		return status;
	}
	*files = imported_files;
	return TG_OK;
}
