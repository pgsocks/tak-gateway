#include "../unit/contract_support.hpp"
#include <cucumber-cpp/autodetect.hpp>

using namespace contract;
using cucumber::ScenarioScope;

namespace {

struct GatewayContext {
	Framer framer;
	Session session;
	Delivery delivery;
	Config config;
	Buffer output;
	Message datagram;
	std::string xml = fixtures::extended_xml();
	fixtures::Bytes input;
	fixtures::Bytes event_only;
	std::vector<fixtures::Bytes> events;
	tg_encoding encoding = TG_ENCODING_XML;
	tg_status result = TG_UNIMPLEMENTED;
	tg_status expected_error = TG_MALFORMED;
	tg_normalize_report normalized{};
	tg_publish_report published{};
	tg_diagnostic diagnostic{};
	tg_output_id first = 0;
	tg_output_id second = 0;
	int truncated = 0;
	std::string first_record;
	std::string second_record;
	std::string config_text;

	void create_session(tg_protocol_preference preference,
		bool authenticated = false)
	{
		tg_session_options options = session_options(
			TG_PROTOCOL_NEGOTIATED, preference);

		if (authenticated) {
			options.username = "fixture-user";
			options.password = "fixture-secret";
		}
		ASSERT_EQ(tg_session_create(&options, nullptr, 0,
			&session.value), TG_OK);
	}

	void create_delivery()
	{
		if (delivery.value)
			return;
		ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
			&delivery.value), TG_OK);
	}

	void add_stream(tg_output_id *id, size_t records = 0)
	{
		create_delivery();
		ASSERT_NE(delivery.value, nullptr);
		tg_output_options options = stream_output();

		options.queue_records = records;
		ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, id),
			TG_OK);
	}

	std::string event_xml()
	{
		if (output.value.size)
			return string(output.value);
		if (events.size() != 1)
			return {};
		if (encoding == TG_ENCODING_XML)
			return {events[0].begin(), events[0].end()};
		result = tg_normalize(encoding, view(events[0]),
			TG_PRESERVE_OPAQUE, nullptr, &output.value,
			&normalized);
		EXPECT_EQ(result, TG_OK);
		return string(output.value);
	}
};

GIVEN("^a complete XML event with unknown namespaced fields and CDATA$")
{
	ScenarioScope<GatewayContext> c;

	c->xml = fixtures::extended_xml();
	c->input = fixtures::bytes(c->xml);
	c->encoding = TG_ENCODING_XML;
}

WHEN("^the XML stream is received one byte at a time$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr,
		&c->framer.value), TG_OK);
	for (size_t i = 0; i < c->input.size(); ++i) {
		ASSERT_GE(frame_all(c->framer.value, {c->input.data() + i, 1},
			c->events), TG_OK);
	}
	EXPECT_EQ(tg_framer_finish(c->framer.value), TG_OK);
}

THEN("^one event contains the exact original XML bytes$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->event_xml(), c->xml);
}

GIVEN("^two complete XML events with different identifiers$")
{
	ScenarioScope<GatewayContext> c;

	c->input = fixtures::bytes(fixtures::with_uid("first") +
		fixtures::with_uid("second"));
}

WHEN("^both documents arrive in one stream read$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr,
		&c->framer.value), TG_OK);
	ASSERT_GE(frame_all(c->framer.value, view(c->input), c->events),
		TG_OK);
}

THEN("^both events are emitted separately in their original order$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(c->events.size(), 2u);
	EXPECT_EQ(c->events[0], fixtures::bytes(fixtures::with_uid("first")));
	EXPECT_EQ(c->events[1], fixtures::bytes(fixtures::with_uid("second")));
}

WHEN("^the stream ends before the final XML byte$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr,
		&c->framer.value), TG_OK);
	ASSERT_EQ(frame_all(c->framer.value,
		{c->input.data(), c->input.size() - 1}, c->events), TG_MORE);
	c->result = tg_framer_finish(c->framer.value);
}

THEN("^no partial event is emitted and truncation is reported$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_TRUE(c->events.empty());
	EXPECT_EQ(c->result, TG_TRUNCATED);
}

GIVEN("^a UDP datagram containing (two XML documents|an unknown TAK version|"
	"a malformed TAK header|an incomplete XML document|"
	"a receive truncation flag)$")
{
	REGEX_PARAM(std::string, kind);
	ScenarioScope<GatewayContext> c;

	if (kind == "two XML documents")
		c->input = fixtures::bytes(c->xml + c->xml);
	if (kind == "an unknown TAK version") {
		c->input = fixtures::mesh(fixtures::envelope(), 2);
		c->expected_error = TG_UNSUPPORTED;
	}
	if (kind == "a malformed TAK header")
		c->input = {0xbf, 1, 0xbe, 0x12};
	if (kind == "an incomplete XML document") {
		c->input = fixtures::bytes(c->xml.substr(0, c->xml.size() - 1));
		c->expected_error = TG_TRUNCATED;
	}
	if (kind == "a receive truncation flag") {
		c->input = fixtures::bytes(c->xml);
		c->truncated = 1;
		c->expected_error = TG_TRUNCATED;
	}
}

WHEN("^the complete datagram is decoded$")
{
	ScenarioScope<GatewayContext> c;

	c->result = tg_datagram_decode(TG_DATAGRAM_AUTO, view(c->input),
		c->truncated, nullptr, &c->datagram.value);
}

THEN("^the datagram is rejected without an event$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->result, c->expected_error);
	EXPECT_EQ(c->datagram.value.bytes.data, nullptr);
	EXPECT_EQ(c->datagram.value.bytes.size, 0u);
}

GIVEN("^a TAK v1 mesh datagram carrying an event$")
{
	ScenarioScope<GatewayContext> c;

	c->event_only = fixtures::envelope();
	c->input = fixtures::mesh(c->event_only);
}

THEN("^version 1 and the exact unframed Protobuf envelope are returned$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(c->result, TG_OK);
	EXPECT_EQ(c->datagram.value.encoding, TG_ENCODING_PROTOBUF_V1);
	EXPECT_EQ(fixtures::Bytes(c->datagram.value.bytes.data,
		c->datagram.value.bytes.data + c->datagram.value.bytes.size),
		c->event_only);
}

WHEN("^the event is normalized in preserve-opaque mode$")
{
	ScenarioScope<GatewayContext> c;

	c->result = tg_normalize(c->encoding, view(c->input),
		TG_PRESERVE_OPAQUE, nullptr, &c->output.value, &c->normalized);
	ASSERT_EQ(c->result, TG_OK);
}

GIVEN("^a control-only Protobuf envelope$")
{
	ScenarioScope<GatewayContext> c;

	c->input = fixtures::control();
	c->encoding = TG_ENCODING_PROTOBUF_V1;
}

GIVEN("^a Protobuf envelope containing control information and an event$")
{
	ScenarioScope<GatewayContext> c;

	c->event_only = fixtures::envelope();
	c->input = fixtures::combined(c->event_only);
	c->encoding = TG_ENCODING_PROTOBUF_V1;
}

WHEN("^the envelope is normalized in (preserve-opaque|reject) mode$")
{
	REGEX_PARAM(std::string, policy);
	ScenarioScope<GatewayContext> c;

	c->result = tg_normalize(c->encoding, view(c->input),
		policy == "reject" ? TG_REJECT_LOSSY : TG_PRESERVE_OPAQUE,
		nullptr, &c->output.value, &c->normalized);
}

THEN("^no application event is returned$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->result, TG_NO_EVENT);
	EXPECT_EQ(c->output.value.data, nullptr);
	EXPECT_EQ(c->output.value.size, 0u);
}

THEN("^one XML event has identifier \"([^\"]+)\"$")
{
	REGEX_PARAM(std::string, uid);
	ScenarioScope<GatewayContext> c;
	std::string xml = c->event_xml();

	ASSERT_FALSE(xml.empty());
	EXPECT_TRUE(xml.find("uid=\"" + uid + "\"") != std::string::npos ||
		xml.find("uid='" + uid + "'") != std::string::npos);
}

THEN("^no TAK control bytes appear in the preservation extension$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->normalized.had_control, 1);
	if (c->normalized.preserved_opaque) {
		EXPECT_NE(string(c->output.value).find(
			fixtures::base64(c->event_only)), std::string::npos);
		EXPECT_EQ(string(c->output.value).find(
			fixtures::base64(c->input)), std::string::npos);
	}
}

GIVEN("^an event envelope with (an unknown event field|"
	"an unknown nested field|top-level server timestamps)$")
{
	REGEX_PARAM(std::string, kind);
	ScenarioScope<GatewayContext> c;

	if (kind == "an unknown event field")
		c->event_only = fixtures::unknown_event_envelope();
	if (kind == "an unknown nested field")
		c->event_only = fixtures::unknown_nested_envelope();
	if (kind == "top-level server timestamps")
		c->event_only = fixtures::timestamp_envelope();
	c->input = fixtures::combined(c->event_only);
	c->encoding = TG_ENCODING_PROTOBUF_V1;
}

THEN("^the preservation extension contains the exact event-only envelope$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(c->result, TG_OK);
	EXPECT_EQ(c->normalized.preserved_opaque, 1);
	EXPECT_NE(string(c->output.value).find(
		"urn:tak-gateway:preservation:1"), std::string::npos);
	fixtures::Bytes recovered;

	ASSERT_TRUE(fixtures::preserved_envelope(string(c->output.value),
		recovered));
	EXPECT_EQ(recovered, c->event_only);
	EXPECT_EQ(string(c->output.value).find(fixtures::base64(c->input)),
		std::string::npos);
}

THEN("^conversion is rejected without partial XML$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->result, TG_LOSSY);
	EXPECT_EQ(c->output.value.data, nullptr);
	EXPECT_EQ(c->output.value.size, 0u);
}

GIVEN("^an XML application event whose type starts with \"t-\"$")
{
	ScenarioScope<GatewayContext> c;

	c->xml = fixtures::control_xml("t-x-m-n", "<mission name='fixture'/>");
	c->input = fixtures::bytes(c->xml);
	c->encoding = TG_ENCODING_XML;
}

GIVEN("^a negotiated session preferring (XML|Protobuf)$")
{
	REGEX_PARAM(std::string, preference);
	ScenarioScope<GatewayContext> c;

	c->create_session(preference == "XML" ?
		TG_PREFER_XML : TG_PREFER_PROTOBUF);
}

GIVEN("^a password-authenticated session preferring Protobuf$")
{
	ScenarioScope<GatewayContext> c;

	c->create_session(TG_PREFER_PROTOBUF, true);
}

WHEN("^a supported-version announcement is received$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_GE(receive_all(c->session.value, view(fixtures::offer()), 1,
		c->events), TG_OK);
	EXPECT_TRUE(c->events.empty());
}

THEN("^no upgrade request is queued$")
{
	ScenarioScope<GatewayContext> c;
	tg_bytes write{};

	EXPECT_EQ(tg_session_peek_write(c->session.value, &write), TG_MORE);
	EXPECT_EQ(write.size, 0u);
}

THEN("^the session remains in XML mode$")
{
	ScenarioScope<GatewayContext> c;
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(c->session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
}

WHEN("^the complete upgrade request has been written$")
{
	ScenarioScope<GatewayContext> c;
	std::string written;

	ASSERT_EQ(write_all(c->session.value, 2, written), TG_OK);
	EXPECT_NE(written.find("TakRequest"), std::string::npos);
	EXPECT_NE(written.find("transaction-1"), std::string::npos);
}

WHEN("^acceptance and a binary event arrive in the same read$")
{
	ScenarioScope<GatewayContext> c;
	fixtures::Bytes input = fixtures::bytes(fixtures::response(true));
	fixtures::Bytes frame = fixtures::stream(fixtures::envelope());

	input.insert(input.end(), frame.begin(), frame.end());
	ASSERT_GE(receive_all(c->session.value, view(input), 3, c->events),
		TG_OK);
	c->encoding = TG_ENCODING_PROTOBUF_V1;
}

THEN("^the session is in Protobuf mode$")
{
	ScenarioScope<GatewayContext> c;
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(c->session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_PROTOBUF);
}

WHEN("^an ordinary XML event arrives before acceptance$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_GE(receive_all(c->session.value, view(c->xml), 3, c->events),
		TG_OK);
}

WHEN("^the server rejects the upgrade$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_GE(receive_all(c->session.value, view(fixtures::response(false)),
		3, c->events), TG_OK);
}

WHEN("^the response deadline expires$")
{
	ScenarioScope<GatewayContext> c;

	c->result = tg_session_tick(c->session.value, 65002, 0);
}

THEN("^the session requires a connection close$")
{
	ScenarioScope<GatewayContext> c;
	tg_session_info info{};

	EXPECT_EQ(c->result, TG_TIMEOUT);
	ASSERT_EQ(tg_session_snapshot(c->session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_FAILED);
}

THEN("^the first upstream document contains the credential preamble$")
{
	ScenarioScope<GatewayContext> c;
	tg_bytes write{};

	ASSERT_EQ(tg_session_peek_write(c->session.value, &write), TG_OK);
	std::string preamble(reinterpret_cast<const char *>(write.data),
		write.size);

	EXPECT_NE(preamble.find("<auth"), std::string::npos);
	EXPECT_NE(preamble.find("fixture-user"), std::string::npos);
	EXPECT_EQ(preamble.find("TakRequest"), std::string::npos);
}

THEN("^the credential preamble is never emitted as an application event$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_TRUE(c->events.empty());
}

WHEN("^part of an XML event arrives before a reconnect$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(receive_all(c->session.value, view(c->xml.substr(0, 25)),
		1, c->events), TG_MORE);
	ASSERT_EQ(tg_session_reset(c->session.value, 2), TG_OK);
}

WHEN("^a fresh complete XML event arrives after reset$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_GE(receive_all(c->session.value, view(c->xml), 3, c->events),
		TG_OK);
}

GIVEN("^two healthy outputs with u32be framing$")
{
	ScenarioScope<GatewayContext> c;

	c->add_stream(&c->first);
	c->add_stream(&c->second);
}

WHEN("^two identical XML events are published$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(tg_delivery_publish(c->delivery.value, view(c->xml),
		&c->published), TG_OK);
	ASSERT_EQ(tg_delivery_publish(c->delivery.value, view(c->xml),
		&c->published), TG_OK);
}

THEN("^both outputs expose both complete records in admission order$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(drain(c->delivery.value, c->first, c->first_record), TG_OK);
	ASSERT_EQ(drain(c->delivery.value, c->second, c->second_record), TG_OK);
	EXPECT_EQ(c->first_record, record(c->xml) + record(c->xml));
	EXPECT_EQ(c->second_record, c->first_record);
}

GIVEN("^one full output and one healthy output$")
{
	ScenarioScope<GatewayContext> c;

	c->add_stream(&c->first, 1);
	c->add_stream(&c->second);
	ASSERT_EQ(tg_delivery_publish(c->delivery.value, view(c->xml),
		&c->published), TG_OK);
	std::string discarded;

	ASSERT_EQ(drain(c->delivery.value, c->second, discarded), TG_OK);
}

WHEN("^a new XML event is published with drop-newest policy$")
{
	ScenarioScope<GatewayContext> c;

	c->xml = fixtures::with_uid("new-event");
	c->result = tg_delivery_publish(c->delivery.value, view(c->xml),
		&c->published);
}

THEN("^the full output reports a local drop$")
{
	ScenarioScope<GatewayContext> c;
	tg_delivery_info info{};

	EXPECT_EQ(c->result, TG_DROPPED);
	EXPECT_EQ(c->published.accepted_outputs, 1u);
	EXPECT_EQ(c->published.dropped_outputs, 1u);
	ASSERT_EQ(tg_delivery_snapshot(c->delivery.value, c->first, &info),
		TG_OK);
	EXPECT_EQ(info.records_dropped, 1u);
}

THEN("^the healthy output exposes the new complete record$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(drain(c->delivery.value, c->second, c->second_record), TG_OK);
	EXPECT_EQ(c->second_record, record(c->xml));
}

GIVEN("^one healthy output with u32be framing$")
{
	ScenarioScope<GatewayContext> c;

	c->add_stream(&c->first);
}

WHEN("^two XML events are published and consumed one byte at a time$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(tg_delivery_publish(c->delivery.value, view(c->xml),
		&c->published), TG_OK);
	ASSERT_EQ(tg_delivery_publish(c->delivery.value,
		view(fixtures::with_uid("second")), &c->published), TG_OK);
	ASSERT_EQ(drain(c->delivery.value, c->first, c->first_record, true),
		TG_OK);
}

THEN("^the collected stream contains two exact length-prefixed XML records$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->first_record, record(c->xml) +
		record(fixtures::with_uid("second")));
}

WHEN("^one XML event is published$")
{
	ScenarioScope<GatewayContext> c;

	c->result = tg_delivery_publish(c->delivery.value, view(c->xml),
		&c->published);
	ASSERT_GE(c->result, TG_OK);
}

THEN("^its record contains a four-byte big-endian byte length$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(drain(c->delivery.value, c->first, c->first_record), TG_OK);
	ASSERT_GE(c->first_record.size(), 4u);
	uint32_t length = 0;

	for (size_t i = 0; i < 4; ++i)
		length = (length << 8) |
			static_cast<uint8_t>(c->first_record[i]);
	EXPECT_EQ(length, c->xml.size());
}

THEN("^the record ends immediately after the XML document$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->first_record, record(c->xml));
}

GIVEN("^a UDP output whose payload limit is smaller than the XML event$")
{
	ScenarioScope<GatewayContext> c;

	c->create_delivery();
	ASSERT_NE(c->delivery.value, nullptr);
	tg_output_options options = {TG_OUTPUT_DATAGRAM,
		TG_FRAMING_XML_DOCUMENT, c->xml.size() - 1, 0, 0};

	ASSERT_EQ(tg_delivery_add_output(c->delivery.value, &options,
		&c->first), TG_OK);
}

GIVEN("^a healthy stream output$")
{
	ScenarioScope<GatewayContext> c;

	c->add_stream(&c->second);
}

THEN("^the UDP output reports oversize without partial delivery$")
{
	ScenarioScope<GatewayContext> c;
	tg_delivery_info info{};
	tg_bytes bytes{};

	EXPECT_EQ(c->result, TG_DROPPED);
	ASSERT_EQ(tg_delivery_snapshot(c->delivery.value, c->first, &info),
		TG_OK);
	EXPECT_EQ(info.udp_oversize, 1u);
	EXPECT_EQ(tg_delivery_peek(c->delivery.value, c->first, &bytes),
		TG_MORE);
	EXPECT_EQ(bytes.size, 0u);
}

GIVEN("^a configuration with a UDP input and TCP listener output$")
{
	ScenarioScope<GatewayContext> c;

	c->config_text = fixtures::minimal_config();
}

GIVEN("^a configuration with (an unknown top-level key|a zero input port|"
	"an unsupported ingress mode|duplicate input identifiers|"
	"an impossible queue limit|an obvious input-output loop)$")
{
	REGEX_PARAM(std::string, defect);
	ScenarioScope<GatewayContext> c;

	c->config_text = fixtures::minimal_config();
	if (defect == "an unknown top-level key")
		c->config_text.insert(0, "surprise = true\n");
	if (defect == "a zero input port") {
		size_t port = c->config_text.find("18087");

		c->config_text.replace(port, 5, "0");
	}
	if (defect == "an unsupported ingress mode") {
		size_t kind = c->config_text.find("\"udp\"");

		c->config_text.replace(kind, 5, "\"grpc\"");
	}
	if (defect == "duplicate input identifiers") {
		c->config_text += "[[inputs]]\nid = \"udp-source\"\n"
			"kind = \"udp\"\nbind = \"127.0.0.1\"\n"
			"port = 18088\nencoding = \"xml\"\n";
	}
	if (defect == "an impossible queue limit")
		c->config_text += "[limits]\nqueue_records = 0\n";
	if (defect == "an obvious input-output loop") {
		c->config_text += "[[outputs]]\nid = \"loop\"\n"
			"kind = \"udp\"\nhost = \"127.0.0.1\"\n"
			"port = 18087\nmax_datagram_bytes = 65507\n";
	}
}

WHEN("^the configuration is parsed$")
{
	ScenarioScope<GatewayContext> c;

	c->result = tg_config_parse(view(c->config_text), base_directory(),
		&c->config.value, &c->diagnostic);
}

THEN("^the configuration is accepted$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->result, TG_OK);
	EXPECT_NE(c->config.value, nullptr);
	EXPECT_EQ(c->diagnostic.key[0], '\0');
}

THEN("^the configuration is rejected with a key-specific diagnostic$")
{
	ScenarioScope<GatewayContext> c;

	EXPECT_EQ(c->result, TG_INVALID);
	EXPECT_EQ(c->config.value, nullptr);
	EXPECT_NE(c->diagnostic.key[0], '\0');
}

GIVEN("^a configuration with password-file authentication$")
{
	ScenarioScope<GatewayContext> c;

	c->config_text = "version = 1\n[[inputs]]\nid = \"server\"\n"
		"kind = \"tls\"\nhost = \"tak.example.org\"\nport = 8089\n"
		"protocol = \"negotiated\"\nuid = \"gateway-fixture\"\n"
		"[inputs.tls]\nca_file = \"public-ca.pem\"\n"
		"[inputs.auth]\nmode = \"password\"\nusername = \"reader\"\n"
		"password_file = \"private-password-fixture\"\n"
		"[[outputs]]\nid = \"out\"\nkind = \"tcp\"\n"
		"mode = \"listen\"\nbind = \"127.0.0.1\"\nport = 19000\n"
		"framing = \"u32be\"\n";
	ASSERT_EQ(tg_config_parse(view(c->config_text), base_directory(),
		&c->config.value, &c->diagnostic), TG_OK);
}

WHEN("^its resolved redacted representation is requested$")
{
	ScenarioScope<GatewayContext> c;

	c->result = tg_config_redacted(c->config.value, &c->output.value,
		&c->diagnostic);
}

THEN("^the representation contains no password or private-key material$")
{
	ScenarioScope<GatewayContext> c;

	ASSERT_EQ(c->result, TG_OK);
	std::string text = string(c->output.value);

	EXPECT_EQ(text.find("private-password-fixture"), std::string::npos);
	EXPECT_NE(text.find("<redacted>"), std::string::npos);
}

} /* namespace */
