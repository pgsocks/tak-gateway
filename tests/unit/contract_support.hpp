#ifndef TAK_GATEWAY_CONTRACT_SUPPORT_HPP
#define TAK_GATEWAY_CONTRACT_SUPPORT_HPP

#include "contract_fixtures.hpp"
#include <tak_gateway/gateway.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

namespace contract {

inline const char *base_directory()
{
#ifdef _WIN32
	return "C:\\";
#else
	return "/tmp";
#endif
}

inline tg_bytes view(const fixtures::Bytes &value)
{
	return {value.data(), value.size()};
}

inline tg_bytes view(const std::string &value)
{
	return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}

inline std::string string(const tg_buffer &value)
{
	if (!value.size)
		return {};
	return {reinterpret_cast<const char *>(value.data), value.size};
}

struct Buffer {
	tg_buffer value{};
	~Buffer() { tg_buffer_release(&value); }
	Buffer() = default;
	Buffer(const Buffer &) = delete;
	Buffer &operator=(const Buffer &) = delete;
};

struct Message {
	tg_message value{};
	~Message() { tg_buffer_release(&value.bytes); }
};

struct Framer {
	tg_framer *value = nullptr;
	~Framer() { tg_framer_destroy(value); }
};

struct Session {
	tg_session *value = nullptr;
	~Session() { tg_session_destroy(value); }
};

struct Delivery {
	tg_delivery *value = nullptr;
	~Delivery() { tg_delivery_destroy(value); }
};

struct Config {
	tg_config *value = nullptr;
	~Config() { tg_config_destroy(value); }
};

struct Runtime {
	tg_runtime *value = nullptr;
	~Runtime() { tg_runtime_destroy(value); }
};

inline tg_session_options session_options(
	tg_protocol_mode mode = TG_PROTOCOL_NEGOTIATED,
	tg_protocol_preference prefer = TG_PREFER_XML)
{
	return {mode, prefer, 1, 65000, 30000,
		"gateway-source-1", nullptr, nullptr, nullptr, 0, 0,
		{nullptr, 0}};
}

inline tg_output_options stream_output()
{
	return {TG_OUTPUT_STREAM, TG_FRAMING_U32BE, 0, 0, 0};
}

inline tg_status frame_all(tg_framer *framer, tg_bytes input,
	std::vector<fixtures::Bytes> &out)
{
	size_t offset = 0;
	tg_status result = TG_MORE;

	while (offset < input.size) {
		Message message;
		size_t consumed = 0;

		result = tg_framer_feed(framer,
			{input.data + offset, input.size - offset},
			&consumed, &message.value);
		if (result < 0)
			return result;
		if (consumed == 0 || consumed > input.size - offset)
			return TG_BAD_STATE;
		offset += consumed;
		if (result == TG_OK) {
			out.emplace_back(message.value.bytes.data,
				message.value.bytes.data +
				message.value.bytes.size);
		}
	}
	return result;
}

inline tg_status receive_all(tg_session *session, tg_bytes input,
	uint64_t now, std::vector<fixtures::Bytes> &out)
{
	size_t offset = 0;
	tg_status result = TG_MORE;

	while (offset < input.size) {
		Message message;
		size_t consumed = 0;

		result = tg_session_receive(session,
			{input.data + offset, input.size - offset}, now,
			&consumed, &message.value);
		if (result < 0)
			return result;
		if (consumed == 0 || consumed > input.size - offset)
			return TG_BAD_STATE;
		offset += consumed;
		if (result == TG_OK) {
			out.emplace_back(message.value.bytes.data,
				message.value.bytes.data +
				message.value.bytes.size);
		}
	}
	return result;
}

inline tg_status write_all(tg_session *session, uint64_t now,
	std::string &out, bool single_byte = false)
{
	for (size_t work = 0; work < 65536; ++work) {
		tg_bytes span{};
		tg_status result = tg_session_peek_write(session, &span);

		if (result == TG_MORE)
			return TG_OK;
		if (result != TG_OK || span.size == 0)
			return result == TG_OK ? TG_BAD_STATE : result;
		size_t count = single_byte ? 1 : span.size;

		out.append(reinterpret_cast<const char *>(span.data), count);
		result = tg_session_consume_write(session, count, now);
		if (result != TG_OK)
			return result;
	}
	return TG_LIMIT;
}

inline std::string record(const std::string &xml)
{
	uint32_t size = static_cast<uint32_t>(xml.size());
	std::string result;

	for (int shift = 24; shift >= 0; shift -= 8)
		result.push_back(static_cast<char>(size >> shift));
	result += xml;
	return result;
}

inline tg_status drain(tg_delivery *delivery, tg_output_id id,
	std::string &out, bool single_byte = false)
{
	for (size_t work = 0; work < 1048576; ++work) {
		tg_bytes span{};
		tg_status result = tg_delivery_peek(delivery, id, &span);

		if (result == TG_MORE)
			return TG_OK;
		if (result != TG_OK || span.size == 0)
			return result == TG_OK ? TG_BAD_STATE : result;
		size_t count = single_byte ? 1 : span.size;

		out.append(reinterpret_cast<const char *>(span.data), count);
		result = tg_delivery_consume(delivery, id, count);
		if (result != TG_OK)
			return result;
	}
	return TG_LIMIT;
}

} /* namespace contract */

#endif
