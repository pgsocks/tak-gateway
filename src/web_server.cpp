#include "web_server.h"
#include "web_assets.h"
#include "managed_runtime.h"
#include "cli_options.h"
#include <civetweb.h>
#include <yyjson.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

static const size_t json_limit = 1024 * 1024;
static const size_t zip_limit = 16 * 1024 * 1024;
static const size_t asset_limit = 8 * 1024 * 1024;

#ifdef _WIN32
struct web_mutex {
	CRITICAL_SECTION value;
};
static void mutex_init(web_mutex *m)
{
	InitializeCriticalSection(&m->value);
}
static void mutex_lock(web_mutex *m)
{
	EnterCriticalSection(&m->value);
}
static void mutex_unlock(web_mutex *m)
{
	LeaveCriticalSection(&m->value);
}
static void mutex_clear(web_mutex *m)
{
	DeleteCriticalSection(&m->value);
}
static void pause_worker(void)
{
	Sleep(5);
}
#else
struct web_mutex {
	pthread_mutex_t value;
};
static void mutex_init(web_mutex *m)
{
	pthread_mutex_init(&m->value, nullptr);
}
static void mutex_lock(web_mutex *m)
{
	pthread_mutex_lock(&m->value);
}
static void mutex_unlock(web_mutex *m)
{
	pthread_mutex_unlock(&m->value);
}
static void mutex_clear(web_mutex *m)
{
	pthread_mutex_destroy(&m->value);
}
static void pause_worker(void)
{
	usleep(5000);
}
#endif

union json_allocation {
	size_t size;
	long double alignment;
	void *pointer;
};

static void *json_allocate(void *, size_t size)
{
	if (size > SIZE_MAX - sizeof(json_allocation))
		return nullptr;
	json_allocation *memory = static_cast<json_allocation *>(
		malloc(sizeof(*memory) + size));

	if (!memory)
		return nullptr;
	memory->size = size;
	return memory + 1;
}

static void json_release(void *, void *pointer)
{
	if (!pointer)
		return;
	json_allocation *memory = static_cast<json_allocation *>(pointer) - 1;
	size_t size = memory->size;

	OPENSSL_cleanse(memory, sizeof(*memory) + size);
	free(memory);
}

static void *json_reallocate(void *user, void *pointer, size_t, size_t size)
{
	void *replacement = json_allocate(user, size);

	if (!replacement)
		return nullptr;
	if (pointer) {
		json_allocation *memory =
			static_cast<json_allocation *>(pointer) - 1;

		memcpy(replacement, pointer,
			memory->size < size ? memory->size : size);
		json_release(user, pointer);
	}
	return replacement;
}

static const yyjson_alc json_allocator = {
	json_allocate, json_reallocate, json_release, nullptr
};

enum job_kind { job_edit, job_remove, job_import, job_confirm };
enum job_state { job_pending, job_working, job_prepared, job_ready,
	job_committed, job_failed };

struct web_asset {
	char id[65];
	char kind[24];
	tg_buffer bytes;
	bool used;
	uint64_t created_ms;
	web_asset *next;
};

struct created_asset {
	tg_buffer path;
	created_asset *next;
};

struct web_job {
	uint64_t id;
	uint64_t expected;
	uint64_t created_ms;
	job_kind kind;
	job_state state;
	bool output;
	bool confirming;
	char *endpoint;
	tg_buffer body;
	tg_buffer source;
	tg_buffer response;
	tg_config *candidate;
	tg_import_files *imports;
	web_job *parent;
	created_asset *created;
	tg_status error;
	tg_diagnostic diagnostic;
	web_job *next;
};

struct tg_web_app {
	tg_store *store;
	tg_config *config;
	tg_runtime *runtime;
	tg_web_settings web;
	uint64_t revision;
	uint64_t active_revision;
	uint64_t published_revision;
	tg_status apply_error;
	tg_diagnostic apply_diagnostic;
	uint64_t sequence;
	char token[65];
	mg_context *http;
	web_mutex mutex;
	web_mutex store_mutex;
	tg_buffer snapshot;
	tg_buffer source;
	web_job *jobs;
	web_asset *assets;
	size_t job_count;
	size_t asset_bytes;
	size_t job_bytes;
	bool stopping;
	bool edits_blocked;
	bool worker_stop;
	bool worker_started;
#ifdef _WIN32
	HANDLE worker;
#else
	pthread_t worker;
#endif
};

static tg_status web_error(tg_diagnostic *d, tg_status s, const char *key)
{
	if (d) {
		*d = {};
		d->status = s;
		snprintf(d->key, sizeof(d->key), "web.%s", key);
	}
	return s;
}

static void secret_free(tg_buffer *buffer)
{
	if (buffer->data)
		OPENSSL_cleanse(buffer->data, buffer->size);
	tg_buffer_release(buffer);
}

static tg_status copy_text(const char *text, tg_buffer *out)
{
	if (!text || !tg_empty(out))
		return TG_INVALID;
	uint8_t *copy = (uint8_t *)tg_strdup(text);

	if (!copy)
		return TG_NO_MEMORY;
	*out = {copy, strlen(text)};
	return TG_OK;
}

static bool random_id(char *out)
{
	unsigned char bytes[32];
	const char hex[] = "0123456789abcdef";

	if (RAND_bytes(bytes, sizeof(bytes)) != 1)
		return false;
	for (size_t i = 0; i < sizeof(bytes); ++i) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 15];
	}
	out[64] = 0;
	return true;
}

static bool json_string(tg_vec *out, const char *value)
{
	return tg_text(out, "\"") && tg_escape(out, value ? value : "", true)
		&& tg_text(out, "\"");
}

static yyjson_mut_val *toml_table_json(yyjson_mut_doc *,
	const toml_table_t *);
static yyjson_mut_val *toml_array_json(yyjson_mut_doc *,
	const toml_array_t *);

static yyjson_mut_val *raw_json(yyjson_mut_doc *doc, const char *raw)
{
	char *text = nullptr;
	int boolean = 0;
	int64_t number = 0;
	double real = 0;

	if (!raw)
		return nullptr;
	if (toml_rtos(raw, &text) == 0) {
		yyjson_mut_val *result = yyjson_mut_strcpy(doc, text);

		free(text);
		return result;
	}
	if (toml_rtob(raw, &boolean) == 0)
		return yyjson_mut_bool(doc, boolean != 0);
	if (toml_rtoi(raw, &number) == 0)
		return yyjson_mut_sint(doc, number);
	if (toml_rtod(raw, &real) == 0)
		return yyjson_mut_real(doc, real);
	return nullptr;
}

static yyjson_mut_val *toml_table_json(yyjson_mut_doc *doc,
	const toml_table_t *table)
{
	yyjson_mut_val *object = yyjson_mut_obj(doc);

	for (int i = 0; object; ++i) {
		const char *key = toml_key_in(table, i);

		if (!key)
			break;
		const toml_table_t *child = toml_table_in(table, key);
		const toml_array_t *array = toml_array_in(table, key);
		yyjson_mut_val *value = child ? toml_table_json(doc, child) :
			array ? toml_array_json(doc, array) :
			raw_json(doc, toml_raw_in(table, key));

		if (!value || !yyjson_mut_obj_add_val(doc, object, key, value))
			return nullptr;
	}
	return object;
}

static yyjson_mut_val *toml_array_json(yyjson_mut_doc *doc,
	const toml_array_t *array)
{
	yyjson_mut_val *result = yyjson_mut_arr(doc);

	for (int i = 0; result && i < toml_array_nelem(array); ++i) {
		const toml_table_t *table = toml_table_at(array, i);
		const toml_array_t *nested = toml_array_at(array, i);
		yyjson_mut_val *value = table ? toml_table_json(doc, table) :
			nested ? toml_array_json(doc, nested) :
			raw_json(doc, toml_raw_at(array, i));

		if (!value || !yyjson_mut_arr_append(result, value))
			return nullptr;
	}
	return result;
}

static bool put(yyjson_mut_doc *doc, yyjson_mut_val *object,
	const char *key, yyjson_mut_val *value)
{
	return value && yyjson_mut_obj_put(object,
		yyjson_mut_strcpy(doc, key), value);
}

static void strip_key(yyjson_mut_val *object, const char *key)
{
	if (object)
		yyjson_mut_obj_remove_key(object, key);
}

static bool present(yyjson_mut_val *object, const char *key)
{
	yyjson_mut_val *value = object ? yyjson_mut_obj_get(object, key) :
		nullptr;

	return value && yyjson_mut_is_str(value) &&
		*yyjson_mut_get_str(value);
}

static void redact_input(yyjson_mut_doc *doc, yyjson_mut_val *input)
{
	yyjson_mut_val *tls = yyjson_mut_obj_get(input, "tls");
	yyjson_mut_val *auth = yyjson_mut_obj_get(input, "auth");
	const char *mode = present(tls, "pkcs12_file") ? "pkcs12" :
		present(tls, "key_file") ? "pem" : "none";

	put(doc, input, "identity_mode", yyjson_mut_strcpy(doc, mode));
	const char *fields[] = {"ca_file", "cert_file", "key_file",
		"pkcs12_file", "key_password_file", "pkcs12_password_file"};
	const char *markers[] = {"has_ca", "has_certificate", "has_key",
		"has_pkcs12", "has_key_password", "has_pkcs12_password"};

	for (size_t i = 0; tls && i < 6; ++i) {
		bool exists = present(tls, fields[i]);

		if (i == 4)
			exists = exists || present(tls, "key_password_env");
		if (i == 5)
			exists = exists || present(tls, "pkcs12_password_env");
		put(doc, tls, markers[i], yyjson_mut_bool(doc, exists));
		strip_key(tls, fields[i]);
	}
	strip_key(tls, "key_password_env");
	strip_key(tls, "pkcs12_password_env");
	if (auth) {
		put(doc, auth, "has_password", yyjson_mut_bool(doc,
			present(auth, "password_file") ||
			present(auth, "password_env")));
		strip_key(auth, "password_file");
		strip_key(auth, "password_env");
	}
	yyjson_mut_val *maintenance = yyjson_mut_obj_get(input, "maintenance");

	strip_key(maintenance, "identity_xml_file");
}

static yyjson_mut_val *configuration_json(yyjson_mut_doc *doc,
	const tg_config *config, bool redact)
{
	yyjson_mut_val *root = toml_table_json(doc, config->table);

	if (!root)
		return nullptr;
	const char *arrays[] = {"inputs", "outputs"};

	for (const char *key : arrays)
		if (!yyjson_mut_obj_get(root, key))
			put(doc, root, key, yyjson_mut_arr(doc));
	if (redact) {
		yyjson_mut_val *inputs = yyjson_mut_obj_get(root, "inputs");
		size_t count = yyjson_mut_arr_size(inputs);

		for (size_t i = 0; i < count; ++i)
			redact_input(doc, yyjson_mut_arr_get(inputs, i));
	}
	return root;
}

static bool json_copy_out(yyjson_mut_doc *doc, tg_buffer *out)
{
	size_t size = 0;
	char *text = yyjson_mut_write(doc, 0, &size);

	if (!text)
		return false;
	*out = {(uint8_t *)text, size};
	return true;
}

static bool json_valid(yyjson_val *value, unsigned depth, size_t *nodes)
{
	if (!value || depth > 32 || ++*nodes > 8192 ||
		(yyjson_is_str(value) && yyjson_get_len(value) !=
			strlen(yyjson_get_str(value))))
		return false;
	if (yyjson_is_obj(value)) {
		yyjson_obj_iter it = yyjson_obj_iter_with(value);
		yyjson_val *key;

		while ((key = yyjson_obj_iter_next(&it))) {
			yyjson_val *child = yyjson_obj_iter_get_val(key);

			if (yyjson_get_len(key) !=
				strlen(yyjson_get_str(key)) ||
				yyjson_obj_get(value, yyjson_get_str(key)) !=
					child ||
				!json_valid(child, depth + 1, nodes))
				return false;
		}
	} else if (yyjson_is_arr(value)) {
		for (size_t i = 0; i < yyjson_arr_size(value); ++i)
			if (!json_valid(yyjson_arr_get(value, i), depth + 1,
				nodes))
				return false;
	}
	return true;
}

static yyjson_doc *read_json(tg_buffer bytes)
{
	if (!bytes.size || bytes.size > json_limit)
		return nullptr;
	yyjson_doc *doc = yyjson_read_opts((char *)bytes.data,
		bytes.size, 0, &json_allocator, nullptr);
	size_t nodes = 0;

	if (doc && !json_valid(yyjson_doc_get_root(doc), 0, &nodes)) {
		yyjson_doc_free(doc);
		doc = nullptr;
	}
	return doc;
}

static const char *error_message(tg_status status)
{
	switch (status) {
	case TG_BAD_STATE:
		return "Configuration changed; refresh before saving again.";
	case TG_LIMIT:
		return "The request exceeds a configured resource limit.";
	case TG_IO:
		return "Unable to read or save configuration or credentials.";
	case TG_UNSUPPORTED:
		return "Unsupported configuration or credential format.";
	case TG_NO_MEMORY:
		return "Insufficient resources to complete the request.";
	default:
		return "Configuration or credential data is invalid.";
	}
}

static const char *state_name(job_state state)
{
	switch (state) {
	case job_ready:
		return "ready";
	case job_committed:
		return "committed";
	case job_failed:
		return "failed";
	default:
		return "pending";
	}
}

static bool refresh_snapshot(tg_web_app *app)
{
	yyjson_mut_doc *doc = yyjson_mut_doc_new(&json_allocator);
	yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : nullptr;
	yyjson_mut_val *config = root ?
		configuration_json(doc, app->config, true) : nullptr;
	tg_buffer status{};
	tg_buffer source{};
	tg_buffer snapshot{};
	bool ok = config && tg_runtime_snapshot(app->runtime, &status) == TG_OK;
	yyjson_doc *runtime = ok ? yyjson_read((const char *)status.data,
		status.size, 0) : nullptr;

	ok = ok && runtime &&
		put(doc, root, "revision", yyjson_mut_uint(doc,
			app->revision)) &&
		put(doc, root, "active_revision",
			yyjson_mut_uint(doc, app->active_revision)) &&
		put(doc, root, "token", yyjson_mut_strcpy(doc, app->token)) &&
		put(doc, root, "config", config) &&
		put(doc, root, "status", yyjson_val_mut_copy(doc,
			yyjson_doc_get_root(runtime)));
	if (ok) {
		yyjson_mut_val *web = yyjson_mut_obj(doc);

		ok = put(doc, web, "enabled",
			yyjson_mut_bool(doc, app->web.enabled)) &&
			put(doc, web, "bind", yyjson_mut_strcpy(doc,
				app->web.bind)) && put(doc, root, "web", web);
	}
	if (ok && app->apply_error < 0) {
		yyjson_mut_val *error = yyjson_mut_obj(doc);

		ok = put(doc, error, "code",
			yyjson_mut_sint(doc, app->apply_error)) &&
			put(doc, error, "key", yyjson_mut_strcpy(doc,
				app->apply_diagnostic.key)) &&
			put(doc, error, "message", yyjson_mut_strcpy(doc,
				error_message(app->apply_error))) &&
			put(doc, root, "apply_error", error);
	}
	if (ok) {
		yyjson_mut_doc_set_root(doc, root);
		ok = json_copy_out(doc, &snapshot) &&
			tg_config_export(app->config, &source, nullptr) ==
				TG_OK;
	}
	if (ok) {
		mutex_lock(&app->mutex);
		tg_buffer_release(&app->snapshot);
		secret_free(&app->source);
		app->snapshot = snapshot;
		app->source = source;
		app->published_revision = app->revision;
		mutex_unlock(&app->mutex);
	} else {
		tg_buffer_release(&snapshot);
		secret_free(&source);
	}
	yyjson_doc_free(runtime);
	yyjson_mut_doc_free(doc);
	tg_buffer_release(&status);
	return ok;
}

static bool to_toml(tg_vec *out, yyjson_mut_val *value, unsigned depth)
{
	if (!value || depth > 32 || out->size > 8 * 1024 * 1024)
		return false;
	if (yyjson_mut_is_str(value))
		return json_string(out, yyjson_mut_get_str(value));
	if (yyjson_mut_is_bool(value))
		return tg_text(out, yyjson_mut_get_bool(value) ? "true" :
			"false");
	if (yyjson_mut_is_uint(value))
		return tg_printf(out, "%llu",
			(unsigned long long)yyjson_mut_get_uint(value));
	if (yyjson_mut_is_sint(value))
		return tg_printf(out, "%lld",
			(long long)yyjson_mut_get_sint(value));
	if (yyjson_mut_is_real(value))
		return tg_printf(out, "%.17g", yyjson_mut_get_real(value));
	bool object = yyjson_mut_is_obj(value);

	if (!object && !yyjson_mut_is_arr(value))
		return false;
	if (!tg_text(out, object ? "{" : "["))
		return false;
	if (object) {
		yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(value);
		yyjson_mut_val *key;
		bool first = true;

		while ((key = yyjson_mut_obj_iter_next(&it))) {
			if ((!first && !tg_text(out, ",")) ||
				!json_string(out, yyjson_mut_get_str(key)) ||
				!tg_text(out, "=") || !to_toml(out,
					yyjson_mut_obj_iter_get_val(key),
						depth + 1))
				return false;
			first = false;
		}
	} else {
		for (size_t i = 0; i < yyjson_mut_arr_size(value); ++i)
			if ((i && !tg_text(out, ",")) || !to_toml(out,
				yyjson_mut_arr_get(value, i), depth + 1))
				return false;
	}
	return tg_text(out, object ? "}" : "]");
}

static tg_status config_from_json(yyjson_mut_val *root, const char *base,
	tg_config **out,
	tg_diagnostic *diagnostic)
{
	tg_vec toml{};
	yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(root);
	yyjson_mut_val *key;
	bool ok = true;

	while (ok && (key = yyjson_mut_obj_iter_next(&it)))
		ok = json_string(&toml, yyjson_mut_get_str(key)) &&
			tg_text(&toml, "=") && to_toml(&toml,
				yyjson_mut_obj_iter_get_val(key), 0) &&
			tg_text(&toml, "\n");
	tg_status status = ok ? tg_config_parse_managed(
		{toml.data, toml.size}, base, out, diagnostic) : TG_INVALID;

	if (toml.data)
		OPENSSL_cleanse(toml.data, toml.size);
	tg_vec_free(&toml);
	return status;
}

static yyjson_mut_val *find_record(yyjson_mut_val *array,
	const char *id, size_t *index)
{
	for (size_t i = 0; i < yyjson_mut_arr_size(array); ++i) {
		yyjson_mut_val *record = yyjson_mut_arr_get(array, i);
		yyjson_mut_val *key = yyjson_mut_obj_get(record, "id");

		if (yyjson_mut_is_str(key) &&
			!strcmp(yyjson_mut_get_str(key), id)) {
			if (index)
				*index = i;
			return record;
		}
	}
	return nullptr;
}

static bool merge_object(yyjson_mut_doc *doc, yyjson_mut_val *target,
	yyjson_val *patch, unsigned depth)
{
	if (!yyjson_mut_is_obj(target) || !yyjson_is_obj(patch) || depth > 8)
		return false;
	yyjson_obj_iter it = yyjson_obj_iter_with(patch);
	yyjson_val *key;

	while ((key = yyjson_obj_iter_next(&it))) {
		const char *name = yyjson_get_str(key);
		yyjson_val *value = yyjson_obj_iter_get_val(key);
		yyjson_mut_val *existing = yyjson_mut_obj_get(target, name);

		if (!strncmp(name, "has_", 4))
			continue;
		if (yyjson_is_null(value)) {
			strip_key(target, name);
			continue;
		}
		if (yyjson_is_obj(value)) {
			if (!existing || !yyjson_mut_is_obj(existing)) {
				existing = yyjson_mut_obj(doc);
				if (!put(doc, target, name, existing))
					return false;
			}
			if (!merge_object(doc, existing, value, depth + 1))
				return false;
		} else if (!put(doc, target, name,
			yyjson_val_mut_copy(doc, value))) {
			return false;
		}
	}
	return true;
}

static void remove_asset(tg_web_app *app, const char *path)
{
	mutex_lock(&app->store_mutex);
	tg_store_remove_asset(app->store, path, nullptr);
	mutex_unlock(&app->store_mutex);
}

static bool write_asset(tg_web_app *app, web_job *job,
	const char *extension, tg_bytes bytes, tg_buffer *path)
{
	created_asset *asset = static_cast<created_asset *>(
		calloc(1, sizeof(*asset)));

	if (!asset)
		return false;
	mutex_lock(&app->store_mutex);
	tg_status status = tg_store_write_asset(app->store, extension,
		bytes, &asset->path, &job->diagnostic);
	mutex_unlock(&app->store_mutex);

	if (status < 0) {
		free(asset);
		return false;
	}
	asset->next = job->created;
	job->created = asset;
	return copy_text((const char *)asset->path.data, path) == TG_OK;
}

static bool secret_setting(tg_web_app *app, web_job *job,
	yyjson_mut_doc *doc, yyjson_mut_val *object, const char *field,
	const char *file, const char *env)
{
	yyjson_mut_val *value = yyjson_mut_obj_get(object, field);
	bool ok = true;

	if (value && !yyjson_mut_is_str(value))
		return false;
	if (value && *yyjson_mut_get_str(value)) {
		const char *text = yyjson_mut_get_str(value);
		tg_buffer path{};

		ok = strlen(text) <= 65536 && write_asset(app, job, "secret",
			{(const uint8_t *)text, strlen(text)}, &path) &&
			put(doc, object, file, yyjson_mut_strcpy(doc,
				(const char *)path.data));
		strip_key(object, env);
		tg_buffer_release(&path);
	}
	strip_key(object, field);
	return ok;
}

static tg_status password_bytes(yyjson_mut_val *object, const char *file,
	const char *environment, tg_buffer *out)
{
	const char *path = yyjson_mut_get_str(yyjson_mut_obj_get(object, file));
	const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(object,
		environment));

	if (path) {
		tg_status status = tg_read_file(path, 65536, true, out);

		if (status < 0)
			return status;
		while (out->size && (out->data[out->size - 1] == '\n' ||
			out->data[out->size - 1] == '\r'))
			out->data[--out->size] = 0;
		return TG_OK;
	}
	if (name) {
		const char *value = getenv(name);

		if (!value || strlen(value) > 65536)
			return TG_IO;
		return copy_text(value, out);
	}
	return TG_OK;
}

static int pem_password(char *buffer, int size, int, void *user)
{
	tg_buffer *password = static_cast<tg_buffer *>(user);

	if (password->size > (size_t)size)
		return 0;
	if (password->size)
		memcpy(buffer, password->data, password->size);
	return (int)password->size;
}

static bool prepare_identity(tg_web_app *app, web_job *job,
	yyjson_mut_doc *doc, yyjson_mut_val *tls)
{
	const char *bundle = yyjson_mut_get_str(
		yyjson_mut_obj_get(tls, "pkcs12_file"));
	tg_buffer password{}, bytes{}, cert{}, key{};
	bool ok = true;

	if (bundle) {
		job->error = password_bytes(tls, "pkcs12_password_file",
			"pkcs12_password_env", &password);
		if (job->error == TG_OK)
			job->error = tg_read_file(bundle, asset_limit, true,
				&bytes);
		if (job->error == TG_OK)
			job->error = tg_import_pkcs12_identity(
				{bytes.data, bytes.size}, password.data ?
					(const char *)password.data : "",
				&cert, &key, &job->diagnostic);
		tg_buffer cert_path{}, key_path{};

		ok = job->error == TG_OK && write_asset(app, job, "pem",
			{cert.data, cert.size}, &cert_path) &&
			write_asset(app, job, "key", {key.data, key.size},
				&key_path) &&
			put(doc, tls, "cert_file", yyjson_mut_strcpy(doc,
				(const char *)cert_path.data)) &&
			put(doc, tls, "key_file", yyjson_mut_strcpy(doc,
				(const char *)key_path.data));
		if (ok) {
			strip_key(tls, "pkcs12_file");
			strip_key(tls, "pkcs12_password_file");
			strip_key(tls, "pkcs12_password_env");
			strip_key(tls, "key_password_file");
			strip_key(tls, "key_password_env");
		}
		tg_buffer_release(&cert_path);
		tg_buffer_release(&key_path);
	}
	secret_free(&password);
	secret_free(&bytes);
	tg_buffer_release(&cert);
	secret_free(&key);
	if (!ok)
		return false;
	const char *ca = yyjson_mut_get_str(yyjson_mut_obj_get(tls, "ca_file"));
	const char *certificate = yyjson_mut_get_str(
		yyjson_mut_obj_get(tls, "cert_file"));
	const char *private_key = yyjson_mut_get_str(
		yyjson_mut_obj_get(tls, "key_file"));
	SSL_CTX *context = SSL_CTX_new(TLS_client_method());

	job->error = password_bytes(tls, "key_password_file",
		"key_password_env", &password);
	if (context) {
		SSL_CTX_set_default_passwd_cb(context, pem_password);
		SSL_CTX_set_default_passwd_cb_userdata(context, &password);
	}
	ok = context && job->error == TG_OK && ca &&
		SSL_CTX_load_verify_locations(context, ca, nullptr) == 1;
	if (ok && certificate)
		ok = private_key && SSL_CTX_use_certificate_chain_file(context,
			certificate) ==
				1 && SSL_CTX_use_PrivateKey_file(context,
				private_key, SSL_FILETYPE_PEM) == 1 &&
			SSL_CTX_check_private_key(context) == 1;
	SSL_CTX_free(context);
	secret_free(&password);
	ERR_clear_error();
	if (!ok) {
		job->error = TG_INVALID;
		web_error(&job->diagnostic, TG_INVALID, "credentials");
	}
	return ok;
}

static bool credential_assets(tg_web_app *app, web_job *job,
	yyjson_mut_doc *doc, yyjson_mut_val *input)
{
	yyjson_mut_val *tls = yyjson_mut_obj_get(input, "tls");
	yyjson_mut_val *auth = yyjson_mut_obj_get(input, "auth");
	bool changed = tls && (yyjson_mut_obj_get(tls, "ca_asset") ||
		yyjson_mut_obj_get(tls, "certificate_asset") ||
		yyjson_mut_obj_get(tls, "key_asset") ||
		yyjson_mut_obj_get(tls, "pkcs12_asset") ||
		yyjson_mut_obj_get(tls, "key_password") ||
		yyjson_mut_obj_get(tls, "pkcs12_password"));
	const char *clear[] = {"key_password_file", "key_password_env",
		"pkcs12_password_file", "pkcs12_password_env"};

	if (tls && yyjson_mut_get_bool(yyjson_mut_obj_get(tls,
		"clear_secrets")))
		for (const char *key : clear)
			strip_key(tls, key);
	strip_key(tls, "clear_secrets");
	yyjson_mut_val *identity = yyjson_mut_obj_get(input, "identity_mode");
	const char *mode = yyjson_mut_get_str(identity);

	if (mode && strcmp(mode, "pem")) {
		strip_key(tls, "cert_file");
		strip_key(tls, "key_file");
		strip_key(tls, "key_password_file");
		strip_key(tls, "key_password_env");
	}
	if (mode && strcmp(mode, "pkcs12")) {
		strip_key(tls, "pkcs12_file");
		strip_key(tls, "pkcs12_password_file");
		strip_key(tls, "pkcs12_password_env");
	}
	if (identity && (!mode || (strcmp(mode, "pem") &&
		strcmp(mode, "pkcs12") && strcmp(mode, "none"))))
		return false;
	strip_key(input, "identity_mode");
	const char *asset_fields[] = {"ca_asset", "certificate_asset",
		"key_asset", "pkcs12_asset"};
	const char *file_fields[] = {"ca_file", "cert_file", "key_file",
		"pkcs12_file"};
	const char *kinds[] = {"ca", "certificate", "key", "pkcs12"};

	for (size_t i = 0; tls && i < 4; ++i) {
		yyjson_mut_val *reference = yyjson_mut_obj_get(tls,
			asset_fields[i]);

		if (!reference)
			continue;
		if (!yyjson_mut_is_str(reference))
			return false;
		const char *id = yyjson_mut_get_str(reference);
		web_asset *asset = nullptr;

		mutex_lock(&app->mutex);
		for (web_asset *at = app->assets; at; at = at->next)
			if (!strcmp(at->id, id) && !strcmp(at->kind,
				kinds[i])) {
				asset = at;
				break;
			}
		mutex_unlock(&app->mutex);
		tg_buffer path{};
		bool ok = asset && write_asset(app, job,
			i == 3 ? "p12" : "pem",
			{asset->bytes.data, asset->bytes.size}, &path) &&
			put(doc, tls, file_fields[i], yyjson_mut_strcpy(doc,
				(const char *)path.data));

		tg_buffer_release(&path);
		strip_key(tls, asset_fields[i]);
		if (!ok)
			return false;
		mutex_lock(&app->mutex);
		asset->used = true;
		mutex_unlock(&app->mutex);
	}
	if (tls && (!secret_setting(app, job, doc, tls, "key_password",
		"key_password_file", "key_password_env") ||
		!secret_setting(app, job, doc, tls, "pkcs12_password",
			"pkcs12_password_file", "pkcs12_password_env")))
		return false;
	if (changed && !prepare_identity(app, job, doc, tls))
		return false;
	if (auth) {
		if (yyjson_mut_get_bool(yyjson_mut_obj_get(auth,
			"clear_secrets"))) {
			strip_key(auth, "password_file");
			strip_key(auth, "password_env");
		}
		strip_key(auth, "clear_secrets");
		if (!secret_setting(app, job, doc, auth, "password",
			"password_file", "password_env"))
			return false;
		const char *auth_mode = yyjson_mut_get_str(
			yyjson_mut_obj_get(auth, "mode"));

		if (auth_mode && !strcmp(auth_mode, "none")) {
			strip_key(auth, "username");
			strip_key(auth, "password_file");
			strip_key(auth, "password_env");
		}
	}
	return true;
}

static bool valid_id(const char *id);

static yyjson_mut_val *record_candidate(tg_web_app *app, web_job *job,
	yyjson_mut_doc *doc, yyjson_mut_val *old, yyjson_val *patch)
{
	yyjson_mut_val *record = old ? yyjson_mut_val_mut_copy(doc, old) :
		yyjson_mut_obj(doc);
	const char *old_kind = old ? yyjson_mut_get_str(
		yyjson_mut_obj_get(old, "kind")) : nullptr;
	const char *new_kind = yyjson_get_str(yyjson_obj_get(patch, "kind"));

	if (old_kind && new_kind && strcmp(old_kind, new_kind)) {
		record = yyjson_mut_obj(doc);
		const char *common[] = {"id", "name", "enabled", "queue_bytes",
			"queue_records"};

		for (const char *key : common) {
			yyjson_mut_val *value = yyjson_mut_obj_get(old, key);

			if (value)
				put(doc, record, key,
					yyjson_mut_val_mut_copy(doc, value));
		}
	}
	if (!record || !merge_object(doc, record, patch, 0))
		return nullptr;
	if (*job->endpoint)
		put(doc, record, "id", yyjson_mut_strcpy(doc, job->endpoint));
	else if (!present(record, "id")) {
		char id[65];

		if (!random_id(id) || !put(doc, record, "id",
			yyjson_mut_strcpy(doc, id)))
			return nullptr;
	}
	const char *effective_kind = yyjson_mut_get_str(
		yyjson_mut_obj_get(record, "kind"));

	if (job->output && effective_kind &&
		!strcmp(effective_kind, "tcp")) {
		const char *mode = yyjson_mut_get_str(
			yyjson_mut_obj_get(record, "mode"));

		if (mode && !strcmp(mode, "connect")) {
			strip_key(record, "bind");
			strip_key(record, "max_clients");
		} else {
			strip_key(record, "host");
		}
	}
	if (!job->output) {
		const char *protocol = yyjson_mut_get_str(
			yyjson_mut_obj_get(record, "protocol"));

		if (protocol && strcmp(protocol, "negotiated"))
			strip_key(record, "prefer");
		if (!protocol || strcmp(protocol, "protobuf-only"))
			strip_key(record, "version");
	}
	if (!job->output && !credential_assets(app, job, doc, record))
		return nullptr;
	if (!*job->endpoint) {
		const char *id = yyjson_mut_get_str(
			yyjson_mut_obj_get(record, "id"));

		if (!id || !valid_id(id))
			return nullptr;
		free(job->endpoint);
		job->endpoint = tg_strdup(id);
		if (!job->endpoint)
			return nullptr;
	}
	return record;
}

static void process_edit(tg_web_app *app, web_job *job)
{
	tg_config *base = nullptr;
	tg_status status = tg_config_parse_managed(
		{job->source.data, job->source.size},
			tg_store_path(app->store), &base,
		&job->diagnostic);
	yyjson_mut_doc *doc = yyjson_mut_doc_new(&json_allocator);
	yyjson_mut_val *root = status == TG_OK && doc ?
		configuration_json(doc, base, false) : nullptr;
	yyjson_doc *request = read_json(job->body);
	yyjson_val *body = request ? yyjson_doc_get_root(request) : nullptr;
	const char *collection = job->output ? "outputs" : "inputs";
	yyjson_mut_val *array = root ? yyjson_mut_obj_get(root, collection) :
		nullptr;
	size_t index = 0;
	yyjson_mut_val *old = array && *job->endpoint ?
		find_record(array, job->endpoint, &index) : nullptr;
	bool ok = root && body && array && (!*job->endpoint || old);

	if (ok && job->kind == job_remove)
		ok = yyjson_mut_arr_remove(array, index) != nullptr;
	else if (ok) {
		yyjson_val *patch = yyjson_obj_get(body,
			job->output ? "output" : "input");
		yyjson_mut_val *record = yyjson_is_obj(patch) ?
			record_candidate(app, job, doc, old, patch) : nullptr;

		ok = record && (old ?
			yyjson_mut_arr_replace(array, index, record) !=
				nullptr :
			yyjson_mut_arr_append(array, record));
	}
	if (ok)
		status = config_from_json(root, tg_store_path(app->store),
			&job->candidate, &job->diagnostic);
	else if (status == TG_OK)
		status = job->error < 0 ? job->error : TG_INVALID;
	job->error = status;
	yyjson_doc_free(request);
	yyjson_mut_doc_free(doc);
	tg_config_destroy(base);
}

static bool preview_response(web_job *job)
{
	yyjson_mut_doc *doc = yyjson_mut_doc_new(&json_allocator);
	yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : nullptr;
	yyjson_mut_val *config = root ?
		configuration_json(doc, job->candidate, true) : nullptr;
	yyjson_mut_val *preview = root ? yyjson_mut_obj(doc) : nullptr;
	bool ok = config && preview && put(doc, preview, "inputs",
		yyjson_mut_obj_get(config, "inputs")) &&
		put(doc, root, "preview", preview);

	if (ok) {
		bool password = false, enrollment = false;

		tg_import_zip_requirements(job->imports, &password,
			&enrollment);
		yyjson_mut_val *warnings = yyjson_mut_arr(doc);

		if (password)
			yyjson_mut_arr_append(warnings, yyjson_mut_strcpy(doc,
				"The package requests password authentication. "
				"Configure credentials if required "
				"by the server."));
		if (enrollment)
			yyjson_mut_arr_append(warnings, yyjson_mut_strcpy(doc,
				"Certificate enrollment is not performed. "
				"The supplied client identity will be used."));
		ok = put(doc, preview, "warnings", warnings);
		yyjson_mut_doc_set_root(doc, root);
		ok = ok && json_copy_out(doc, &job->response);
	}
	yyjson_mut_doc_free(doc);
	return ok;
}

static void process_import(tg_web_app *app, web_job *job)
{
	tg_buffer path{};
	tg_buffer imported{};
	tg_vec source{};

	job->error = write_asset(app, job, "zip",
		{job->body.data, job->body.size}, &path) ? TG_OK : TG_IO;
	if (job->error == TG_OK)
		job->error = tg_import_zip((const char *)path.data, &imported,
			&job->imports, &job->diagnostic);
	if (path.data)
		remove_asset(app, (const char *)path.data);
	tg_buffer_release(&path);
	if (job->error == TG_OK) {
		bool ok = tg_text(&source, "version=1\n") &&
			tg_append(&source, imported.data, imported.size);

		job->error = ok ? tg_config_parse_managed(
			{source.data, source.size}, tg_store_path(app->store),
			&job->candidate,
			&job->diagnostic) : TG_NO_MEMORY;
	}
	if (job->error == TG_OK) {
		for (size_t i = 0; i < job->candidate->input_count; ++i) {
			char id[65];

			if (!random_id(id)) {
				job->error = TG_NO_MEMORY;
				break;
			}
			job->candidate->inputs[i].id =
				tg_config_string(job->candidate, id);
		}
		/* Reparse so table metadata includes the generated IDs. */
		tg_buffer normalized{};
		tg_config *copy = nullptr;

		if (job->error == TG_OK)
			job->error = tg_config_export(job->candidate,
				&normalized,
				&job->diagnostic);
		if (job->error == TG_OK)
			job->error = tg_config_parse_managed(
				{normalized.data, normalized.size},
				tg_store_path(app->store), &copy,
				&job->diagnostic);
		if (job->error == TG_OK) {
			tg_config_destroy(job->candidate);
			job->candidate = copy;
			if (!preview_response(job))
				job->error = TG_NO_MEMORY;
		}
		tg_buffer_release(&normalized);
	}
	tg_buffer_release(&imported);
	tg_vec_free(&source);
}

static void process_confirm(tg_web_app *app, web_job *job)
{
	tg_config *base = nullptr;
	job->error = tg_config_parse_managed(
		{job->source.data, job->source.size},
			tg_store_path(app->store), &base,
		&job->diagnostic);
	yyjson_mut_doc *doc = yyjson_mut_doc_new(&json_allocator);
	yyjson_mut_val *root = job->error == TG_OK && doc ?
		configuration_json(doc, base, false) : nullptr;
	yyjson_mut_val *original = doc && job->parent->candidate ?
		configuration_json(doc, job->parent->candidate, false) :
			nullptr;
	yyjson_doc *request = read_json(job->body);
	yyjson_val *body = request ? yyjson_doc_get_root(request) : nullptr;
	yyjson_val *inputs = body ? yyjson_obj_get(body, "inputs") : nullptr;
	bool ok = root && original && yyjson_is_arr(inputs) &&
		yyjson_arr_size(inputs) && yyjson_arr_size(inputs) <= 32;
	yyjson_mut_val *array = root ? yyjson_mut_obj_get(root, "inputs") :
		nullptr;
	yyjson_mut_val *originals = original ?
		yyjson_mut_obj_get(original, "inputs") : nullptr;

	for (size_t i = 0; ok && i < yyjson_arr_size(inputs); ++i) {
		yyjson_val *patch = yyjson_arr_get(inputs, i);
		const char *id = yyjson_get_str(yyjson_obj_get(patch, "id"));
		yyjson_mut_val *old = id ? find_record(originals, id, nullptr) :
			nullptr;
		web_job item{};

		item.endpoint = tg_strdup(id ? id : "");
		item.created = job->created;
		yyjson_mut_val *record = old ?
			record_candidate(app, &item, doc, old, patch) : nullptr;

		job->created = item.created;
		if (!record && item.error < 0) {
			job->error = item.error;
			job->diagnostic = item.diagnostic;
		}
		free(item.endpoint);
		ok = record && yyjson_mut_arr_append(array, record);
	}
	if (ok)
		job->error = config_from_json(root, tg_store_path(app->store),
			&job->candidate, &job->diagnostic);
	else if (job->error == TG_OK)
		job->error = TG_INVALID;
	yyjson_doc_free(request);
	yyjson_mut_doc_free(doc);
	tg_config_destroy(base);
}

#ifdef _WIN32
static DWORD WINAPI worker_main(void *user)
#else
static void *worker_main(void *user)
#endif
{
	tg_web_app *app = static_cast<tg_web_app *>(user);

	for (;;) {
		mutex_lock(&app->mutex);
		bool stop = app->worker_stop;
		web_job *job = nullptr;

		for (web_job *at = app->jobs; !stop && at; at = at->next)
			if (at->state == job_pending) {
				job = at;
				job->state = job_working;
				break;
			}
		mutex_unlock(&app->mutex);
		if (stop)
			break;
		if (!job) {
			pause_worker();
			continue;
		}
		if (job->kind == job_import)
			process_import(app, job);
		else if (job->kind == job_confirm)
			process_confirm(app, job);
		else
			process_edit(app, job);
		size_t released = job->body.size + job->source.size;

		secret_free(&job->body);
		secret_free(&job->source);
		mutex_lock(&app->mutex);
		app->job_bytes -= released;
		job->state = job->error < 0 ? job_failed :
			job->kind == job_import ? job_ready : job_prepared;
		if (job->state == job_failed && job->parent) {
			job->parent->confirming = false;
			job->parent = nullptr;
		}
		mutex_unlock(&app->mutex);
	}
#ifdef _WIN32
	return 0;
#else
	return nullptr;
#endif
}

static int response(mg_connection *connection, int status,
	const char *mime, tg_bytes body)
{
	mg_printf(connection, "HTTP/1.1 %d OK\r\nContent-Type: %s\r\n"
		"Content-Length: %llu\r\nConnection: close\r\n"
		"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
		"Content-Security-Policy: default-src 'self'; "
		"style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; "
		"base-uri 'none'\r\n\r\n", status, mime,
		(unsigned long long)body.size);
	for (size_t at = 0; at < body.size;) {
		int n = mg_write(connection, body.data + at, body.size - at);

		if (n <= 0)
			break;
		at += (size_t)n;
	}
	return status;
}

static int api_error(mg_connection *connection, int status, const char *code,
	const char *message)
{
	tg_vec out{};

	tg_text(&out, "{\"error\":{\"code\":");
	json_string(&out, code);
	tg_text(&out, ",\"message\":");
	json_string(&out, message);
	tg_text(&out, "}}");
	int result = response(connection, status, "application/json",
		{out.data, out.size});

	tg_vec_free(&out);
	return result;
}

static bool authority(const char *text, char *host, size_t size,
	unsigned *port)
{
	if (!text || !*text || strlen(text) >= size)
		return false;
	strcpy(host, text);
	char *value = nullptr;

	if (*host == '[') {
		char *close = strchr(host, ']');

		if (!close || close[1] != ':')
			return false;
		*close = 0;
		value = close + 2;
		memmove(host, host + 1, strlen(host));
	} else {
		value = strrchr(host, ':');
		if (!value || strchr(host, ':') != value)
			return false;
		*value++ = 0;
	}
	if (!*host || !*value)
		return false;
	unsigned number = 0;

	for (; *value; ++value) {
		if (*value < '0' || *value > '9' || number > 65535 / 10)
			return false;
		number = number * 10 + (unsigned)(*value - '0');
		if (number > 65535)
			return false;
	}
	*port = number;
	return number != 0;
}

static bool valid_host(tg_web_app *app, mg_connection *connection)
{
	const mg_request_info *info = mg_get_request_info(connection);
	const char *header = mg_get_header(connection, "Host");
	char host[256], bound[256], default_port[256];
	unsigned port = 0, selected = 0;

	if (header && info->server_port == 80 &&
		(!strchr(header, ':') || header[strlen(header) - 1] == ']')) {
		if (snprintf(default_port, sizeof(default_port), "%s:80",
			header)
			>= (int)sizeof(default_port))
			return false;
		header = default_port;
	}
	if (!authority(header, host, sizeof(host), &port) ||
		port != (unsigned)info->server_port ||
		!authority(app->web.bind, bound, sizeof(bound), &selected))
		return false;
	for (char *at = host; *at; ++at)
		if (*at >= 'A' && *at <= 'Z')
			*at += 'a' - 'A';
	unsigned char address[16]{}, binding[16]{};
	int family = inet_pton(AF_INET, host, address) == 1 ? AF_INET :
		inet_pton(AF_INET6, host, address) == 1 ? AF_INET6 : 0;
	bool wildcard = !strcmp(bound, "0.0.0.0") || !strcmp(bound, "::");
	bool loopback = !strcmp(bound, "127.0.0.1") ||
		!strcmp(bound, "::1") || !strcmp(bound, "localhost");

	if (!strcmp(host, "localhost"))
		return wildcard || loopback;
	if (!family)
		return false;
	if (wildcard)
		return true;
	if (!strcmp(bound, "localhost"))
		return !strcmp(host, "127.0.0.1") || !strcmp(host, "::1");
	return inet_pton(family, bound, binding) == 1 &&
		!memcmp(address, binding, family == AF_INET ? 4 : 16);
}

static bool authorized(tg_web_app *app, mg_connection *connection)
{
	const char *token = mg_get_header(connection, "X-TG-Token");
	const char *origin = mg_get_header(connection, "Origin");
	const char *host = mg_get_header(connection, "Host");

	if (!token || strlen(token) != 64 ||
		CRYPTO_memcmp(token, app->token, 64))
		return false;
	if (!origin)
		return true;
	return host && !strncmp(origin, "http://", 7) &&
		!strcmp(origin + 7, host);
}

static bool read_body(mg_connection *connection, size_t limit, tg_buffer *out)
{
	const mg_request_info *info = mg_get_request_info(connection);

	if (info->content_length < 0 ||
		(uint64_t)info->content_length > limit)
		return false;
	size_t size = (size_t)info->content_length;
	uint8_t *data = static_cast<uint8_t *>(malloc(size + 1));

	if (!data)
		return false;
	for (size_t at = 0; at < size;) {
		int n = mg_read(connection, data + at, size - at);

		if (n <= 0) {
			OPENSSL_cleanse(data, size);
			free(data);
			return false;
		}
		at += (size_t)n;
	}
	data[size] = 0;
	*out = {data, size};
	return true;
}

static web_job *find_job(tg_web_app *app, uint64_t id)
{
	for (web_job *job = app->jobs; job; job = job->next)
		if (job->id == id)
			return job;
	return nullptr;
}

static const char *job_error_message(const web_job *job)
{
	if (!strcmp(job->diagnostic.key, "web.preview_cancelled"))
		return "ZIP preview canceled.";
	if (!strcmp(job->diagnostic.key, "web.preview_expired"))
		return "ZIP preview expired; upload it again.";
	if (!strcmp(job->diagnostic.key, "store.durability"))
		return "Published settings; storage durability is unconfirmed. "
			"Reload before retrying.";
	if (strstr(job->diagnostic.key, "client_identity"))
		return "No usable client identity was found. Check the bundle "
			"password and certificate; enrollment is unsupported.";
	return error_message(job->error);
}

static int job_response(tg_web_app *app, mg_connection *connection,
	uint64_t id)
{
	tg_vec out{};

	mutex_lock(&app->mutex);
	web_job *job = find_job(app, id);

	if (job) {
		tg_printf(&out, "{\"id\":%llu,\"state\":\"%s\","
			"\"revision\":%llu", (unsigned long long)job->id,
			state_name(job->state),
			(unsigned long long)app->published_revision);
		if (job->response.data && job->state == job_ready) {
			/* The preview response has one outer object wrapper. */
			tg_text(&out, ",");
			tg_append(&out, job->response.data + 1,
				job->response.size - 2);
		}
		if (job->state == job_failed) {
			tg_printf(&out, ",\"error\":{\"code\":%d,\"key\":",
				(int)job->error);
			json_string(&out, job->diagnostic.key);
			tg_text(&out, ",\"message\":");
			json_string(&out, job_error_message(job));
			tg_text(&out, "}");
		}
		tg_text(&out, "}");
	}
	mutex_unlock(&app->mutex);
	if (!job) {
		tg_vec_free(&out);
		return api_error(connection, 404, "not_found",
			"Job not found.");
	}
	int result = response(connection, 200, "application/json",
		{out.data, out.size});

	tg_vec_free(&out);
	return result;
}

static bool valid_id(const char *id)
{
	if (!id || !*id || strlen(id) > json_limit ||
		!tg_utf8({(const uint8_t *)id, strlen(id)}))
		return false;
	return true;
}

static bool path_id(const char *id)
{
	if (!valid_id(id))
		return false;
	for (; *id; ++id)
		if (!((*id >= 'a' && *id <= 'z') ||
			(*id >= 'A' && *id <= 'Z') ||
			(*id >= '0' && *id <= '9') || *id == '-' || *id == '_'))
			return false;
	return true;
}

static int upload_asset(tg_web_app *app, mg_connection *connection)
{
	const mg_request_info *info = mg_get_request_info(connection);
	const char *query = info->query_string;
	const char *kind = query && !strncmp(query, "kind=", 5) ? query + 5 :
		"";

	if (strcmp(kind, "ca") && strcmp(kind, "certificate") &&
		strcmp(kind, "key") && strcmp(kind, "pkcs12"))
		return api_error(connection, 400, "asset_kind",
			"Choose a supported credential file kind.");
	web_asset *asset = static_cast<web_asset *>(calloc(1, sizeof(*asset)));

	if (!asset)
		return api_error(connection, 503, "resources",
			"Insufficient resources.");
	if (!read_body(connection, asset_limit, &asset->bytes) ||
		!asset->bytes.size || !random_id(asset->id)) {
		secret_free(&asset->bytes);
		free(asset);
		return api_error(connection, 413, "asset_limit",
			"The credential file is empty or exceeds 8 MiB.");
	}
	snprintf(asset->kind, sizeof(asset->kind), "%s", kind);
	mutex_lock(&app->mutex);
	bool room = app->asset_bytes + asset->bytes.size <= 64 * 1024 * 1024;

	if (room) {
		asset->created_ms = tg_monotonic_ms();
		asset->next = app->assets;
		app->assets = asset;
		app->asset_bytes += asset->bytes.size;
	}
	mutex_unlock(&app->mutex);
	if (!room) {
		secret_free(&asset->bytes);
		free(asset);
		return api_error(connection, 503, "asset_limit",
			"Pending credential uploads exceed the memory budget.");
	}
	char result[100];
	int n = snprintf(result, sizeof(result), "{\"asset_id\":\"%s\"}",
		asset->id);

	return response(connection, 200, "application/json",
		{(const uint8_t *)result, (size_t)n});
}

static int enqueue(tg_web_app *app, mg_connection *connection,
	job_kind kind, bool output, const char *id, uint64_t parent_id)
{
	tg_buffer body{};
	const mg_request_info *info = mg_get_request_info(connection);
	size_t limit = kind == job_import ? zip_limit : json_limit;

	if (info->content_length < 0 ||
		(uint64_t)info->content_length > limit)
		return api_error(connection, 413, "request_limit",
			"The request exceeds the upload limit.");
	if (!read_body(connection, limit, &body))
		return api_error(connection, 400, "request_body",
			"Unable to read the complete request body.");
	yyjson_doc *request = kind == job_import ? nullptr : read_json(body);
	yyjson_val *root = request ? yyjson_doc_get_root(request) : nullptr;
	yyjson_val *revision = root ? yyjson_obj_get(root, "revision") :
		nullptr;
	bool valid = kind == job_import ||
		(yyjson_is_obj(root) && yyjson_is_uint(revision));

	if (!valid) {
		secret_free(&body);
		yyjson_doc_free(request);
		return api_error(connection, 400, "json",
			"Expected a JSON object with a revision.");
	}
	web_job *job = static_cast<web_job *>(calloc(1, sizeof(*job)));

	if (!job) {
		secret_free(&body);
		yyjson_doc_free(request);
		return api_error(connection, 503, "resources",
			"Insufficient resources.");
	}
	job->kind = kind;
	job->output = output;
	job->body = body;
	job->created_ms = tg_monotonic_ms();
	if (!id && kind != job_import && kind != job_confirm &&
		strcmp(info->request_method, "POST")) {
		id = yyjson_get_str(yyjson_obj_get(root, "id"));
		if (!id && kind == job_edit) {
			yyjson_val *record = yyjson_obj_get(root,
				output ? "output" : "input");

			id = yyjson_get_str(yyjson_obj_get(record, "id"));
		}
		if (!valid_id(id)) {
			secret_free(&job->body);
			free(job);
			yyjson_doc_free(request);
			return api_error(connection, 400, "identifier",
				"An existing endpoint identifier is required.");
		}
	}
	job->endpoint = tg_strdup(id ? id : "");
	if (!job->endpoint) {
		secret_free(&job->body);
		free(job);
		yyjson_doc_free(request);
		return api_error(connection, 503, "resources",
			"Insufficient resources.");
	}
	mutex_lock(&app->mutex);
	web_job *parent = parent_id ? find_job(app, parent_id) : nullptr;

	job->parent = parent;
	job->expected = kind == job_import ? app->published_revision :
		yyjson_get_uint(revision);
	bool conflict = job->expected != app->published_revision;
	bool room = app->job_count < 64 && !app->stopping &&
		!app->edits_blocked &&
		app->job_bytes + body.size + app->source.size <=
			64 * 1024 * 1024;
	bool copied = !conflict && room &&
		tg_copy({app->source.data, app->source.size}, &job->source) ==
			TG_OK;

	if (copied && parent_id && (!parent ||
		parent->state != job_ready || parent->confirming))
		copied = false;
	if (copied) {
		if (parent)
			parent->confirming = true;
		job->id = ++app->sequence;
		web_job **tail = &app->jobs;

		while (*tail)
			tail = &(*tail)->next;
		*tail = job;
		++app->job_count;
		app->job_bytes += job->body.size + job->source.size;
	}
	mutex_unlock(&app->mutex);
	yyjson_doc_free(request);
	if (!copied) {
		secret_free(&job->body);
		secret_free(&job->source);
		free(job->endpoint);
		free(job);
		return api_error(connection, conflict ? 409 : 503,
			conflict ? "revision_conflict" : "busy",
			conflict ? "Configuration changed; refresh and retry." :
				"The management queue is busy.");
	}
	char result[80];
	int n = snprintf(result, sizeof(result), "{\"job_id\":%llu}",
		(unsigned long long)job->id);

	return response(connection, 202, "application/json",
		{(const uint8_t *)result, (size_t)n});
}

static int cancel_preview(tg_web_app *app, mg_connection *connection,
	uint64_t id)
{
	mutex_lock(&app->mutex);
	web_job *job = find_job(app, id);
	bool cancel = job && job->kind == job_import &&
		job->state == job_ready && !job->confirming;
	tg_config *candidate = nullptr;
	tg_import_files *imports = nullptr;
	tg_buffer preview{};

	if (cancel) {
		job->state = job_failed;
		job->error = TG_BAD_STATE;
		snprintf(job->diagnostic.key, sizeof(job->diagnostic.key),
			"web.preview_cancelled");
		candidate = job->candidate;
		job->candidate = nullptr;
		imports = job->imports;
		job->imports = nullptr;
		preview = job->response;
		job->response = {};
	}
	mutex_unlock(&app->mutex);
	tg_config_destroy(candidate);
	tg_import_zip_destroy(imports);
	tg_buffer_release(&preview);
	if (!cancel)
		return api_error(connection, 409, "preview_state",
			"The preview cannot be canceled in its current state.");
	const char result[] = "{\"state\":\"canceled\"}";

	return response(connection, 200, "application/json",
		{(const uint8_t *)result, sizeof(result) - 1});
}

static int handle_request(mg_connection *connection, void *user)
{
	tg_web_app *app = static_cast<tg_web_app *>(user);
	const mg_request_info *info = mg_get_request_info(connection);
	const char *path = info->local_uri ? info->local_uri : "";
	const char *method = info->request_method;

	if (!valid_host(app, connection))
		return api_error(connection, 403, "host",
			"Use the configured numeric address or localhost.");

	if (!strcmp(method, "GET") && !strcmp(path, "/api/state")) {
		tg_buffer copy{};

		mutex_lock(&app->mutex);
		tg_status status = tg_copy({app->snapshot.data,
			app->snapshot.size}, &copy);
		mutex_unlock(&app->mutex);
		if (status < 0)
			return api_error(connection, 503, "resources",
				"Insufficient resources.");
		int result = response(connection, 200, "application/json",
			{copy.data, copy.size});

		tg_buffer_release(&copy);
		return result;
	}
	if (!strcmp(method, "GET") && !strncmp(path, "/api/jobs/", 10)) {
		const char *id = path + 10;
		char *end = nullptr;
		unsigned long long number = strtoull(id, &end, 10);

		if (*id && !*end && number)
			return job_response(app, connection, (uint64_t)number);
		return api_error(connection, 404, "not_found",
			"Job not found.");
	}
	if (!strncmp(path, "/api/", 5)) {
		if (!authorized(app, connection))
			return api_error(connection, 403, "origin",
				"A same-origin management token is required.");
		if (!strcmp(method, "DELETE") &&
			!strncmp(path, "/api/jobs/", 10)) {
			char *end = nullptr;
			uint64_t id = (uint64_t)strtoull(path + 10, &end, 10);

			if (id && !*end)
				return cancel_preview(app, connection, id);
		}
		if (!strcmp(method, "POST") && !strcmp(path, "/api/assets"))
			return upload_asset(app, connection);
		if (!strcmp(method, "POST") && !strcmp(path, "/api/import"))
			return enqueue(app, connection, job_import, false,
				nullptr, 0);
		if (!strcmp(method, "POST") &&
			!strncmp(path, "/api/import/", 12)) {
			const char *text = path + 12;
			char *end = nullptr;
			uint64_t id = (uint64_t)strtoull(text, &end, 10);

			mutex_lock(&app->mutex);
			web_job *parent = find_job(app, id);
			bool valid = parent && !strcmp(end, "/confirm") &&
				parent->kind == job_import;
			mutex_unlock(&app->mutex);
			if (valid)
				return enqueue(app, connection, job_confirm,
					false, nullptr, id);
		}
		const char *collections[] = {"/api/inputs", "/api/outputs"};

		for (size_t i = 0; i < 2; ++i) {
			size_t n = strlen(collections[i]);

			if (strncmp(path, collections[i], n))
				continue;
			const char *id = path[n] == '/' ? path + n + 1 :
				nullptr;

			if (!path[n] && (!strcmp(method, "POST") ||
				!strcmp(method, "PUT") || !strcmp(method,
					"DELETE")))
				return enqueue(app, connection,
					!strcmp(method, "DELETE") ?
						job_remove : job_edit,
					i == 1, nullptr, 0);
			if (id && path_id(id) &&
				(!strcmp(method, "PUT") || !strcmp(method,
					"DELETE")))
				return enqueue(app, connection,
					!strcmp(method, "DELETE") ?
						job_remove : job_edit,
					i == 1, id, 0);
		}
		return api_error(connection, 404, "not_found",
			"Endpoint not found.");
	}
	if (!strcmp(method, "GET")) {
		tg_bytes bytes{};
		const char *mime = nullptr;

		if (tg_web_asset(path, &bytes, &mime))
			return response(connection, 200, mime, bytes);
	}
	return api_error(connection, 404, "not_found", "Page not found.");
}

static void created_clear(tg_web_app *app, web_job *job, bool remove)
{
	while (job->created) {
		created_asset *asset = job->created;

		job->created = asset->next;
		if (remove && asset->path.data)
			remove_asset(app, (const char *)asset->path.data);
		tg_buffer_release(&asset->path);
		free(asset);
	}
}

static void job_free(tg_web_app *app, web_job *job)
{
	created_clear(app, job, job->state != job_committed);
	secret_free(&job->body);
	secret_free(&job->source);
	tg_buffer_release(&job->response);
	tg_config_destroy(job->candidate);
	tg_import_zip_destroy(job->imports);
	free(job->endpoint);
	free(job);
}

static void collect_jobs(tg_web_app *app)
{
	uint64_t now = tg_monotonic_ms();

	for (;;) {
		mutex_lock(&app->mutex);
		web_job **slot = &app->jobs;
		web_job *removed = nullptr;

		while (*slot) {
			web_job *job = *slot;
			bool expired = now - job->created_ms >= 600000;

			if (expired && job->kind == job_import &&
				job->state == job_ready && !job->confirming) {
				job->state = job_failed;
				job->error = TG_BAD_STATE;
				snprintf(job->diagnostic.key,
					sizeof(job->diagnostic.key),
					"web.preview_expired");
			}
			bool terminal = job->state == job_failed ||
				job->state == job_committed;

			if (terminal && !job->confirming &&
				(expired || app->job_count > 32)) {
				*slot = job->next;
				--app->job_count;
				removed = job;
				break;
			}
			slot = &job->next;
		}
		bool working = false;

		for (web_job *job = app->jobs; job; job = job->next)
			if (job->state == job_pending || job->state ==
				job_working)
				working = true;
		web_asset *discarded = nullptr;

		if (!working) {
			web_asset **asset = &app->assets;

			while (*asset) {
				web_asset *value = *asset;

				if (value->used || now - value->created_ms >=
					600000) {
					*asset = value->next;
					app->asset_bytes -= value->bytes.size;
					value->next = discarded;
					discarded = value;
				} else {
					asset = &value->next;
				}
			}
		}
		mutex_unlock(&app->mutex);
		while (discarded) {
			web_asset *asset = discarded;

			discarded = asset->next;
			secret_free(&asset->bytes);
			free(asset);
		}
		if (!removed)
			break;
		job_free(app, removed);
	}
	/* Release failed jobs' staged assets before history expires. */
	mutex_lock(&app->mutex);
	for (web_job *job = app->jobs; job; job = job->next) {
		bool failed = job->state == job_failed;
		if (failed)
			created_clear(app, job, true);
	}
	mutex_unlock(&app->mutex);
}

static bool config_references(const tg_config *config, const char *path)
{
	for (size_t i = 0; i < config->input_count; ++i) {
		const tg_input_config *input = &config->inputs[i];
		const char *paths[] = {input->ca_file, input->cert_file,
			input->key_file, input->pkcs12_file,
			input->key_password_file, input->pkcs12_password_file,
			input->password_file, input->identity_file};

		for (const char *reference : paths)
			if (reference && !strcmp(reference, path))
				return true;
	}
	return false;
}

static void apply_prepared(tg_web_app *app, web_job *job)
{
	uint64_t committed_revision = 0, loaded_revision = 0;
	tg_import_files *imports = job->parent ? job->parent->imports : nullptr;

	mutex_lock(&app->store_mutex);
	tg_status saved = tg_store_save_import(app->store, job->candidate,
		&app->web, job->expected, imports, &committed_revision,
		&job->diagnostic);
	tg_diagnostic save_diagnostic = job->diagnostic;
	tg_buffer source{};
	tg_web_settings web{};
	tg_config *loaded = nullptr;
	tg_status status = saved;

	if (committed_revision)
		status = tg_store_load(app->store, &source, &web,
			&loaded_revision, &job->diagnostic);
	mutex_unlock(&app->store_mutex);
	if (status == TG_OK && committed_revision &&
		loaded_revision != committed_revision)
		status = web_error(&job->diagnostic, TG_BAD_STATE,
			"saved_revision");
	if (status == TG_OK && committed_revision)
		status = tg_config_parse_managed({source.data, source.size},
			tg_store_path(app->store), &loaded, &job->diagnostic);
	bool loaded_config = loaded != nullptr;

	if (loaded_config) {
		status = tg_runtime_apply_config(app->runtime, loaded,
			&job->diagnostic);
		app->apply_error = status;
		app->apply_diagnostic = job->diagnostic;
		if (status == TG_OK)
			app->active_revision = committed_revision;
		tg_config_destroy(app->config);
		app->config = loaded;
		loaded = nullptr;
	} else if (committed_revision) {
		app->apply_error = status;
		app->apply_diagnostic = job->diagnostic;
	}
	if (committed_revision) {
		mutex_lock(&app->mutex);
		app->revision = committed_revision;
		app->edits_blocked = !loaded_config;
		mutex_unlock(&app->mutex);
	}
	if (status == TG_OK && saved < 0) {
		status = saved;
		job->diagnostic = save_diagnostic;
	}
	job->error = status;
	tg_config_destroy(loaded);
	secret_free(&source);
	if (loaded_config) {
		for (created_asset *asset = job->created; asset;
			asset = asset->next)
			if (!config_references(app->config,
				(const char *)asset->path.data))
				remove_asset(app,
					(const char *)asset->path.data);
	}
	/* Published assets survive reload and durability-check failures. */
	created_clear(app, job, !committed_revision);
	if (committed_revision && !refresh_snapshot(app) && status == TG_OK)
		job->error = TG_NO_MEMORY;
	mutex_lock(&app->mutex);
	job->state = job->error < 0 ? job_failed : job_committed;
	if (job->parent) {
		job->parent->confirming = false;
		if (committed_revision)
			job->parent->state = job_committed;
		job->parent = nullptr;
	}
	mutex_unlock(&app->mutex);
}

tg_status tg_web_app_create(tg_store *store, const tg_config *initial,
	const tg_web_settings *web, uint64_t revision, tg_web_app **out,
	tg_diagnostic *diagnostic)
{
	if (diagnostic)
		*diagnostic = {};
	if (!store || !initial || !web || !out || *out)
		return web_error(diagnostic, TG_INVALID, "arguments");
	tg_web_app *app = static_cast<tg_web_app *>(calloc(1, sizeof(*app)));

	if (!app)
		return web_error(diagnostic, TG_NO_MEMORY, "resources");
	mutex_init(&app->mutex);
	mutex_init(&app->store_mutex);
	app->store = store;
	app->web = *web;
	app->revision = revision;
	app->active_revision = revision;
	tg_buffer source{};
	tg_status status = random_id(app->token) ? TG_OK : TG_NO_MEMORY;

	if (status == TG_OK)
		status = tg_config_export(initial, &source, diagnostic);
	if (status == TG_OK)
		status = tg_config_parse_managed({source.data, source.size},
			tg_store_path(store), &app->config, diagnostic);
	tg_buffer_release(&source);
	if (status == TG_OK)
		status = tg_runtime_create_managed(app->config, &app->runtime,
			diagnostic);
	if (status == TG_OK && !refresh_snapshot(app))
		status = TG_NO_MEMORY;
	if (status == TG_OK) {
#ifdef _WIN32
		app->worker = CreateThread(nullptr, 0, worker_main, app, 0,
			nullptr);
		app->worker_started = app->worker != nullptr;
#else
		app->worker_started = pthread_create(&app->worker, nullptr,
			worker_main, app) == 0;
#endif
		if (!app->worker_started)
			status = TG_IO;
	}
	if (status == TG_OK && web->enabled) {
		const char *options[] = {"listening_ports", web->bind,
			"num_threads", "4", "request_timeout_ms", "5000",
			"enable_keep_alive", "no", nullptr};
		mg_callbacks callbacks{};

		app->http = mg_start(&callbacks, app, options);
		if (!app->http)
			status = web_error(diagnostic, TG_IO, "bind");
		else
			mg_set_request_handler(app->http, "/", handle_request,
				app);
	}
	if (status < 0) {
		tg_web_app_destroy(app);
		return diagnostic && diagnostic->status ? status :
			web_error(diagnostic, status, "create");
	}
	*out = app;
	return TG_OK;
}

tg_status tg_web_app_poll(tg_web_app *app, uint32_t max_wait_ms)
{
	if (!app)
		return TG_INVALID;
	for (;;) {
		mutex_lock(&app->mutex);
		web_job *prepared = nullptr;

		for (web_job *job = app->jobs; job; job = job->next)
			if (job->state == job_prepared) {
				prepared = job;
				job->state = job_working;
				break;
			}
		mutex_unlock(&app->mutex);
		if (!prepared)
			break;
		apply_prepared(app, prepared);
	}
	collect_jobs(app);
	tg_status status = tg_runtime_poll(app->runtime, max_wait_ms);

	if (!refresh_snapshot(app))
		return TG_NO_MEMORY;
	/* A failed gateway remains inspectable and editable through HTTP. */
	return status == TG_NO_MEMORY ? status : TG_OK;
}

tg_status tg_web_app_stop(tg_web_app *app, uint32_t drain_ms)
{
	if (!app)
		return TG_INVALID;
	mutex_lock(&app->mutex);
	app->stopping = true;
	mutex_unlock(&app->mutex);
	return tg_runtime_stop(app->runtime, drain_ms);
}

void tg_web_app_destroy(tg_web_app *app)
{
	if (!app)
		return;
	if (app->http)
		mg_stop(app->http);
	mutex_lock(&app->mutex);
	app->worker_stop = true;
	mutex_unlock(&app->mutex);
	if (app->worker_started) {
#ifdef _WIN32
		WaitForSingleObject(app->worker, INFINITE);
		CloseHandle(app->worker);
#else
		pthread_join(app->worker, nullptr);
#endif
	}
	while (app->jobs) {
		web_job *job = app->jobs;

		app->jobs = job->next;
		job_free(app, job);
	}
	while (app->assets) {
		web_asset *asset = app->assets;

		app->assets = asset->next;
		secret_free(&asset->bytes);
		free(asset);
	}
	tg_runtime_destroy(app->runtime);
	tg_config_destroy(app->config);
	tg_buffer_release(&app->snapshot);
	secret_free(&app->source);
	mutex_clear(&app->mutex);
	mutex_clear(&app->store_mutex);
	free(app);
}
