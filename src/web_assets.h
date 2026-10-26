#ifndef TG_WEB_ASSETS_H
#define TG_WEB_ASSETS_H
#include "internal.h"

/* Borrow immutable embedded asset bytes for an exact HTTP path. */
bool tg_web_asset(const char *path, tg_bytes *bytes, const char **mime);
#endif
