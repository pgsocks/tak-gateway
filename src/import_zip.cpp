#include "import_zip.h"
#include "config_internal.h"
#include <miniz.h>
#include <expat.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static const size_t archive_limit = 16 * 1024 * 1024;
static const size_t xml_limit = 1024 * 1024;

struct zip_entry {
	char *name;
	tg_buffer bytes;
	bool directory;
};

struct archive {
	zip_entry *entries;
	size_t count;
};

struct temporary_file {
	char *path;
	temporary_file *next;
};

struct tg_import_files {
	char *directory;
	temporary_file *files;
	bool password_auth;
	bool enrollment;
};

struct preference {
	char *key;
	char *value;
	bool stream;
};

struct xml_reader {
	XML_Parser parser;
	tg_status status;
	unsigned depth;
	bool manifest;
	bool stream;
	bool global;
	bool capture;
	bool root;
	tg_vec text;
	char *key;
	preference entries[1024];
	size_t count;
	char *references[256];
	size_t reference_count;
};

struct material {
	EVP_PKEY *key;
	X509 *certificates[64];
	size_t count;
};

static tg_status failure(tg_diagnostic *d, tg_status s, const char *key)
{
	if (d) {
		*d = {};
		d->status = s;
		snprintf(d->key, sizeof(d->key), "import.%s", key);
	}
	return s;
}

static void clear_buffer(tg_buffer *b)
{
	if (b->data)
		OPENSSL_cleanse(b->data, b->size);
	tg_buffer_release(b);
}

static void clear_text(char *s)
{
	if (s)
		OPENSSL_cleanse(s, strlen(s));
	free(s);
}

static bool suffix(const char *s, const char *ending)
{
	size_t n = strlen(s), m = strlen(ending);

	return n >= m && !strcmp(s + n - m, ending);
}

static bool safe_path(const char *path)
{
	if (!path || !*path || *path == '/' || strchr(path, '\\') ||
		strchr(path, ':') || strlen(path) > 511 ||
		!tg_utf8({(const uint8_t *)path, strlen(path)}))
		return false;
	for (const char *part = path; *part;) {
		const char *end = strchr(part, '/');
		size_t n = end ? (size_t)(end - part) : strlen(part);

		if (!n || (n == 1 && *part == '.') ||
			(n == 2 && part[0] == '.' && part[1] == '.'))
			return false;
		for (size_t i = 0; i < n; ++i)
			if ((unsigned char)part[i] < 32 || part[i] == 127)
				return false;
		part += n;
		if (*part)
			++part;
	}
	return true;
}

static void archive_clear(archive *a)
{
	for (size_t i = 0; i < a->count; ++i) {
		free(a->entries[i].name);
		clear_buffer(&a->entries[i].bytes);
	}
	free(a->entries);
	*a = {};
}

static tg_status archive_read(tg_bytes bytes, archive *a)
{
	if (!bytes.size || bytes.size > archive_limit)
		return TG_LIMIT;
	mz_zip_archive zip{};

	if (!mz_zip_reader_init_mem(&zip, bytes.data, bytes.size, 0))
		return TG_MALFORMED;
	size_t count = mz_zip_reader_get_num_files(&zip);
	tg_status status = TG_OK;
	size_t total = 0;

	if (!count || count > 256)
		status = TG_LIMIT;
	if (status == TG_OK) {
		a->entries = static_cast<zip_entry *>(
			calloc(count, sizeof(*a->entries)));
		if (!a->entries)
			status = TG_NO_MEMORY;
		else
			a->count = count;
	}
	for (size_t i = 0; status == TG_OK && i < count; ++i) {
		mz_zip_archive_file_stat st{};
		zip_entry *entry = &a->entries[i];

		if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &st) ||
			mz_zip_reader_get_filename(&zip, (mz_uint)i,
				nullptr, 0) > sizeof(st.m_filename) ||
			mz_zip_reader_get_filename(&zip, (mz_uint)i,
				nullptr, 0) != strlen(st.m_filename) + 1 ||
			!safe_path(st.m_filename) || st.m_is_encrypted ||
			!st.m_is_supported ||
			((st.m_external_attr >> 16) & 0170000) == 0120000) {
			status = TG_MALFORMED;
			break;
		}
		for (size_t j = 0; j < i; ++j)
			if (!strcmp(st.m_filename, a->entries[j].name))
				status = TG_MALFORMED;
		if (status != TG_OK)
			break;
		entry->name = tg_strdup(st.m_filename);
		entry->directory = st.m_is_directory != 0;
		if (!entry->name) {
			status = TG_NO_MEMORY;
			break;
		}
		if (entry->directory)
			continue;
		if (st.m_uncomp_size > archive_limit - total) {
			status = TG_LIMIT;
			break;
		}
		total += (size_t)st.m_uncomp_size;
		size_t size = 0;
		void *data = mz_zip_reader_extract_to_heap(
			&zip, (mz_uint)i, &size, 0);

		if (!data || size != st.m_uncomp_size) {
			mz_free(data);
			status = TG_MALFORMED;
			break;
		}
		status = tg_copy({(const uint8_t *)data, size}, &entry->bytes);
		OPENSSL_cleanse(data, size);
		mz_free(data);
	}
	mz_zip_reader_end(&zip);
	return status;
}

static zip_entry *find_entry(archive *a, const char *name)
{
	for (size_t i = 0; i < a->count; ++i)
		if (!a->entries[i].directory &&
			!strcmp(a->entries[i].name, name))
			return &a->entries[i];
	return nullptr;
}

static zip_entry *find_manifest(archive *a, size_t *count)
{
	zip_entry *result = nullptr;
	const char *name = "MANIFEST/manifest.xml";

	*count = 0;
	for (size_t i = 0; i < a->count; ++i) {
		zip_entry *entry = &a->entries[i];
		size_t n = strlen(entry->name), m = strlen(name);

		if (!entry->directory && suffix(entry->name, name) &&
			(n == m || entry->name[n - m - 1] == '/')) {
			result = entry;
			++*count;
		}
	}
	return result;
}

static const char *attribute(const char **attributes, const char *name)
{
	for (size_t i = 0; attributes[i]; i += 2)
		if (!strcmp(attributes[i], name))
			return attributes[i + 1];
	return nullptr;
}

static void xml_fail(xml_reader *r, tg_status status = TG_MALFORMED)
{
	r->status = status;
	XML_StopParser(r->parser, XML_FALSE);
}

static void XMLCALL xml_start(void *user, const char *name,
	const char **attributes)
{
	xml_reader *r = static_cast<xml_reader *>(user);

	if (++r->depth > 32) {
		xml_fail(r, TG_LIMIT);
		return;
	}
	if (r->depth == 1) {
		r->root = !strcmp(name, r->manifest ?
			"MissionPackageManifest" : "preferences");
		if (!r->root)
			xml_fail(r);
	}
	if (r->manifest && !strcmp(name, "Content")) {
		const char *path = attribute(attributes, "zipEntry");
		const char *ignore = attribute(attributes, "ignore");

		if (!path || !safe_path(path)) {
			xml_fail(r);
			return;
		}
		if (ignore && !strcmp(ignore, "true"))
			return;
		if (r->reference_count == 256) {
			xml_fail(r, TG_LIMIT);
			return;
		}
		char *copy = tg_strdup(path);

		if (!copy)
			xml_fail(r, TG_NO_MEMORY);
		else
			r->references[r->reference_count++] = copy;
	}
	if (r->manifest)
		return;
	if (r->depth == 2 && !strcmp(name, "preference")) {
		const char *scope = attribute(attributes, "name");

		r->stream = scope && !strcmp(scope, "cot_streams");
		r->global = scope &&
			(!strcmp(scope, "com.atakmap.app_preferences") ||
			 !strcmp(scope, "com.atakmap.civ_preferences") ||
			 !strcmp(scope, "com.atakmap.fvey_preferences"));
	}
	if (r->depth == 3 && !strcmp(name, "entry") &&
		(r->stream || r->global)) {
		const char *key = attribute(attributes, "key");

		if (!key || strlen(key) > 127 || r->count == 1024) {
			xml_fail(r, TG_LIMIT);
			return;
		}
		for (size_t i = 0; i < r->count; ++i)
			if (r->entries[i].stream == r->stream &&
				!strcmp(r->entries[i].key, key)) {
				xml_fail(r);
				return;
			}
		r->key = tg_strdup(key);
		r->capture = r->key != nullptr;
		if (!r->key)
			xml_fail(r, TG_NO_MEMORY);
	} else if (r->capture) {
		xml_fail(r);
	}
}

static void XMLCALL xml_end(void *user, const char *)
{
	xml_reader *r = static_cast<xml_reader *>(user);

	if (r->capture && r->depth == 3) {
		if (!r->text.data)
			r->text.data = (uint8_t *)tg_strdup("");
		if (!r->text.data) {
			xml_fail(r, TG_NO_MEMORY);
			return;
		}
		r->entries[r->count++] = {r->key,
			(char *)r->text.data, r->stream};
		r->key = nullptr;
		r->text = {};
		r->capture = false;
	}
	if (r->depth == 2) {
		r->stream = false;
		r->global = false;
	}
	--r->depth;
}

static void XMLCALL xml_text(void *user, const char *text, int length)
{
	xml_reader *r = static_cast<xml_reader *>(user);

	if (!r->capture)
		return;
	if (r->text.size + (size_t)length > 65536)
		xml_fail(r, TG_LIMIT);
	else if (!tg_append(&r->text, text, (size_t)length))
		xml_fail(r, TG_NO_MEMORY);
}

static void XMLCALL xml_doctype(void *user, const char *, const char *,
	const char *, int)
{
	xml_fail(static_cast<xml_reader *>(user));
}

static int XMLCALL xml_external(XML_Parser, const char *, const char *,
	const char *, const char *)
{
	return XML_STATUS_ERROR;
}

static tg_status xml_read(tg_buffer bytes, bool manifest, xml_reader *r)
{
	if (!bytes.size || bytes.size > xml_limit)
		return TG_LIMIT;
	r->manifest = manifest;
	r->parser = XML_ParserCreate("UTF-8");
	if (!r->parser)
		return TG_NO_MEMORY;
	XML_SetUserData(r->parser, r);
	XML_SetElementHandler(r->parser, xml_start, xml_end);
	XML_SetCharacterDataHandler(r->parser, xml_text);
	XML_SetStartDoctypeDeclHandler(r->parser, xml_doctype);
	XML_SetExternalEntityRefHandler(r->parser, xml_external);
	XML_SetParamEntityParsing(r->parser, XML_PARAM_ENTITY_PARSING_NEVER);
	int parsed = XML_Parse(r->parser, (const char *)bytes.data,
		(int)bytes.size, XML_TRUE);

	XML_ParserFree(r->parser);
	r->parser = nullptr;
	return r->status < 0 ? r->status :
		parsed == XML_STATUS_OK && r->root ? TG_OK : TG_MALFORMED;
}

static void xml_clear(xml_reader *r)
{
	clear_text(r->key);
	if (r->text.data)
		OPENSSL_cleanse(r->text.data, r->text.size);
	tg_vec_free(&r->text);
	for (size_t i = 0; i < r->count; ++i) {
		free(r->entries[i].key);
		clear_text(r->entries[i].value);
	}
	for (size_t i = 0; i < r->reference_count; ++i)
		free(r->references[i]);
}

static const char *value(xml_reader *r, const char *key, bool stream)
{
	for (size_t i = 0; i < r->count; ++i)
		if (r->entries[i].stream == stream &&
			!strcmp(r->entries[i].key, key))
			return r->entries[i].value;
	return nullptr;
}

static const char *setting(xml_reader *r, const char *key, unsigned index,
	bool global = true)
{
	char indexed[160];

	snprintf(indexed, sizeof(indexed), "%s%u", key, index);
	const char *result = value(r, indexed, true);

	return result || !global ? result : value(r, key, false);
}

static bool requested(const char *text)
{
	return text && !strcmp(text, "true");
}

static void collect_requirements(xml_reader *preferences, unsigned index,
	tg_import_files *files)
{
	files->password_auth = files->password_auth || requested(
		setting(preferences, "useAuth", index, false));
	char global[160];

	snprintf(global, sizeof(global), "enrollForCertificateWithTrust%u",
		index);
	files->enrollment = files->enrollment || requested(setting(preferences,
		"enrollForCertificateWithTrust", index, false)) ||
		requested(value(preferences, global, false)) ||
		requested(value(preferences, "enrollForCertificateWithTrust",
			false));
}

static bool integer(const char *s, unsigned maximum, unsigned *out)
{
	if (!s || !*s)
		return false;
	unsigned n = 0;

	for (; *s; ++s) {
		if (*s < '0' || *s > '9' || n > maximum / 10)
			return false;
		n = n * 10 + (unsigned)(*s - '0');
		if (n > maximum)
			return false;
	}
	*out = n;
	return true;
}

static zip_entry *resolve(archive *package, archive *outer,
	const char *base, const char *path)
{
	if (!safe_path(path))
		return nullptr;
	tg_vec full{};
	zip_entry *result = nullptr;

	if (tg_printf(&full, "%s%s", base, path))
		result = find_entry(package, (const char *)full.data);
	tg_vec_free(&full);
	if (result)
		return result;
	const char *basename = strrchr(path, '/');

	basename = basename ? basename + 1 : path;
	archive *archives[] = {package, outer};

	for (archive *a : archives) {
		if (!a)
			continue;
		size_t matches = 0;

		for (size_t i = 0; i < a->count; ++i) {
			zip_entry *entry = &a->entries[i];
			const char *name = strrchr(entry->name, '/');

			name = name ? name + 1 : entry->name;
			if (!entry->directory && !strcmp(name, basename)) {
				result = entry;
				++matches;
			}
		}
		if (matches)
			return matches == 1 ? result : nullptr;
	}
	return nullptr;
}

static void material_clear(material *m)
{
	EVP_PKEY_free(m->key);
	for (size_t i = 0; i < m->count; ++i)
		X509_free(m->certificates[i]);
	*m = {};
}

static bool valid_iterations(const ASN1_INTEGER *iterations)
{
	int64_t count = 1;

	return (!iterations || ASN1_INTEGER_get_int64(&count, iterations)) &&
		count > 0 && count <= 1000000;
}

static bool valid_encryption(const X509_ALGOR *algorithm)
{
	const ASN1_OBJECT *object = nullptr;
	const void *parameter = nullptr;
	int type = 0;

	X509_ALGOR_get0(&object, &type, &parameter, algorithm);
	if (type != V_ASN1_SEQUENCE || !parameter)
		return false;
	const ASN1_STRING *encoded =
		static_cast<const ASN1_STRING *>(parameter);
	const unsigned char *p = ASN1_STRING_get0_data(encoded);
	long length = ASN1_STRING_length(encoded);

	if (OBJ_obj2nid(object) != NID_pbes2) {
		PBEPARAM *parameters = d2i_PBEPARAM(nullptr, &p, length);
		bool ok = parameters && valid_iterations(parameters->iter);

		PBEPARAM_free(parameters);
		return ok;
	}
	PBE2PARAM *parameters = d2i_PBE2PARAM(nullptr, &p, length);
	bool ok = parameters != nullptr;

	if (ok) {
		X509_ALGOR_get0(&object, &type, &parameter,
			parameters->keyfunc);
		ok = OBJ_obj2nid(object) == NID_id_pbkdf2 &&
			type == V_ASN1_SEQUENCE && parameter;
	}
	PBKDF2PARAM *kdf = nullptr;

	if (ok) {
		encoded = static_cast<const ASN1_STRING *>(parameter);
		p = ASN1_STRING_get0_data(encoded);
		kdf = d2i_PBKDF2PARAM(nullptr, &p, ASN1_STRING_length(encoded));
		ok = kdf && valid_iterations(kdf->iter);
	}
	PBKDF2PARAM_free(kdf);
	PBE2PARAM_free(parameters);
	return ok;
}

static bool read_bags(const STACK_OF(PKCS12_SAFEBAG) *bags,
	const char *password, int password_length, material *m, unsigned depth)
{
	if (!bags || depth > 4 || sk_PKCS12_SAFEBAG_num(bags) > 128)
		return false;
	for (int i = 0; i < sk_PKCS12_SAFEBAG_num(bags); ++i) {
		const PKCS12_SAFEBAG *bag = sk_PKCS12_SAFEBAG_value(bags, i);
		int type = PKCS12_SAFEBAG_get_nid(bag);

		if (type == NID_certBag) {
			if (m->count == 64)
				return false;
			X509 *cert = PKCS12_SAFEBAG_get1_cert(bag);

			if (!cert)
				return false;
			m->certificates[m->count++] = cert;
		} else if (type == NID_keyBag ||
			   type == NID_pkcs8ShroudedKeyBag) {
			if (m->key)
				return false;
			PKCS8_PRIV_KEY_INFO *decrypted = nullptr;
			const PKCS8_PRIV_KEY_INFO *info = nullptr;

			if (type == NID_keyBag)
				info = PKCS12_SAFEBAG_get0_p8inf(bag);
			else {
				const X509_ALGOR *algorithm = nullptr;
				const ASN1_OCTET_STRING *encrypted = nullptr;

				X509_SIG_get0(PKCS12_SAFEBAG_get0_pkcs8(bag),
					&algorithm, &encrypted);
				if (!valid_encryption(algorithm))
					return false;
				decrypted = PKCS12_decrypt_skey(bag,
					password, password_length);
				info = decrypted;
			}
			m->key = info ? EVP_PKCS82PKEY(info) : nullptr;
			PKCS8_PRIV_KEY_INFO_free(decrypted);
			if (!m->key)
				return false;
		} else if (type == NID_safeContentsBag &&
			!read_bags(PKCS12_SAFEBAG_get0_safes(bag),
				password, password_length, m, depth + 1)) {
			return false;
		}
	}
	return true;
}

static bool material_read(zip_entry *entry, const char *password,
	material *m)
{
	if (!entry)
		return false;
	password = password ? password : "";
	int password_length = -1;
	const unsigned char *p = entry->bytes.data;
	PKCS12 *bundle = d2i_PKCS12(nullptr, &p, (long)entry->bytes.size);
	bool ok = bundle && p == entry->bytes.data + entry->bytes.size &&
		PKCS12_mac_present(bundle);
	const ASN1_INTEGER *iterations = nullptr;
	const X509_ALGOR *mac_algorithm = nullptr;

	if (ok) {
		PKCS12_get0_mac(nullptr, &mac_algorithm, nullptr,
			&iterations, bundle);
		const ASN1_OBJECT *object = nullptr;

		X509_ALGOR_get0(&object, nullptr, nullptr, mac_algorithm);
		ok = OBJ_obj2nid(object) != NID_pbmac1 &&
			valid_iterations(iterations);
	}
	if (ok && !*password) {
		password_length = 0;
		if (PKCS12_verify_mac(bundle, nullptr, 0) == 1)
			password = nullptr;
		else {
			ERR_clear_error();
			ok = PKCS12_verify_mac(bundle, "", 0) == 1;
		}
	} else if (ok) {
		ok = PKCS12_verify_mac(bundle, password, password_length) == 1;
	}
	STACK_OF(PKCS7) *safes = ok ? PKCS12_unpack_authsafes(bundle) : nullptr;

	ok = ok && safes && sk_PKCS7_num(safes) <= 128;
	for (int i = 0; ok && i < sk_PKCS7_num(safes); ++i) {
		PKCS7 *safe = sk_PKCS7_value(safes, i);
		STACK_OF(PKCS12_SAFEBAG) *bags = nullptr;

		if (PKCS7_type_is_data(safe))
			bags = PKCS12_unpack_p7data(safe);
		else if (PKCS7_type_is_encrypted(safe) && valid_encryption(
			safe->d.encrypted->enc_data->algorithm))
			bags = PKCS12_unpack_p7encdata(safe, password,
				password_length);
		ok = bags && read_bags(bags, password, password_length, m, 0);
		sk_PKCS12_SAFEBAG_pop_free(bags, PKCS12_SAFEBAG_free);
	}
	sk_PKCS7_pop_free(safes, PKCS7_free);
	PKCS12_free(bundle);
	ERR_clear_error();
	if (!ok || !m->count) {
		material_clear(m);
		return false;
	}
	return true;
}

static int matching_certificate(material *m)
{
	if (!m->key)
		return -1;
	for (size_t i = 0; i < m->count; ++i)
		if (X509_check_private_key(m->certificates[i], m->key) == 1) {
			ERR_clear_error();
			return (int)i;
		}
	ERR_clear_error();
	return -1;
}

tg_status tg_import_pkcs12_identity(tg_bytes bundle, const char *password,
	tg_buffer *certificates, tg_buffer *key, tg_diagnostic *diagnostic)
{
	if (diagnostic)
		*diagnostic = {};
	if (!tg_span(bundle) || !bundle.size ||
		!tg_empty(certificates) || !tg_empty(key) ||
		certificates == key)
		return failure(diagnostic, TG_INVALID, "pkcs12_identity");
	if (bundle.size > 8 * 1024 * 1024 ||
		(password && strlen(password) > 65536))
		return failure(diagnostic, TG_LIMIT, "pkcs12_identity");
	OSSL_LIB_CTX *context = OSSL_LIB_CTX_new();
	OSSL_PROVIDER *defaults = context ?
		OSSL_PROVIDER_load(context, "default") : nullptr;
	OSSL_PROVIDER *legacy = context ?
		OSSL_PROVIDER_load(context, "legacy") : nullptr;
	OSSL_LIB_CTX *previous = defaults ?
		OSSL_LIB_CTX_set0_default(context) : nullptr;
	tg_status status = !context ? TG_NO_MEMORY :
		!defaults ? TG_UNSUPPORTED : !previous ? TG_NO_MEMORY : TG_OK;
	zip_entry entry{};
	material identity{};
	BIO *certificate_bio = nullptr;
	BIO *key_bio = nullptr;
	int leaf = -1;

	entry.bytes = {const_cast<uint8_t *>(bundle.data), bundle.size};
	if (status == TG_OK &&
		(!material_read(&entry, password, &identity) ||
		 (leaf = matching_certificate(&identity)) < 0))
		status = TG_INVALID;
	if (status == TG_OK) {
		certificate_bio = BIO_new(BIO_s_mem());
		key_bio = BIO_new(BIO_s_mem());
		if (!certificate_bio || !key_bio)
			status = TG_NO_MEMORY;
	}
	if (status == TG_OK &&
		PEM_write_bio_X509(certificate_bio,
			identity.certificates[leaf]) != 1)
		status = TG_INVALID;
	for (size_t i = 0; status == TG_OK && i < identity.count; ++i)
		if ((int)i != leaf && PEM_write_bio_X509(certificate_bio,
			identity.certificates[i]) != 1)
			status = TG_INVALID;
	if (status == TG_OK && PEM_write_bio_PrivateKey(key_bio,
		identity.key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
		status = TG_INVALID;
	char *certificate_bytes = nullptr;
	char *key_bytes = nullptr;
	long certificate_size = certificate_bio ?
		BIO_get_mem_data(certificate_bio, &certificate_bytes) : 0;
	long key_size = key_bio ? BIO_get_mem_data(key_bio, &key_bytes) : 0;

	if (status == TG_OK) {
		status = tg_copy({(const uint8_t *)certificate_bytes,
			(size_t)certificate_size}, certificates);
		if (status == TG_OK)
			status = tg_copy({(const uint8_t *)key_bytes,
				(size_t)key_size}, key);
	}
	if (key_size > 0)
		OPENSSL_cleanse(key_bytes, (size_t)key_size);
	BIO_free(certificate_bio);
	BIO_free(key_bio);
	material_clear(&identity);
	if (previous)
		OSSL_LIB_CTX_set0_default(previous);
	OSSL_PROVIDER_unload(legacy);
	OSSL_PROVIDER_unload(defaults);
	OSSL_LIB_CTX_free(context);
	ERR_clear_error();
	if (status < 0) {
		clear_buffer(certificates);
		clear_buffer(key);
		return failure(diagnostic, status, "pkcs12_identity");
	}
	return TG_OK;
}

static bool find_identity(archive *package, archive *outer,
	const char *password, material *result)
{
	archive *archives[] = {package, outer};
	size_t matches = 0;

	for (archive *a : archives) {
		if (!a)
			continue;
		for (size_t i = 0; i < a->count; ++i) {
			zip_entry *entry = &a->entries[i];
			material candidate{};

			if (entry->directory ||
				(!suffix(entry->name, ".p12") &&
				 !suffix(entry->name, ".pfx")) ||
				!material_read(entry, password, &candidate))
				continue;
			if (matching_certificate(&candidate) >= 0) {
				++matches;
				if (matches == 1) {
					*result = candidate;
					candidate = {};
				}
			}
			material_clear(&candidate);
		}
	}
	return matches == 1;
}

#ifdef _WIN32
static bool private_security(SECURITY_ATTRIBUTES *attributes)
{
	HANDLE token = nullptr;
	DWORD size = 0;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	TOKEN_USER *user = static_cast<TOKEN_USER *>(malloc(size));
	char *sid = nullptr;
	tg_vec sddl{};
	bool ok = user && GetTokenInformation(token, TokenUser, user, size,
		&size) && ConvertSidToStringSidA(user->User.Sid, &sid) &&
		tg_printf(&sddl, "O:%sD:P(A;OICI;FA;;;%s)(A;OICI;FA;;;SY)",
			sid, sid);

	*attributes = {};
	attributes->nLength = sizeof(*attributes);
	if (ok)
		ok = ConvertStringSecurityDescriptorToSecurityDescriptorA(
			(const char *)sddl.data, SDDL_REVISION_1,
			&attributes->lpSecurityDescriptor, nullptr) != 0;
	LocalFree(sid);
	free(user);
	tg_vec_free(&sddl);
	CloseHandle(token);
	return ok;
}
#endif

static bool directory_create(tg_import_files *files)
{
	tg_vec path{};
	bool ok = false;
#ifdef _WIN32
	char parent[MAX_PATH + 1];
	unsigned char random[16];
	SECURITY_ATTRIBUTES attributes{};
	DWORD n = GetTempPathA(sizeof(parent), parent);

	if (!n || n >= sizeof(parent) ||
		RAND_bytes(random, sizeof(random)) != 1 ||
		!private_security(&attributes))
		return false;
	ok = tg_printf(&path, "%stak-gateway-", parent);
	for (unsigned char byte : random)
		ok = ok && tg_printf(&path, "%02x", byte);
	if (ok)
		ok = CreateDirectoryA((const char *)path.data,
			&attributes) != 0;
	LocalFree(attributes.lpSecurityDescriptor);
#else
	const char *parent = getenv("TMPDIR");

	if (!parent || *parent != '/')
		parent = "/tmp";
	ok = tg_printf(&path, "%s/tak-gateway-XXXXXX", parent) &&
		mkdtemp((char *)path.data);
#endif
	if (ok) {
		files->directory = (char *)path.data;
		path = {};
	}
	tg_vec_free(&path);
	return ok;
}

static bool file_write(tg_import_files *files, const char *name,
	BIO *data, const char **out)
{
	if (!files->directory && !directory_create(files))
		return false;
	tg_vec path{};
	temporary_file *file = static_cast<temporary_file *>(
		calloc(1, sizeof(*file)));

	if (!file || !tg_printf(&path, "%s/%s", files->directory, name)) {
		free(file);
		tg_vec_free(&path);
		return false;
	}
	char *bytes = nullptr;
	long size = BIO_get_mem_data(data, &bytes);
	bool ok = false;
	bool created = false;
#ifdef _WIN32
	SECURITY_ATTRIBUTES attributes{};
	HANDLE output = INVALID_HANDLE_VALUE;

	if (private_security(&attributes))
		output = CreateFileA((const char *)path.data, GENERIC_WRITE,
			0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
			nullptr);
	LocalFree(attributes.lpSecurityDescriptor);
	if (output != INVALID_HANDLE_VALUE) {
		created = true;
		DWORD written = 0;

		ok = size >= 0 && WriteFile(output, bytes, (DWORD)size,
			&written, nullptr) && written == (DWORD)size;
		CloseHandle(output);
	}
#else
	int output = open((const char *)path.data,
		O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);

	if (output >= 0) {
		created = true;
		long written = 0;

		while (written < size) {
			ssize_t n = write(output, bytes + written,
				(size_t)(size - written));

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			written += n;
		}
		int closed = close(output);

		ok = written == size && closed == 0;
	}
#endif
	if (!ok) {
		if (created)
			remove((const char *)path.data);
		free(file);
		tg_vec_free(&path);
		return false;
	}
	file->path = (char *)path.data;
	file->next = files->files;
	files->files = file;
	*out = file->path;
	return true;
}

static bool store_files(tg_import_files *files, unsigned index,
	material *trust, material *client, const char **ca_path,
	const char **cert_path, const char **key_path)
{
	BIO *ca = BIO_new(BIO_s_mem());
	BIO *cert = BIO_new(BIO_s_mem());
	BIO *key = BIO_new(BIO_s_mem());
	int leaf = matching_certificate(client);
	bool ok = ca && cert && key && leaf >= 0;

	for (size_t i = 0; ok && i < trust->count; ++i)
		ok = X509_check_ca(trust->certificates[i]) > 0 &&
			PEM_write_bio_X509(ca, trust->certificates[i]) == 1;
	if (ok)
		ok = PEM_write_bio_X509(cert,
			client->certificates[leaf]) == 1;
	for (size_t i = 0; ok && i < client->count; ++i)
		if ((int)i != leaf)
			ok = PEM_write_bio_X509(cert,
				client->certificates[i]) == 1;
	if (ok)
		ok = PEM_write_bio_PrivateKey(key, client->key, nullptr,
			nullptr, 0, nullptr, nullptr) == 1;
	char name[64];

	snprintf(name, sizeof(name), "%u-ca.pem", index);
	ok = ok && file_write(files, name, ca, ca_path);
	snprintf(name, sizeof(name), "%u-client.pem", index);
	ok = ok && file_write(files, name, cert, cert_path);
	snprintf(name, sizeof(name), "%u-client.key", index);
	ok = ok && file_write(files, name, key, key_path);
	char *secret = nullptr;
	long length = key ? BIO_get_mem_data(key, &secret) : 0;

	if (length > 0)
		OPENSSL_cleanse(secret, (size_t)length);
	BIO_free(ca);
	BIO_free(cert);
	BIO_free(key);
	return ok;
}

static bool emit(tg_vec *text, const char *key, const char *value)
{
	return tg_printf(text, "%s = \"", key) &&
		tg_escape(text, value, true) && tg_text(text, "\"\n");
}

static tg_status import_stream(xml_reader *preferences, unsigned index,
	archive *package, archive *outer, const char *base,
	tg_import_files *files, tg_vec *text, const char **diagnostic_key)
{
	const char *enabled = setting(preferences, "enabled", index, false);

	if (enabled && !strcmp(enabled, "false"))
		return TG_NO_EVENT;
	if (enabled && strcmp(enabled, "true"))
		return TG_INVALID;
	const char *connection = setting(preferences, "connectString", index,
		false);
	char *host = tg_strdup(connection);
	unsigned port = 0;

	if (!host)
		return connection ? TG_NO_MEMORY : TG_INVALID;
	char *protocol = strrchr(host, ':');
	bool ssl = protocol && !strcmp(protocol + 1, "ssl");

	if (protocol)
		*protocol = 0;
	char *port_text = strrchr(host, ':');
	bool valid = ssl && port_text && integer(port_text + 1, 65535, &port)
		&& port;

	if (port_text)
		*port_text = 0;
	size_t length = strlen(host);

	if (length >= 2 && host[0] == '[' && host[length - 1] == ']') {
		memmove(host, host + 1, length - 2);
		host[length - 2] = 0;
	}
	valid = valid && *host && !strpbrk(host, " /\\@?#\t\r\n") &&
		!strstr(host, "://");
	if (!valid) {
		free(host);
		return ssl ? TG_INVALID : TG_UNSUPPORTED;
	}
	const char *ca_location = setting(preferences, "caLocation", index);
	const char *ca_password = setting(preferences, "caPassword", index);
	const char *client_location = setting(preferences,
		"certificateLocation", index);
	const char *client_password = setting(preferences,
		"clientPassword", index);
	material trust{}, client{};
	tg_status status = TG_OK;

	*diagnostic_key = "trust_store";
	if (!material_read(resolve(package, outer, base, ca_location),
		ca_password, &trust))
		status = TG_INVALID;
	*diagnostic_key = status < 0 ? "trust_store" : "client_identity";
	if (status == TG_OK && client_location) {
		if (!material_read(resolve(package, outer, base,
			client_location), client_password, &client) ||
			matching_certificate(&client) < 0)
			status = TG_INVALID;
	} else if (status == TG_OK &&
		!find_identity(package, outer, client_password ?
			client_password : ca_password, &client)) {
		status = TG_INVALID;
	}
	const char *ca_path = nullptr, *cert_path = nullptr;
	const char *key_path = nullptr;

	if (status == TG_OK) {
		*diagnostic_key = "temporary_files";
		if (!store_files(files, index, &trust, &client, &ca_path,
			&cert_path, &key_path))
			status = TG_IO;
	}
	if (status == TG_OK) {
		*diagnostic_key = "configuration";
		const char *name = setting(preferences, "description", index,
			false);

		if (!tg_printf(text,
			"\n[[inputs]]\nid = \"atak-%u\"\nkind = \"tls\"\n"
			"uid = \"atak-%u\"\nprotocol = \"xml-only\"\n"
			"port = %u\n", index, index, port) ||
			!emit(text, "name", name && *name ? name : host) ||
			!emit(text, "host", host) ||
			!tg_text(text, "[inputs.tls]\n"
				"verify_hostname = false\n") ||
			!emit(text, "ca_file", ca_path) ||
			!emit(text, "cert_file", cert_path) ||
			!emit(text, "key_file", key_path))
			status = TG_NO_MEMORY;
	}
	if (status == TG_OK)
		collect_requirements(preferences, index, files);
	material_clear(&trust);
	material_clear(&client);
	free(host);
	return status;
}

tg_status tg_import_zip(const char *path, tg_buffer *toml,
	tg_import_files **files, tg_diagnostic *diagnostic)
{
	if (diagnostic)
		*diagnostic = {};
	if (!path || !*path || !tg_empty(toml) || !files || *files)
		return failure(diagnostic, TG_INVALID, "arguments");
	tg_buffer bytes{};
	archive outer{}, nested{};
	archive *package = &outer;
	xml_reader manifest{}, preferences{};
	tg_vec text{};
	tg_import_files *owned = static_cast<tg_import_files *>(
		calloc(1, sizeof(*owned)));
	const char *key = "archive";
	tg_status status = owned ? tg_read_file(path, archive_limit,
		false, &bytes) : TG_NO_MEMORY;

	if (status == TG_OK)
		status = archive_read({bytes.data, bytes.size}, &outer);
	clear_buffer(&bytes);
	size_t manifests = 0;
	zip_entry *manifest_entry = status == TG_OK ?
		find_manifest(package, &manifests) : nullptr;

	if (status == TG_OK && !manifests) {
		zip_entry *inner = nullptr;
		size_t zip_count = 0;

		for (size_t i = 0; i < outer.count; ++i)
			if (suffix(outer.entries[i].name, ".zip")) {
				inner = &outer.entries[i];
				++zip_count;
			}
		if (zip_count != 1)
			status = TG_INVALID;
		else
			status = archive_read({inner->bytes.data,
				inner->bytes.size}, &nested);
		package = &nested;
		if (status == TG_OK)
			manifest_entry = find_manifest(package, &manifests);
	}
	if (status == TG_OK) {
		key = "manifest";
		status = manifests == 1 ?
			xml_read(manifest_entry->bytes, true, &manifest) :
			TG_INVALID;
	}
	char base[512] = {};
	zip_entry *pref_entry = nullptr;
	size_t pref_count = 0;

	if (status == TG_OK) {
		size_t n = strlen(manifest_entry->name) -
			strlen("MANIFEST/manifest.xml");

		memcpy(base, manifest_entry->name, n);
		for (size_t i = 0; i < manifest.reference_count; ++i) {
			tg_vec reference{};

			if (!tg_printf(&reference, "%s%s", base,
				manifest.references[i]))
				status = TG_NO_MEMORY;
			zip_entry *entry = status == TG_OK ? find_entry(package,
				(const char *)reference.data) : nullptr;

			if (!entry)
				status = TG_INVALID;
			if (entry && suffix(entry->name, ".pref")) {
				pref_entry = entry;
				++pref_count;
			}
			tg_vec_free(&reference);
		}
		if (pref_count != 1)
			status = TG_INVALID;
	}
	if (status == TG_OK) {
		key = "preferences";
		status = xml_read(pref_entry->bytes, false, &preferences);
	}
	unsigned count = 0;

	if (status == TG_OK &&
		(!integer(value(&preferences, "count", true), 32, &count) ||
		 !count))
		status = TG_INVALID;
	OSSL_LIB_CTX *context = status == TG_OK ? OSSL_LIB_CTX_new() : nullptr;
	OSSL_PROVIDER *defaults = nullptr, *legacy = nullptr;
	OSSL_LIB_CTX *previous = nullptr;

	if (status == TG_OK) {
		key = "cryptography";
		if (!context)
			status = TG_NO_MEMORY;
		else {
			defaults = OSSL_PROVIDER_load(context, "default");
			legacy = OSSL_PROVIDER_load(context, "legacy");
			if (!defaults)
				status = TG_UNSUPPORTED;
			else
				previous = OSSL_LIB_CTX_set0_default(context);
		}
	}
	size_t imported = 0;

	for (unsigned i = 0; status == TG_OK && i < count; ++i) {
		key = "preferences";
		status = import_stream(&preferences, i, package,
			package == &outer ? nullptr : &outer, base,
			owned, &text, &key);
		if (status == TG_NO_EVENT)
			status = TG_OK;
		else if (status == TG_OK)
			++imported;
	}
	if (status == TG_OK && !imported) {
		key = "preferences";
		status = TG_INVALID;
	}
	if (previous)
		OSSL_LIB_CTX_set0_default(previous);
	OSSL_PROVIDER_unload(legacy);
	OSSL_PROVIDER_unload(defaults);
	OSSL_LIB_CTX_free(context);
	ERR_clear_error();
	xml_clear(&preferences);
	xml_clear(&manifest);
	archive_clear(&nested);
	archive_clear(&outer);
	if (status < 0) {
		tg_vec_free(&text);
		tg_import_zip_destroy(owned);
		return failure(diagnostic, status, key);
	}
	*toml = {text.data, text.size};
	*files = owned;
	return TG_OK;
}

void tg_import_zip_requirements(const tg_import_files *files,
	bool *password_auth, bool *enrollment)
{
	if (password_auth)
		*password_auth = files && files->password_auth;
	if (enrollment)
		*enrollment = files && files->enrollment;
}

bool tg_import_files_contains(const tg_import_files *files, const char *path)
{
	if (!files || !path)
		return false;
	for (const temporary_file *file = files->files; file;
		file = file->next)
		if (!strcmp(file->path, path))
			return true;
	return false;
}

void tg_import_zip_destroy(tg_import_files *files)
{
	if (!files)
		return;
	while (files->files) {
		temporary_file *file = files->files;

		files->files = file->next;
		remove(file->path);
		free(file->path);
		free(file);
	}
	if (files->directory) {
#ifdef _WIN32
		RemoveDirectoryA(files->directory);
#else
		rmdir(files->directory);
#endif
	}
	free(files->directory);
	free(files);
}
