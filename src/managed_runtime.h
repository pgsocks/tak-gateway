#ifndef TG_MANAGED_RUNTIME_H
#define TG_MANAGED_RUNTIME_H
#include "config_internal.h"

/* Parse the regular schema while allowing empty/disabled input and output
 * collections. All other shape, ID, bounds and loop checks remain strict.
 * Ownership, diagnostics and filesystem-free behavior match config_parse.
 */
tg_status tg_config_parse_managed(tg_bytes toml, const char *base,
	tg_config **out, tg_diagnostic *diagnostic);

/* Serialize the complete resolved schema, including secret-reference paths
 * and environment names. No secret file/environment contents are read.
 * out must be empty; caller releases it with tg_buffer_release. This is for
 * protected local persistence, never an HTTP response or a log.
 */
tg_status tg_config_export(const tg_config *config, tg_buffer *out,
	tg_diagnostic *diagnostic);

/* Create a READY runtime which accepts empty configurations and isolates
 * endpoint credential/bind failures in its status rather than aborting other
 * endpoints. Global resource failures still fail creation/poll. Copies config.
 */
tg_status tg_runtime_create_managed(const tg_config *config, tg_runtime **out,
	tg_diagnostic *diagnostic);

/* Apply an owned copy of candidate to a READY/RUNNING managed runtime. Calls
 * must be serialized with poll/snapshot. Match endpoints by stable ID;
 * preserve unchanged sockets, queued delivery and counters. Replace/remove
 * only changed endpoints and their downstream clients. Metadata name changes
 * do not reconnect. Global settings must remain identical. Shape/global/OOM
 * failure leaves the runtime unchanged. Local endpoint failures are accepted
 * and reported per endpoint. No connection success is implied by TG_OK.
 */
tg_status tg_runtime_apply_config(tg_runtime *runtime,
	const tg_config *candidate, tg_diagnostic *diagnostic);
#endif
