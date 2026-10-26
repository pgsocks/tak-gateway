#ifndef TG_IMPORT_ZIP_H
#define TG_IMPORT_ZIP_H
#include "internal.h"

struct tg_import_files;

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

/* Remove the owned temporary files and directory, then release the handle.
 * NULL is harmless. The caller must no longer use their credential paths.
 */
void tg_import_zip_destroy(tg_import_files *files);
#endif
