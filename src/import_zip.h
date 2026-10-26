#ifndef TG_IMPORT_ZIP_H
#define TG_IMPORT_ZIP_H
#include "internal.h"

struct tg_import_files;

/* Return whether path is an exact owned credential path in this live handle.
 * Pure lookup: no filesystem access. NULL handle/path returns false.
 */
bool tg_import_files_contains(const tg_import_files *files, const char *path);

/*
 * Import one ATAK mission package, optionally inside a transport ZIP, into
 * TOML [[inputs]] tables. path is a nonempty filename. toml must point to an
 * empty buffer; files must point to a NULL handle. Both are owned by the
 * caller on success. Append toml to the caller's configuration, then parse
 * and validate it through the regular configuration API.
 *
 * Enabled SSL streams require a CA store and a client identity. Enrollment
 * is not performed. An outer sibling identity is used only when exactly one
 * decryptable private-key store is available. Passwords come from preferences.
 * Imported inputs use certificate authentication and XML-only streaming.
 *
 * Reads at most 16 MiB of archive data, accepts one nested package, at most
 * 256 entries per archive, 32 streams, and 1 MiB XML documents. Archive paths,
 * references, duplicate entries, XML entities and ambiguous selections are
 * rejected. No archive path is extracted to the filesystem.
 *
 * Creates a private temporary directory containing credential PEM files.
 * Keep files alive until all configurations/runtimes using toml have been
 * destroyed. Release toml with tg_buffer_release and files with destroy.
 * On failure both outputs remain empty; partial temporary files are removed.
 * diagnostic, if non-NULL, receives a non-secret import key and status.
 * Performs no network access. Independent calls do not share mutable state.
 */
tg_status tg_import_zip(const char *path, tg_buffer *toml,
	tg_import_files **files, tg_diagnostic *diagnostic);

/* Decode a PKCS#12 client identity into a leaf-first PEM certificate chain
 * and an unencrypted PEM private key. bundle is borrowed, nonempty and at
 * most 8 MiB; password is NULL/empty for an empty password, or a string of
 * at most 65536 bytes. Both distinct output buffers must initially be empty.
 * The stores must have a valid MAC, bounded KDF work and one private key
 * with a matching certificate. Legacy algorithms use an isolated OpenSSL
 * context without changing global providers. Performs no I/O or networking.
 * Valid empty outputs remain empty on failure; invalid output preconditions
 * leave the supplied buffers unchanged. Caller releases certificates with
 * tg_buffer_release and must erase private-key bytes before releasing key.
 * diagnostic receives only a non-secret status/key when provided.
 */
tg_status tg_import_pkcs12_identity(tg_bytes bundle, const char *password,
	tg_buffer *certificates, tg_buffer *key, tg_diagnostic *diagnostic);

/* Report password-authentication/enrollment requests from enabled imported
 * streams. Output pointers are optional; a NULL handle reports false.
 * These metadata flags do not perform enrollment or change authentication.
 */
void tg_import_zip_requirements(const tg_import_files *files,
	bool *password_auth, bool *enrollment);

/* Remove the owned temporary files and directory, then release the handle.
 * NULL is harmless. The caller must no longer use their credential paths.
 */
void tg_import_zip_destroy(tg_import_files *files);
#endif
