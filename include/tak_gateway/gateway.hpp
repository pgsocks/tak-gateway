#ifndef TAK_GATEWAY_GATEWAY_HPP
#define TAK_GATEWAY_GATEWAY_HPP

#include <tak_gateway/gateway.h>

/*
 * Procedural C++17 view of the same C contract. These imports introduce no
 * wrappers, methods, templates, exception policy, or second resource model.
 * All tg_* enum constants remain available from the included C header.
 */
namespace tak_gateway {
using ::tg_status;
using ::tg_bytes;
using ::tg_buffer;
using ::tg_buffer_release;
using ::tg_encoding;
using ::tg_message;
using ::tg_limits;
using ::tg_framer;
using ::tg_framer_create;
using ::tg_framer_feed;
using ::tg_framer_finish;
using ::tg_framer_reset;
using ::tg_framer_destroy;
using ::tg_datagram_mode;
using ::tg_datagram_decode;
using ::tg_preservation_policy;
using ::tg_normalize_report;
using ::tg_normalize;
using ::tg_protocol_mode;
using ::tg_protocol_preference;
using ::tg_session_options;
using ::tg_session_state;
using ::tg_session_info;
using ::tg_session;
using ::tg_session_create;
using ::tg_session_receive;
using ::tg_session_peek_write;
using ::tg_session_consume_write;
using ::tg_session_tick;
using ::tg_session_reset;
using ::tg_session_snapshot;
using ::tg_session_destroy;
using ::tg_output_framing;
using ::tg_output_kind;
using ::tg_overflow_policy;
using ::tg_output_options;
using ::tg_output_id;
using ::tg_publish_report;
using ::tg_delivery_info;
using ::tg_delivery;
using ::tg_delivery_create;
using ::tg_delivery_add_output;
using ::tg_delivery_remove_output;
using ::tg_delivery_publish;
using ::tg_delivery_peek;
using ::tg_delivery_consume;
using ::tg_delivery_snapshot;
using ::tg_delivery_destroy;
using ::tg_diagnostic;
using ::tg_config;
using ::tg_config_parse;
using ::tg_config_validate;
using ::tg_config_redacted;
using ::tg_config_destroy;
using ::tg_runtime_state;
using ::tg_runtime;
using ::tg_runtime_create;
using ::tg_runtime_poll;
using ::tg_runtime_stop;
using ::tg_runtime_snapshot;
using ::tg_runtime_destroy;
}

#endif
