#ifndef TAK_GATEWAY_CONTRACT_FIXTURES_HPP
#define TAK_GATEWAY_CONTRACT_FIXTURES_HPP

#include <stdint.h>
#include <string.h>
#include <string>
#include <regex>
#include <vector>

namespace fixtures {

using Bytes = std::vector<uint8_t>;

inline const std::string &xml()
{
	static const std::string value =
		"<event version=\"2.0\" uid=\"fixture-1\" type=\"a-f-G-U-C\" "
		"time=\"2026-10-22T12:00:00.000Z\" "
		"start=\"2026-10-22T12:00:00.000Z\" "
		"stale=\"2026-10-22T12:05:00.000Z\" how=\"m-g\">"
		"<point lat=\"38\" lon=\"-77\" hae=\"12\" ce=\"3\" le=\"4\"/>"
		"<detail><contact callsign=\"Gateway fixture\"/></detail>"
		"</event>";

	return value;
}

inline std::string extended_xml()
{
	std::string value = xml();
	size_t offset = value.find("</detail>");

	value.insert(offset,
		"<x:unknown xmlns:x=\"urn:test:custom\" x:flag='yes'>"
		"<![CDATA[apparent </event> boundary]]>"
		"<!-- apparent </event> boundary -->"
		"<x:child>caf\xc3\xa9 &amp; snow \xe2\x98\x83</x:child>"
		"</x:unknown>");
	return value;
}

inline std::string with_uid(const std::string &uid)
{
	std::string value = xml();
	const std::string old = "fixture-1";

	value.replace(value.find(old), old.size(), uid);
	return value;
}

inline Bytes bytes(const std::string &value)
{
	return Bytes(value.begin(), value.end());
}

inline void varint(Bytes &out, uint64_t value)
{
	do {
		uint8_t octet = static_cast<uint8_t>(value & 0x7f);

		value >>= 7;
		out.push_back(static_cast<uint8_t>(octet |
			(value ? 0x80 : 0)));
	} while (value);
}

inline void integer(Bytes &out, unsigned field, uint64_t value)
{
	varint(out, static_cast<uint64_t>(field) << 3);
	varint(out, value);
}

inline void blob(Bytes &out, unsigned field, const Bytes &value)
{
	varint(out, (static_cast<uint64_t>(field) << 3) | 2);
	varint(out, value.size());
	out.insert(out.end(), value.begin(), value.end());
}

inline void text(Bytes &out, unsigned field, const std::string &value)
{
	blob(out, field, bytes(value));
}

inline void real(Bytes &out, unsigned field, double value)
{
	uint64_t bits;

	static_assert(sizeof(bits) == sizeof(value), "IEEE double required");
	memcpy(&bits, &value, sizeof(bits));
	varint(out, (static_cast<uint64_t>(field) << 3) | 1);
	for (unsigned i = 0; i != 8; ++i)
		out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

/*
 * Field numbers follow pinned official Server 5187abd... schemas:
 * src/takserver-protobuf/src/main/proto/{takmessage,cotevent,detail}.proto.
 * Composition here is independent of the gateway codec implementation.
 */
inline Bytes event(const Bytes &detail = {}, const Bytes &extra = {})
{
	Bytes result;

	text(result, 1, "a-f-G-U-C");
	text(result, 5, "fixture-1");
	integer(result, 6, UINT64_C(1792670400000));
	integer(result, 7, UINT64_C(1792670400000));
	integer(result, 8, UINT64_C(1792670700000));
	text(result, 9, "m-g");
	real(result, 10, 38);
	real(result, 11, -77);
	real(result, 12, 12);
	real(result, 13, 3);
	real(result, 14, 4);
	if (!detail.empty())
		blob(result, 15, detail);
	result.insert(result.end(), extra.begin(), extra.end());
	return result;
}

inline Bytes envelope(const Bytes &value = event())
{
	Bytes result;

	blob(result, 2, value);
	return result;
}

inline Bytes control()
{
	Bytes version;
	Bytes result;

	integer(version, 1, 1);
	integer(version, 2, 1);
	blob(result, 1, version);
	return result;
}

inline Bytes combined(const Bytes &value = envelope())
{
	Bytes result = control();

	result.insert(result.end(), value.begin(), value.end());
	return result;
}

inline Bytes unknown_event_envelope()
{
	Bytes extra;

	integer(extra, 100, 42);
	return envelope(event({}, extra));
}

inline Bytes unknown_nested_envelope()
{
	Bytes contact;
	Bytes detail;

	text(contact, 2, "Fixture");
	integer(contact, 101, 9);
	blob(detail, 2, contact);
	integer(detail, 102, 10);
	return envelope(event(detail));
}

inline Bytes timestamp_envelope()
{
	Bytes result = envelope();

	integer(result, 3, UINT64_C(1792670400123));
	integer(result, 4, UINT64_C(1792670400456));
	return result;
}

/* Detail field 8 follows pgsocks/cot's extended mapping (src/protobuf.cpp).
 * Pinned Server detail.proto ends at field 7; field 8 remains opaque there.
 */
inline Bytes binary_extension_envelope()
{
	Bytes extension;
	Bytes detail;

	integer(extension, 1, 7);
	blob(extension, 2, Bytes{0x00, 0xff, 0x80, 0x01});
	blob(detail, 8, extension);
	return envelope(event(detail));
}

inline Bytes conflicting_detail_envelope()
{
	Bytes detail;
	Bytes contact;

	text(detail, 1, "<contact callsign='residual'/>");
	text(contact, 2, "typed");
	blob(detail, 2, contact);
	return envelope(event(detail));
}

inline Bytes stream(const Bytes &payload)
{
	Bytes result = {0xbf};

	varint(result, payload.size());
	result.insert(result.end(), payload.begin(), payload.end());
	return result;
}

inline Bytes mesh(const Bytes &payload, uint64_t version = 1)
{
	Bytes result = {0xbf};

	varint(result, version);
	result.push_back(0xbf);
	result.insert(result.end(), payload.begin(), payload.end());
	return result;
}

inline std::string control_xml(const std::string &type,
	const std::string &detail, const std::string &uid = "transaction-1")
{
	std::string value = with_uid(uid);
	const std::string old = "a-f-G-U-C";

	value.replace(value.find(old), old.size(), type);
	size_t begin = value.find("<detail>") + strlen("<detail>");
	size_t end = value.find("</detail>");

	value.replace(begin, end - begin, detail);
	return value;
}

inline std::string offer()
{
	return control_xml("t-x-takp-v",
		"<TakControl><TakProtocolSupport version=\"1\"/>"
		"</TakControl>");
}

inline std::string response(bool accepted,
	const std::string &uid = "transaction-1")
{
	return control_xml("t-x-takp-r", accepted ?
		"<TakControl><TakResponse status=\"true\"/></TakControl>" :
		"<TakControl><TakResponse status=\"false\"/></TakControl>",
		uid);
}

inline std::string base64(const Bytes &value)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
		"0123456789+/";
	std::string out;

	for (size_t i = 0; i < value.size(); i += 3) {
		uint32_t bits = static_cast<uint32_t>(value[i]) << 16;
		size_t remain = value.size() - i;

		if (remain > 1)
			bits |= static_cast<uint32_t>(value[i + 1]) << 8;
		if (remain > 2)
			bits |= value[i + 2];
		out.push_back(alphabet[(bits >> 18) & 63]);
		out.push_back(alphabet[(bits >> 12) & 63]);
		out.push_back(remain > 1 ? alphabet[(bits >> 6) & 63] : '=');
		out.push_back(remain > 2 ? alphabet[bits & 63] : '=');
	}
	return out;
}

inline bool preserved_envelope(const std::string &xml, Bytes &out)
{
	if (xml.find("urn:tak-gateway:preservation:1") == std::string::npos)
		return false;
	std::regex element(
		"<((?:[A-Za-z_][A-Za-z0-9_.-]*:)?protobuf)"
		"[[:space:]][^>]*>([^<]*)</\\1[[:space:]]*>");
	std::smatch match;

	if (!std::regex_search(xml, match, element))
		return false;
	const std::string alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
		"0123456789+/";
	unsigned bits = 0;
	unsigned count = 0;
	bool padding = false;

	out.clear();
	for (unsigned char c : match[2].str()) {
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;
		if (c == '=') {
			padding = true;
			continue;
		}
		size_t index = alphabet.find(static_cast<char>(c));

		if (padding || index == std::string::npos)
			return false;
		bits = (bits << 6) | static_cast<unsigned>(index);
		count += 6;
		if (count >= 8) {
			count -= 8;
			out.push_back(static_cast<uint8_t>(bits >> count));
		}
	}
	return true;
}

inline std::string minimal_config()
{
	return "version = 1\n"
		"[[inputs]]\n"
		"id = \"udp-source\"\n"
		"kind = \"udp\"\n"
		"bind = \"127.0.0.1\"\n"
		"port = 18087\n"
		"encoding = \"auto\"\n"
		"[[outputs]]\n"
		"id = \"tcp-consumers\"\n"
		"kind = \"tcp\"\n"
		"mode = \"listen\"\n"
		"bind = \"127.0.0.1\"\n"
		"port = 19000\n"
		"framing = \"u32be\"\n";
}

} /* namespace fixtures */

#endif
