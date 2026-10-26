#ifndef TG_STORE_H
#define TG_STORE_H
#include "internal.h"

struct tg_store;
struct tg_import_files;

struct tg_web_settings {
	bool enabled;
	char bind[256];
};

/* Initialize disabled UI settings bound to 127.0.0.1:8080. */
void tg_web_settings_default(tg_web_settings *settings);

/* Resolve CLI selector, TAK_GATEWAY_STATE_DIR, then platform user state.
 * cwd must be absolute; an explicit/environment relative path uses cwd.
 * Return an owned NUL-terminated path in initially empty out.
 */
tg_status tg_store_select(const char *selector, const char *cwd,
	tg_buffer *out, tg_diagnostic *diagnostic);

/* Open an absolute state directory. exclusive creates private directories
 * and holds a nonblocking lifetime writer lock. A competing writer fails
 * with TG_BAD_STATE. Read-only opens never take the writer lock or create
 * files, and may read while a runner owns the lock. Missing state is empty.
 * Existing insecure/symlink state or corrupt CURRENT is rejected. out must
 * initially point to NULL; no handle is returned on failure.
 */
tg_status tg_store_open(const char *path, bool exclusive, tg_store **out,
	tg_diagnostic *diagnostic);

/* Read one immutable revision selected by atomic CURRENT. toml is an empty
 * owned-buffer output. Uninitialized state returns version=1, revision=0
 * and default web settings. Other successful results have revision>0.
 * Config bytes exclude web settings and contain resolved credential paths.
 * On failure toml remains empty and web/revision are cleared. No secrets are
 * loaded from referenced files. The caller owns toml via buffer_release.
 */
tg_status tg_store_load(tg_store *store, tg_buffer *toml,
	tg_web_settings *web, uint64_t *revision, tg_diagnostic *diagnostic);

/* Persist a managed configuration and web settings under the writer lock.
 * expected_revision is a compare-and-swap token (0 means uninitialized).
 * Stale tokens fail TG_BAD_STATE without changing CURRENT. Write immutable
 * revision files, then atomically replace CURRENT. new_revision is zero
 * before publication. If the final directory sync fails after publication,
 * return TG_IO with nonzero new_revision and diagnostic store.durability:
 * the revision is committed, but its crash durability is unconfirmed. Callers
 * must reload that revision and retain its assets even on this error.
 * All external file/environment references remain unchanged.
 * Actual secret values never enter configuration TOML. No raw ZIP retained.
 * Does not apply a runtime. Caller retains config and importer ownership.
 */
tg_status tg_store_save(tg_store *store, const tg_config *config,
	const tg_web_settings *web, uint64_t expected_revision,
	uint64_t *new_revision, tg_diagnostic *diagnostic);

/* Save with the same commit/error contract, promoting only credential paths
 * which are actually owned by imports. The borrowed import handle remains
 * owned by the caller and must stay alive throughout this synchronous call.
 * NULL behaves exactly as ordinary store_save. Reload persisted configuration
 * before releasing imports; the borrowed input config is not modified.
 */
tg_status tg_store_save_import(tg_store *store, const tg_config *config,
	const tg_web_settings *web, uint64_t expected_revision,
	const tg_import_files *imports, uint64_t *new_revision,
	tg_diagnostic *diagnostic);

/* Write one private, randomly named managed credential asset (at most16MiB).
 * extension is NULL or1..16 ASCII letters/digits. out is initially empty and
 * receives an owned absolute NUL-terminated path. Requires exclusive store.
 * The caller removes an asset if its surrounding configuration edit fails.
 */
tg_status tg_store_write_asset(tg_store *store, const char *extension,
	tg_bytes bytes, tg_buffer *out, tg_diagnostic *diagnostic);

/* Remove only a direct managed assets child. Requires exclusive store.
 * Never remove a file outside this store. Missing managed files are harmless.
 */
tg_status tg_store_remove_asset(tg_store *store, const char *path,
	tg_diagnostic *diagnostic);

/* Borrow the absolute state directory until store_close. NULL returns NULL. */
const char *tg_store_path(const tg_store *store);

/* Release the lifetime lock and owned handle. NULL is harmless. */
void tg_store_close(tg_store *store);
#endif
