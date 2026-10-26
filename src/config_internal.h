#ifndef TG_CONFIG_INTERNAL_H
#define TG_CONFIG_INTERNAL_H
#include "internal.h"
#include <toml.h>
struct config_block {
	void *memory;
	config_block *next;
};
struct tg_input_config {
	const char *id;
	const char *name;
	const char *kind;
	bool enabled;
	const char *host;
	const char *bind;
	const char *group;
	const char *interface_name;
	uint16_t port;
	tg_datagram_mode encoding;
	tg_session_options session;
	const char *ca_file;
	const char *cert_file;
	const char *key_file;
	const char *key_password_file;
	const char *key_password_env;
	const char *pkcs12_file;
	const char *pkcs12_password_file;
	const char *pkcs12_password_env;
	const char *server_name;
	const char *username;
	const char *password_file;
	const char *password_env;
	const char *identity_file;
	const char **allow_peers;
	size_t peer_count;
};
struct tg_output_config {
	const char *id;
	const char *name;
	const char *kind;
	bool enabled;
	const char *mode;
	const char *host;
	const char *bind;
	const char *path;
	uint16_t port;
	size_t max_clients;
	bool create;
	unsigned permissions;
	tg_output_options delivery;
};
struct tg_config {
	bool managed;
	toml_table_t *table;
	config_block *blocks;
	char *base;
	char *original;
	tg_limits limits;
	size_t max_inputs;
	tg_overflow_policy overflow;
	tg_preservation_policy preservation;
	const char *status_file;
	const char *log_format;
	const char *log_level;
	tg_input_config *inputs;
	size_t input_count;
	tg_output_config *outputs;
	size_t output_count;
};
uint64_t tg_monotonic_ms(void);
uint64_t tg_utc_ms(void);
tg_status tg_read_file(const char *, size_t, bool, tg_buffer *);
const char *tg_config_string(tg_config *, const char *);
#endif
