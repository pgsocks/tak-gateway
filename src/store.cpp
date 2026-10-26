#include "store.h"
#include "cli_options.h"
#include "managed_runtime.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

struct tg_store {
	char *path;
	bool exclusive;
#ifdef _WIN32
	HANDLE lock;
#else
	int lock;
#endif
};

struct promoted_file {
	char *path;
	promoted_file *next;
};

static tg_status error(tg_diagnostic *d, tg_status status, const char *key)
{
	if (d) {
		*d = {};
		d->status = status;
		snprintf(d->key, sizeof(d->key), "store.%s", key);
	}
	return status;
}

void tg_web_settings_default(tg_web_settings *settings)
{
	if (settings) {
		*settings = {};
		snprintf(settings->bind, sizeof(settings->bind),
			"127.0.0.1:8080");
	}
}

static bool absolute(const char *path)
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

static bool join(tg_vec *out, const char *base, const char *name)
{
	return tg_printf(out, "%s/%s", base, name);
}

static void secret_release(tg_buffer *bytes)
{
	if (bytes->data)
		OPENSSL_cleanse(bytes->data, bytes->size);
	tg_buffer_release(bytes);
}

#ifdef _WIN32
static bool private_attributes(SECURITY_ATTRIBUTES *attributes)
{
	HANDLE token = nullptr;
	DWORD size = 0;

	*attributes = {};
	attributes->nLength = sizeof(*attributes);
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	TOKEN_USER *user = static_cast<TOKEN_USER *>(malloc(size));
	char *sid = nullptr;
	tg_vec text{};
	bool ok = user && GetTokenInformation(token, TokenUser, user, size,
		&size) && ConvertSidToStringSidA(user->User.Sid, &sid) &&
		tg_printf(&text,
			"O:%sD:P(A;OICI;FA;;;%s)(A;OICI;FA;;;SY)", sid, sid);

	/* Elevated tokens may otherwise assign Administrators as the owner. */
	if (ok)
		ok = ConvertStringSecurityDescriptorToSecurityDescriptorA(
			(const char *)text.data, SDDL_REVISION_1,
			&attributes->lpSecurityDescriptor, nullptr) != 0;
	LocalFree(sid);
	free(user);
	tg_vec_free(&text);
	CloseHandle(token);
	return ok;
}

static bool private_handle(HANDLE handle)
{
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	PSID owner = nullptr;
	PACL acl = nullptr;

	if (GetSecurityInfo(handle, SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		&owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS)
		return false;
	bool safe = owner && acl;

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

		if (!EqualSid(owner, sid) &&
			!IsWellKnownSid(sid, WinLocalSystemSid) &&
			!IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
			safe = false;
	}
	LocalFree(descriptor);
	return safe;
}
#endif

/* Return one for a safe directory, zero for missing, minus one for unsafe. */
static int directory(const char *path, bool private_mode)
{
#ifdef _WIN32
	DWORD attributes = GetFileAttributesA(path);

	if (attributes == INVALID_FILE_ATTRIBUTES)
		return GetLastError() == ERROR_FILE_NOT_FOUND ||
			GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : -1;
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
		return -1;
	if (!private_mode)
		return 1;
	HANDLE handle = CreateFileA(path, READ_CONTROL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
			FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	bool ok = handle != INVALID_HANDLE_VALUE && private_handle(handle);

	if (handle != INVALID_HANDLE_VALUE)
		CloseHandle(handle);
	return ok ? 1 : -1;
#else
	struct stat info{};

	if (lstat(path, &info))
		return errno == ENOENT ? 0 : -1;
	return S_ISDIR(info.st_mode) &&
		(!private_mode || (info.st_uid == geteuid() &&
			!(info.st_mode & 0077))) ? 1 : -1;
#endif
}

static bool sync_directory(const char *path);

static bool sync_parent(const char *path)
{
	char *parent = tg_strdup(path);

	if (!parent)
		return false;
	char *slash = strrchr(parent, '/');
#ifdef _WIN32
	char *back = strrchr(parent, '\\');

	if (back && (!slash || back > slash))
		slash = back;
#endif
	bool ok = slash != nullptr;

	if (ok) {
		bool root = slash == parent;
#ifdef _WIN32
		root = root || (slash == parent + 2 && parent[1] == ':');
#endif
		slash[root ? 1 : 0] = 0;
		ok = sync_directory(parent);
	}
	free(parent);
	return ok;
}

static bool make_directory(const char *path, bool exclusive = false)
{
	if (!exclusive && directory(path, false) == 1)
		return true;
#ifdef _WIN32
	SECURITY_ATTRIBUTES attributes{};

	if (!private_attributes(&attributes))
		return false;
	bool ok = CreateDirectoryA(path, &attributes) != 0;

	LocalFree(attributes.lpSecurityDescriptor);
	return ok && sync_directory(path) && sync_parent(path);
#else
	return mkdir(path, 0700) == 0 && sync_directory(path) &&
		sync_parent(path);
#endif
}

static bool make_tree(const char *path)
{
	char *copy = tg_strdup(path);

	if (!copy)
		return false;
	bool ok = true;

	for (size_t i = 1; ok && copy[i]; ++i) {
		if (copy[i] != '/' && copy[i] != '\\')
			continue;
#ifdef _WIN32
		if (i == 2 && copy[1] == ':')
			continue;
#endif
		char separator = copy[i];

		copy[i] = 0;
		ok = make_directory(copy);
		copy[i] = separator;
	}
	ok = ok && make_directory(copy) && directory(copy, true) == 1;
	free(copy);
	return ok;
}

static bool sync_directory(const char *path)
{
#ifdef _WIN32
	(void)path;
	return true;
#else
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return false;
	bool ok = fsync(fd) == 0;
	int closed = close(fd);

	return ok && closed == 0;
#endif
}

static tg_status read_private(const char *path, size_t maximum,
	tg_buffer *out, bool *missing = nullptr)
{
	if (missing)
		*missing = false;
	tg_vec bytes{};
	tg_status status = TG_OK;
#ifdef _WIN32
	HANDLE file = CreateFileA(path, GENERIC_READ | READ_CONTROL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

	if (file == INVALID_HANDLE_VALUE) {
		if (missing && (GetLastError() == ERROR_FILE_NOT_FOUND ||
			GetLastError() == ERROR_PATH_NOT_FOUND)) {
			*missing = true;
			return TG_OK;
		}
		return TG_IO;
	}
	BY_HANDLE_FILE_INFORMATION info{};
	LARGE_INTEGER size{};
	bool valid = GetFileInformationByHandle(file, &info) &&
		!(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
			FILE_ATTRIBUTE_DIRECTORY)) && private_handle(file) &&
		GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
		(uint64_t)size.QuadPart <= maximum;

	if (!valid)
		status = TG_IO;
	uint8_t chunk[4096];
	DWORD n = 0;

	while (status == TG_OK) {
		if (!ReadFile(file, chunk, sizeof(chunk), &n, nullptr)) {
			status = TG_IO;
			break;
		}
		if (!n)
			break;
		if (n > maximum - bytes.size)
			status = TG_LIMIT;
		else if (!tg_append(&bytes, chunk, n))
			status = TG_NO_MEMORY;
	}
	CloseHandle(file);
#else
	int file = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);

	if (file < 0) {
		if (missing && errno == ENOENT) {
			*missing = true;
			return TG_OK;
		}
		return TG_IO;
	}
	struct stat info{};

	if (fstat(file, &info) || !S_ISREG(info.st_mode) ||
		info.st_uid != geteuid() || (info.st_mode & 0077) ||
		info.st_size < 0 || (uint64_t)info.st_size > maximum)
		status = TG_IO;
	uint8_t chunk[4096];

	while (status == TG_OK) {
		ssize_t n = read(file, chunk, sizeof(chunk));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			status = TG_IO;
			break;
		}
		if (!n)
			break;
		if ((size_t)n > maximum - bytes.size)
			status = TG_LIMIT;
		else if (!tg_append(&bytes, chunk, (size_t)n))
			status = TG_NO_MEMORY;
	}
	if (close(file) && status == TG_OK)
		status = TG_IO;
#endif
	if (status == TG_OK) {
		*out = {bytes.data, bytes.size};
		bytes = {};
	}
	if (bytes.data)
		OPENSSL_cleanse(bytes.data, bytes.size);
	tg_vec_free(&bytes);
	return status;
}

static tg_status write_new(const char *path, tg_bytes bytes)
{
	bool created = false, ok = false;
#ifdef _WIN32
	SECURITY_ATTRIBUTES attributes{};

	if (!private_attributes(&attributes))
		return TG_IO;
	HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, &attributes,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

	LocalFree(attributes.lpSecurityDescriptor);
	if (file != INVALID_HANDLE_VALUE) {
		created = true;
		size_t offset = 0;

		while (offset < bytes.size) {
			DWORD n = 0;
			DWORD size = (DWORD)(bytes.size - offset);

			if (!WriteFile(file, bytes.data + offset, size, &n,
				nullptr) || !n)
				break;
			offset += n;
		}
		ok = offset == bytes.size && FlushFileBuffers(file);
		if (!CloseHandle(file))
			ok = false;
	}
#else
	int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
		O_CLOEXEC, 0600);

	if (file >= 0) {
		created = true;
		size_t offset = 0;

		while (offset < bytes.size) {
			ssize_t n = write(file, bytes.data + offset,
				bytes.size - offset);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			offset += (size_t)n;
		}
		ok = offset == bytes.size && fsync(file) == 0;
		if (close(file))
			ok = false;
	}
#endif
	if (!ok && created)
		remove(path);
	return ok ? TG_OK : TG_IO;
}

static bool random_name(char name[33])
{
	uint8_t bytes[16];
	const char hex[] = "0123456789abcdef";

	if (RAND_bytes(bytes, sizeof(bytes)) != 1)
		return false;
	for (size_t i = 0; i < sizeof(bytes); ++i) {
		name[i * 2] = hex[bytes[i] >> 4];
		name[i * 2 + 1] = hex[bytes[i] & 15];
	}
	name[32] = 0;
	return true;
}

static bool hex_name(const char *name, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (!((name[i] >= '0' && name[i] <= '9') ||
			(name[i] >= 'a' && name[i] <= 'f')))
			return false;
	return true;
}

tg_status tg_store_select(const char *selector, const char *cwd,
	tg_buffer *out, tg_diagnostic *d)
{
	if (d)
		*d = {};
	if (!absolute(cwd) || !tg_empty(out) || (selector && !*selector))
		return error(d, TG_INVALID, "arguments");
	const char *selected = selector ? selector :
		getenv("TAK_GATEWAY_STATE_DIR");
	tg_vec path{};
	bool ok = true;

	if (selected && *selected) {
		ok = (absolute(selected) || (tg_text(&path, cwd) &&
			tg_text(&path, "/"))) && tg_text(&path, selected);
	} else {
#ifdef _WIN32
		const char *base = getenv("LOCALAPPDATA");

		ok = absolute(base) && join(&path, base, "TakGateway");
#else
		const char *base = getenv("XDG_STATE_HOME");

		if (absolute(base))
			ok = join(&path, base, "tak-gateway");
		else {
			base = getenv("HOME");
			ok = absolute(base) &&
				join(&path, base, ".local/state/tak-gateway");
		}
#endif
	}
	if (!ok || path.size > 30000 ||
		!tg_utf8({path.data, path.size})) {
		tg_vec_free(&path);
		return error(d, ok ? TG_INVALID : TG_IO, "directory");
	}
	*out = {path.data, path.size};
	return TG_OK;
}

static tg_status acquire_lock(tg_store *store)
{
	tg_vec path{};

	if (!join(&path, store->path, ".lock"))
		return TG_NO_MEMORY;
	tg_status status = TG_OK;
#ifdef _WIN32
	SECURITY_ATTRIBUTES attributes{};

	if (!private_attributes(&attributes))
		status = TG_IO;
	if (status == TG_OK) {
		store->lock = CreateFileA((const char *)path.data,
			GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0,
			&attributes, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr);
		if (store->lock == INVALID_HANDLE_VALUE)
			status = GetLastError() == ERROR_SHARING_VIOLATION ?
				TG_BAD_STATE : TG_IO;
		else if (!private_handle(store->lock))
			status = TG_IO;
	}
	LocalFree(attributes.lpSecurityDescriptor);
#else
	store->lock = open((const char *)path.data,
		O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	struct stat info{};

	if (store->lock < 0 || fstat(store->lock, &info) ||
		!S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
		(info.st_mode & 0077))
		status = TG_IO;
	else if (flock(store->lock, LOCK_EX | LOCK_NB))
		status = errno == EWOULDBLOCK || errno == EAGAIN ?
			TG_BAD_STATE : TG_IO;
#endif
	tg_vec_free(&path);
	return status;
}

tg_status tg_store_open(const char *path, bool exclusive, tg_store **out,
	tg_diagnostic *d)
{
	if (d)
		*d = {};
	if (!absolute(path) || !out || *out || strlen(path) > 30000 ||
		!tg_utf8({(const uint8_t *)path, strlen(path)}))
		return error(d, TG_INVALID, "arguments");
	tg_store *store = static_cast<tg_store *>(calloc(1, sizeof(*store)));

	if (!store)
		return error(d, TG_NO_MEMORY, "directory");
#ifdef _WIN32
	store->lock = INVALID_HANDLE_VALUE;
#else
	store->lock = -1;
#endif
	store->path = tg_strdup(path);
	store->exclusive = exclusive;
	tg_status status = store->path ? TG_OK : TG_NO_MEMORY;

	if (status == TG_OK) {
		size_t length = strlen(store->path);

		while (length > 1 && (store->path[length - 1] == '/' ||
			store->path[length - 1] == '\\')) {
#ifdef _WIN32
			if (length == 3 && store->path[1] == ':')
				break;
#endif
			store->path[--length] = 0;
		}
		int exists = directory(store->path, true);

		if (exists < 0 || (exclusive && !exists &&
			!make_tree(store->path)))
			status = TG_IO;
	}
	if (status == TG_OK && exclusive)
		status = acquire_lock(store);
	if (status == TG_OK) {
		tg_buffer snapshot{};
		tg_web_settings web{};
		uint64_t revision = 0;

		status = tg_store_load(store, &snapshot, &web, &revision, d);
		tg_buffer_release(&snapshot);
	}
	if (status == TG_OK && exclusive) {
		const char *directories[] = {"assets", "revisions"};

		for (const char *name : directories) {
			tg_vec child{};
			bool ok = join(&child, store->path, name) &&
				make_directory((const char *)child.data) &&
				directory((const char *)child.data, true) == 1;

			tg_vec_free(&child);
			if (!ok)
				status = TG_IO;
		}
	}
	if (status < 0) {
		tg_store_close(store);
		if (!d || d->status == TG_OK)
			return error(d, status, "open");
		return status;
	}
	*out = store;
	return TG_OK;
}

static bool pointer_parse(tg_buffer bytes, uint64_t *revision,
	char generation[33])
{
	if (!bytes.size || bytes.size > 128)
		return false;
	size_t i = 0;
	uint64_t value = 0;

	while (i < bytes.size && bytes.data[i] >= '0' &&
		bytes.data[i] <= '9') {
		unsigned digit = bytes.data[i++] - '0';

		if (value > (UINT64_MAX - digit) / 10)
			return false;
		value = value * 10 + digit;
	}
	if (!i || !value || i + 34 != bytes.size ||
		bytes.data[i++] != ' ' || bytes.data[bytes.size - 1] != '\n' ||
		!hex_name((const char *)bytes.data + i, 32))
		return false;
	memcpy(generation, bytes.data + i, 32);
	generation[32] = 0;
	*revision = value;
	return true;
}

tg_status tg_store_load(tg_store *store, tg_buffer *toml,
	tg_web_settings *web, uint64_t *revision, tg_diagnostic *d)
{
	if (d)
		*d = {};
	if (!store || !tg_empty(toml) || !web || !revision)
		return error(d, TG_INVALID, "arguments");
	*web = {};
	*revision = 0;
	tg_vec path{}, generation_path{};
	tg_buffer pointer{}, metadata{}, gateway{};
	bool missing = false;
	tg_status status = join(&path, store->path, "CURRENT") ?
		read_private((const char *)path.data, 128, &pointer, &missing) :
		TG_NO_MEMORY;

	if (status == TG_OK && missing) {
		tg_web_settings_default(web);
		const char defaults[] = "version=1\n";

		status = tg_copy({(const uint8_t *)defaults, strlen(defaults)},
			toml);
	} else if (status == TG_OK) {
		char generation[33];
		uint64_t selected = 0;

		if (!pointer_parse(pointer, &selected, generation))
			status = TG_MALFORMED;
		if (status == TG_OK && (!tg_printf(&generation_path,
			"%s/revisions/%s", store->path, generation) ||
			directory((const char *)generation_path.data,
				true) != 1))
			status = TG_IO;
		tg_vec_free(&path);
		if (status == TG_OK) {
			if (!join(&path, (const char *)generation_path.data,
				"gateway.toml"))
				status = TG_NO_MEMORY;
			else
				status = read_private((const char *)path.data,
					8 * 1024 * 1024, &gateway);
		}
		tg_vec_free(&path);
		if (status == TG_OK) {
			if (!join(&path, (const char *)generation_path.data,
				"web"))
				status = TG_NO_MEMORY;
			else
				status = read_private((const char *)path.data,
					512, &metadata);
		}
		tg_web_settings selected_web{};

		if (status == TG_OK) {
			if (metadata.size < 4 || metadata.size > 258 ||
				(metadata.data[0] != '0' &&
					metadata.data[0] != '1') ||
				metadata.data[1] != '\n' ||
				metadata.data[metadata.size - 1] != '\n' ||
				!tg_utf8({metadata.data, metadata.size}))
				status = TG_MALFORMED;
			else {
				selected_web.enabled = metadata.data[0] == '1';
				memcpy(selected_web.bind, metadata.data + 2,
					metadata.size - 3);
			}
		}
		if (status == TG_OK) {
			tg_config *parsed = nullptr;
			tg_import_files *files = nullptr;
			tg_web_settings checked = selected_web;

			status = tg_cli_compose({gateway.data, gateway.size}, 0,
				nullptr, store->path, true, &parsed, &files,
				&checked, nullptr);
			if (status == TG_OK && (checked.enabled !=
				selected_web.enabled || strcmp(checked.bind,
				selected_web.bind)))
				status = TG_MALFORMED;
			tg_config_destroy(parsed);
			tg_import_zip_destroy(files);
		}
		if (status == TG_OK) {
			*web = selected_web;
			*revision = selected;
			*toml = gateway;
			gateway = {};
		}
	}
	tg_buffer_release(&pointer);
	tg_buffer_release(&metadata);
	tg_buffer_release(&gateway);
	tg_vec_free(&path);
	tg_vec_free(&generation_path);
	if (status < 0) {
		*web = {};
		*revision = 0;
		return error(d, status, "revision");
	}
	return TG_OK;
}

tg_status tg_store_write_asset(tg_store *store, const char *extension,
	tg_bytes bytes, tg_buffer *out, tg_diagnostic *d)
{
	if (d)
		*d = {};
	if (!store || !tg_span(bytes) || !tg_empty(out))
		return error(d, TG_INVALID, "arguments");
	if (!store->exclusive)
		return error(d, TG_BAD_STATE, "lock");
	if (bytes.size > 16 * 1024 * 1024)
		return error(d, TG_LIMIT, "asset");
	if (extension) {
		size_t n = strlen(extension);

		if (!n || n > 16)
			return error(d, TG_INVALID, "extension");
		for (size_t i = 0; i < n; ++i)
			if (!((extension[i] >= 'a' && extension[i] <= 'z') ||
				(extension[i] >= 'A' && extension[i] <= 'Z') ||
				(extension[i] >= '0' && extension[i] <= '9')))
				return error(d, TG_INVALID, "extension");
	}
	char random[33];
	tg_vec path{};
	tg_status status = random_name(random) && tg_printf(&path,
		"%s/assets/%s%s%s", store->path, random,
		extension ? "." : "", extension ? extension : "") ?
		write_new((const char *)path.data, bytes) : TG_NO_MEMORY;

	if (status == TG_OK) {
		tg_vec parent{};

		if (!join(&parent, store->path, "assets") ||
			!sync_directory((const char *)parent.data)) {
			remove((const char *)path.data);
			status = TG_IO;
		}
		tg_vec_free(&parent);
	}
	if (status < 0) {
		tg_vec_free(&path);
		return error(d, status, "asset");
	}
	*out = {path.data, path.size};
	return TG_OK;
}

tg_status tg_store_remove_asset(tg_store *store, const char *path,
	tg_diagnostic *d)
{
	if (d)
		*d = {};
	if (!store || !path)
		return error(d, TG_INVALID, "arguments");
	if (!store->exclusive)
		return error(d, TG_BAD_STATE, "lock");
	tg_vec prefix{};
	bool valid = tg_printf(&prefix, "%s/assets/", store->path) &&
		!strncmp(path, (const char *)prefix.data, prefix.size);
	const char *name = valid ? path + prefix.size : "";
	size_t n = strlen(name);

	valid = valid && n >= 32 && hex_name(name, 32) &&
		(n == 32 || (n > 33 && n <= 49 && name[32] == '.'));
	for (size_t i = 33; valid && i < n; ++i)
		valid = (name[i] >= 'a' && name[i] <= 'z') ||
			(name[i] >= 'A' && name[i] <= 'Z') ||
			(name[i] >= '0' && name[i] <= '9');
	tg_vec_free(&prefix);
	if (!valid)
		return error(d, TG_INVALID, "asset");
	if (remove(path) && errno != ENOENT)
		return error(d, TG_IO, "asset");
	return TG_OK;
}

static tg_status promote(tg_store *store, const tg_config *config,
	const tg_import_files *imports, tg_buffer *result,
	promoted_file **assets)
{
	tg_buffer exported{};
	tg_status status = tg_config_export(config, &exported, nullptr);
	char *arguments[1024]{};
	int argc = 0;
	const char *keys[] = {"tls.ca_file", "tls.cert_file", "tls.key_file",
		"tls.pkcs12_file", "tls.key_password_file",
		"tls.pkcs12_password_file", "auth.password_file",
		"maintenance.identity_xml_file"};

	for (size_t i = 0; status == TG_OK && i < config->input_count; ++i) {
		const tg_input_config *input = &config->inputs[i];
		const char *paths[] = {input->ca_file, input->cert_file,
			input->key_file, input->pkcs12_file,
			input->key_password_file, input->pkcs12_password_file,
			input->password_file, input->identity_file};

		for (size_t j = 0; status == TG_OK && j < 8; ++j) {
			if (!tg_import_files_contains(imports, paths[j]))
				continue;
			if (argc == 1024) {
				status = TG_LIMIT;
				break;
			}
			tg_buffer content{}, saved{};

			status = read_private(paths[j], 16 * 1024 * 1024,
				&content);
			if (status == TG_OK)
				status = tg_store_write_asset(store, "pem",
					{content.data, content.size}, &saved,
					nullptr);
			secret_release(&content);
			if (status < 0)
				break;
			promoted_file *file = static_cast<promoted_file *>(
				calloc(1, sizeof(*file)));

			if (!file) {
				tg_store_remove_asset(store,
					(const char *)saved.data, nullptr);
				tg_buffer_release(&saved);
				status = TG_NO_MEMORY;
				break;
			}
			file->path = (char *)saved.data;
			file->next = *assets;
			*assets = file;
			tg_vec assignment{};
			bool ok = tg_printf(&assignment, "inputs[%zu].%s=\"",
				i, keys[j]) && tg_escape(&assignment,
				file->path, true) && tg_text(&assignment, "\"");

			if (!ok) {
				tg_vec_free(&assignment);
				status = TG_NO_MEMORY;
				break;
			}
			arguments[argc++] = (char *)"--set";
			arguments[argc++] = (char *)assignment.data;
		}
	}
	if (status == TG_OK && argc) {
		tg_config *updated = nullptr;
		tg_import_files *files = nullptr;
		tg_web_settings web{};

		tg_web_settings_default(&web);
		status = tg_cli_compose({exported.data, exported.size}, argc,
			arguments, store->path, true, &updated, &files, &web,
			nullptr);
		if (status == TG_OK) {
			tg_buffer_release(&exported);
			status = tg_config_export(updated, &exported, nullptr);
		}
		tg_config_destroy(updated);
		tg_import_zip_destroy(files);
	}
	for (int i = 1; i < argc; i += 2)
		free(arguments[i]);
	if (status == TG_OK) {
		*result = exported;
		exported = {};
	}
	tg_buffer_release(&exported);
	return status;
}

static bool atomic_replace(const char *from, const char *to)
{
#ifdef _WIN32
	return MoveFileExA(from, to,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(from, to) == 0;
#endif
}

tg_status tg_store_save_import(tg_store *store, const tg_config *config,
	const tg_web_settings *web, uint64_t expected_revision,
	const tg_import_files *imports, uint64_t *new_revision,
	tg_diagnostic *d)
{
	if (d)
		*d = {};
	if (new_revision)
		*new_revision = 0;
	if (!store || !config || !web || !new_revision ||
		!memchr(web->bind, 0, sizeof(web->bind)))
		return error(d, TG_INVALID, "arguments");
	if (!store->exclusive)
		return error(d, TG_BAD_STATE, "lock");
	tg_buffer previous{}, document{};
	tg_web_settings current_web{};
	uint64_t current = 0;
	tg_status status = tg_store_load(store, &previous, &current_web,
		&current, d);

	tg_buffer_release(&previous);
	if (status == TG_OK && current != expected_revision)
		status = TG_BAD_STATE;
	if (status == TG_OK && current == UINT64_MAX)
		status = TG_LIMIT;
	promoted_file *assets = nullptr;

	if (status == TG_OK)
		status = promote(store, config, imports, &document, &assets);
	if (status == TG_OK) {
		tg_config *checked = nullptr;
		tg_import_files *files = nullptr;
		tg_web_settings copy = *web;

		status = tg_cli_compose({document.data, document.size}, 0,
			nullptr, store->path, true, &checked, &files, &copy,
			nullptr);
		tg_config_destroy(checked);
		tg_import_zip_destroy(files);
	}
	char generation[33] = {};
	tg_vec parent{}, path{}, gateway_path{}, metadata_path{};
	tg_vec pointer{}, pointer_path{}, metadata{};
	bool published = false, made = false;

	if (status == TG_OK && (!random_name(generation) ||
		!tg_printf(&parent, "%s/revisions/%s", store->path,
			generation)))
		status = TG_NO_MEMORY;
	if (status == TG_OK) {
		made = make_directory((const char *)parent.data, true);
		if (!made)
			status = TG_IO;
	}
	if (status == TG_OK && (!join(&gateway_path,
		(const char *)parent.data, "gateway.toml") ||
		!join(&metadata_path, (const char *)parent.data, "web") ||
		!tg_printf(&metadata, "%u\n%s\n", web->enabled ? 1 : 0,
			web->bind)))
		status = TG_NO_MEMORY;
	if (status == TG_OK)
		status = write_new((const char *)gateway_path.data,
			{document.data, document.size});
	if (status == TG_OK)
		status = write_new((const char *)metadata_path.data,
			{metadata.data, metadata.size});
	if (status == TG_OK && !sync_directory((const char *)parent.data))
		status = TG_IO;
	if (status == TG_OK) {
		tg_vec revisions{};

		if (!join(&revisions, store->path, "revisions") ||
			!sync_directory((const char *)revisions.data))
			status = TG_IO;
		tg_vec_free(&revisions);
	}
	if (status == TG_OK && (!tg_printf(&pointer, "%llu %s\n",
		(unsigned long long)(current + 1), generation) ||
		!tg_printf(&pointer_path, "%s/.CURRENT-%s", store->path,
			generation) || !join(&path, store->path, "CURRENT")))
		status = TG_NO_MEMORY;
	if (status == TG_OK)
		status = write_new((const char *)pointer_path.data,
			{pointer.data, pointer.size});
	if (status == TG_OK) {
		published = atomic_replace((const char *)pointer_path.data,
			(const char *)path.data);
		if (!published)
			status = TG_IO;
		else {
			*new_revision = current + 1;
			if (!sync_directory(store->path))
				status = TG_IO;
		}
	}
	if (!published) {
		if (pointer_path.data)
			remove((const char *)pointer_path.data);
		if (gateway_path.data)
			remove((const char *)gateway_path.data);
		if (metadata_path.data)
			remove((const char *)metadata_path.data);
		if (made) {
#ifdef _WIN32
			RemoveDirectoryA((const char *)parent.data);
#else
			rmdir((const char *)parent.data);
#endif
		}
	}
	while (assets) {
		promoted_file *file = assets;

		assets = file->next;
		if (!published)
			tg_store_remove_asset(store, file->path, nullptr);
		free(file->path);
		free(file);
	}
	tg_buffer_release(&document);
	tg_vec_free(&parent);
	tg_vec_free(&path);
	tg_vec_free(&gateway_path);
	tg_vec_free(&metadata_path);
	tg_vec_free(&pointer);
	tg_vec_free(&pointer_path);
	tg_vec_free(&metadata);
	return status < 0 ? error(d, status,
		published ? "durability" : "save") : TG_OK;
}

tg_status tg_store_save(tg_store *store, const tg_config *config,
	const tg_web_settings *web, uint64_t expected_revision,
	uint64_t *new_revision, tg_diagnostic *d)
{
	return tg_store_save_import(store, config, web, expected_revision,
		nullptr, new_revision, d);
}

const char *tg_store_path(const tg_store *store)
{
	return store ? store->path : nullptr;
}

void tg_store_close(tg_store *store)
{
	if (!store)
		return;
#ifdef _WIN32
	if (store->lock != INVALID_HANDLE_VALUE)
		CloseHandle(store->lock);
#else
	if (store->lock >= 0)
		close(store->lock);
#endif
	free(store->path);
	free(store);
}
