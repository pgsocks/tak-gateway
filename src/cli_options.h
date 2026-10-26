#ifndef TG_CLI_OPTIONS_H
#define TG_CLI_OPTIONS_H
#include "import_zip.h"
#include "store.h"

/* Overlay a saved gateway document (or version=1 when empty) using ordinary
 * CLI configuration arguments. Explicit --config replaces gateway bytes.
 * web is input/output: start with saved/default settings, preserve omitted
 * fields, then apply file [web] values and --web-bind/--set web.* in order.
 * --web-bind enables the UI. managed permits empty/all-disabled endpoints.
 * In managed mode, imported IDs colliding with existing inputs are replaced
 * with the next free atak-N ID; the matching generated UID changes with it.
 * Existing profiles and noncolliding imported IDs keep their order/identity.
 * Otherwise preserve configure's ownership, paths, bounds and error rules.
 * Web settings remain unchanged on failure. Does not read/write the store.
 */
tg_status tg_cli_compose(tg_bytes base, int argc, const char *const *argv,
	const char *cwd, bool managed, tg_config **out, tg_import_files **files,
	tg_web_settings *web, tg_diagnostic *diagnostic);

/*
 * Compose the foreground CLI configuration from argc option/value arguments
 * (without executable or command), using the absolute cwd as the path base.
 * out and files must initially point to NULL handles. diagnostic is optional.
 *
 * --config and --import-zip are optional and may occur once each. Load TOML
 * first (or version=1 defaults), append imported inputs, then apply all other
 * options in command order. Scalar setters and --set use last-wins semantics.
 * --output ID=URI replaces an existing output with that ID or appends one;
 * repeated matching IDs replace the whole output in order. Supported schemes:
 * tcp://BIND:PORT, tcp-connect://HOST:PORT, udp://HOST:PORT, pipe:///PATH.
 * IPv6 hosts require brackets. URI credentials, queries, fragments and percent
 * escapes are unsupported. pipe URIs accept literal POSIX absolute paths;
 * native Windows pipe paths can be set through the general --set option.
 * Output replacement starts from schema defaults; advanced fields can follow
 * through --set outputs[INDEX].FIELD=TOML_VALUE.
 *
 * --set PATH=TOML_VALUE addresses root/dotted keys and zero-based array
 * indices. It replaces the complete addressed value and creates missing
 * tables/arrays. An array can be extended only at its next index, not sparsely.
 * The final merged document must satisfy the regular version-1 schema.
 * Scalar flags cover process, global limits, normalization, and overflow.
 *
 * With --config, all final relative setting paths resolve beside that file;
 * otherwise they resolve against cwd. --config and ZIP filenames themselves
 * resolve against cwd. Does not change cwd or perform network access.
 * Reads at most 8 MiB TOML, accepts 512 option/value pairs, at most 8192 tree
 * nodes and depth 32. Errors publish neither output and remove imported files.
 * Success transfers an owned config and optional imported-files handle. Keep
 * imported files until all runtimes are destroyed, then release both through
 * tg_config_destroy/tg_import_zip_destroy. Only schema parsing is performed;
 * the caller still validates local files before creating a runtime.
 */
tg_status tg_cli_configure(int argc, const char *const *argv, const char *cwd,
	tg_config **out, tg_import_files **files, tg_diagnostic *diagnostic);
#endif
