#ifndef TG_WEB_SERVER_H
#define TG_WEB_SERVER_H
#include "store.h"

struct tg_web_app;

/*
 * Create the management controller and its managed runtime from a copied
 * configuration. store must remain exclusively locked and alive until app
 * destruction. HTTP starts only when web.enabled is true. The caller owns
 * polling, stop and destruction; HTTP workers never call the runtime.
 * out must initially be NULL. Failure leaves it NULL and releases resources.
 * No credential contents or secret-reference values appear in API responses.
 */
tg_status tg_web_app_create(tg_store *store, const tg_config *initial,
	const tg_web_settings *web, uint64_t revision, tg_web_app **out,
	tg_diagnostic *diagnostic);

/* Drive bounded queued management operations and ordinary runtime polling. */
tg_status tg_web_app_poll(tg_web_app *app, uint32_t max_wait_ms);

/* Stop intake and drain runtime outputs; safe to call repeatedly. */
tg_status tg_web_app_stop(tg_web_app *app, uint32_t drain_ms);

/* Stop workers and HTTP; release owned runtime/config. NULL is harmless. */
void tg_web_app_destroy(tg_web_app *app);
#endif
