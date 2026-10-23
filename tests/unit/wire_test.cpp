#include "contract_support.hpp"
#include <limits>

using namespace contract;

TEST(XmlFraming, EverySplitPreservesExactExtendedDocument)
{
	std::string xml =
		"\xef\xbb\xbf<?xml version='1.0'?>" + fixtures::extended_xml();

	for (size_t split = 0; split <= xml.size(); ++split) {
		SCOPED_TRACE(split);
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(
				  TG_ENCODING_XML, nullptr, &framer.value),
			TG_OK);
		EXPECT_GE(frame_all(framer.value, view(xml.substr(0, split)),
				  out),
			TG_OK);
		EXPECT_GE(frame_all(framer.value, view(xml.substr(split)), out),
			TG_OK);
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], fixtures::bytes(xml));
		EXPECT_EQ(tg_framer_finish(framer.value), TG_OK);
	}
}

TEST(XmlFraming, OneByteReadsAndCoalescedDocumentsHaveSameBoundaries)
{
	std::string first = fixtures::extended_xml();
	std::string second = fixtures::with_uid("second");
	std::string stream = " \r\n" + first + "\t" + second + "\n";

	for (size_t chunk : {size_t{1}, stream.size()}) {
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(
				  TG_ENCODING_XML, nullptr, &framer.value),
			TG_OK);
		for (size_t offset = 0; offset < stream.size();
			offset += chunk) {
			EXPECT_GE(frame_all(framer.value,
					  view(stream.substr(offset, chunk)),
					  out),
				TG_OK);
		}
		ASSERT_EQ(out.size(), 2u);
		EXPECT_EQ(out[0], fixtures::bytes(first));
		EXPECT_EQ(out[1], fixtures::bytes(second));
		EXPECT_EQ(tg_framer_finish(framer.value), TG_OK);
	}
}

TEST(XmlFraming, EveryIncompletePrefixIsTruncatedAtEof)
{
	std::string xml = fixtures::xml();

	for (size_t prefix = 1; prefix < xml.size(); ++prefix) {
		SCOPED_TRACE(prefix);
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(
				  TG_ENCODING_XML, nullptr, &framer.value),
			TG_OK);
		EXPECT_EQ(frame_all(framer.value, view(xml.substr(0, prefix)),
				  out),
			TG_MORE);
		EXPECT_TRUE(out.empty());
		EXPECT_EQ(tg_framer_finish(framer.value), TG_TRUNCATED);
		EXPECT_EQ(tg_framer_finish(framer.value), TG_BAD_STATE);
	}
}

TEST(XmlFraming, RejectsUnsafeAndMalformedXmlWithoutPartialOutput)
{
	const std::vector<std::string> invalid = {"<!DOCTYPE event><event/>",
		"<!DOCTYPE event [<!ENTITY x 'boom'>]><event>&x;</event>",
		"<event><detail></event>", "<event a='1' a='2'/>", "<wrong/>",
		"<event>&missing;</event>", "<event>\xc0\xaf</event>",
		"<event>\x01</event>"};

	for (const std::string &input : invalid) {
		SCOPED_TRACE(input);
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(
				  TG_ENCODING_XML, nullptr, &framer.value),
			TG_OK);
		EXPECT_EQ(frame_all(framer.value, view(input), out),
			TG_MALFORMED);
		EXPECT_TRUE(out.empty());
		EXPECT_EQ(tg_framer_finish(framer.value), TG_BAD_STATE);
		ASSERT_EQ(tg_framer_reset(framer.value), TG_OK);
		EXPECT_GE(frame_all(framer.value, view(fixtures::xml()), out),
			TG_OK);
		EXPECT_EQ(out.size(), 1u);
	}
}

TEST(XmlFraming, DepthAndByteLimitsAreInclusive)
{
	std::string xml = "<event><detail><x/></detail></event>";
	tg_limits limits = TG_LIMITS_DEFAULT;

	limits.max_message_bytes = xml.size();
	limits.max_xml_depth = 3;
	Framer good;
	std::vector<fixtures::Bytes> out;

	ASSERT_EQ(
		tg_framer_create(TG_ENCODING_XML, &limits, &good.value), TG_OK);
	EXPECT_EQ(frame_all(good.value, view(xml), out), TG_OK);
	ASSERT_EQ(out.size(), 1u);
	for (bool depth : {false, true}) {
		Framer bad;
		std::vector<fixtures::Bytes> rejected;
		tg_limits small = limits;

		if (depth)
			--small.max_xml_depth;
		else
			--small.max_message_bytes;
		ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, &small, &bad.value),
			TG_OK);
		EXPECT_EQ(frame_all(bad.value, view(xml), rejected), TG_LIMIT);
		EXPECT_TRUE(rejected.empty());
	}
}

TEST(XmlFraming, IndependentHandlesAndResetNeverCombinePartialDocuments)
{
	Framer first;
	Framer second;
	std::vector<fixtures::Bytes> out;
	std::string xml = fixtures::xml();

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr, &first.value),
		TG_OK);
	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr, &second.value),
		TG_OK);
	ASSERT_EQ(
		frame_all(first.value, view(xml.substr(0, 19)), out), TG_MORE);
	EXPECT_EQ(frame_all(second.value, view(xml), out), TG_OK);
	ASSERT_EQ(out.size(), 1u);
	ASSERT_EQ(tg_framer_reset(first.value), TG_OK);
	EXPECT_EQ(frame_all(first.value, view(xml), out), TG_OK);
	ASSERT_EQ(out.size(), 2u);
	EXPECT_EQ(out[0], out[1]);
}

TEST(StreamFraming, EverySplitAndOneByteReadPreserveEnvelope)
{
	fixtures::Bytes payload(140, 0x12);
	fixtures::Bytes wire = fixtures::stream(payload);

	for (size_t split = 0; split <= wire.size(); ++split) {
		SCOPED_TRACE(split);
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(TG_ENCODING_PROTOBUF_V1, nullptr,
				  &framer.value),
			TG_OK);
		EXPECT_GE(frame_all(framer.value, {wire.data(), split}, out),
			TG_OK);
		EXPECT_GE(frame_all(framer.value,
				  {wire.data() + split, wire.size() - split},
				  out),
			TG_OK);
		ASSERT_EQ(out.size(), 1u);
		EXPECT_EQ(out[0], payload);
		EXPECT_EQ(tg_framer_finish(framer.value), TG_OK);
	}
}

TEST(StreamFraming, CoalescedFramesReturnOneAtATime)
{
	fixtures::Bytes first = fixtures::stream(fixtures::envelope());
	fixtures::Bytes second = fixtures::stream(fixtures::control());

	first.insert(first.end(), second.begin(), second.end());
	Framer framer;
	std::vector<fixtures::Bytes> out;

	ASSERT_EQ(tg_framer_create(
			  TG_ENCODING_PROTOBUF_V1, nullptr, &framer.value),
		TG_OK);
	EXPECT_EQ(frame_all(framer.value, view(first), out), TG_OK);
	ASSERT_EQ(out.size(), 2u);
	EXPECT_EQ(out[0], fixtures::envelope());
	EXPECT_EQ(out[1], fixtures::control());
}

TEST(StreamFraming, RejectsMagicZeroLengthOverflowAndElevenByteVarint)
{
	std::vector<fixtures::Bytes> invalid = {{0xbe, 0x01, 0x12},
		{0xbf, 0x00},
		{0xbf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0x02},
		{0xbf, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
			0x80, 0x00}};

	for (const auto &wire : invalid) {
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(TG_ENCODING_PROTOBUF_V1, nullptr,
				  &framer.value),
			TG_OK);
		EXPECT_EQ(
			frame_all(framer.value, view(wire), out), TG_MALFORMED);
		EXPECT_TRUE(out.empty());
	}
}

TEST(StreamFraming, OversizeHeaderFailsBeforePayloadIsSupplied)
{
	tg_limits limits = TG_LIMITS_DEFAULT;

	limits.max_message_bytes = 10;
	Framer framer;
	std::vector<fixtures::Bytes> out;
	fixtures::Bytes header = {0xbf, 11};

	ASSERT_EQ(tg_framer_create(
			  TG_ENCODING_PROTOBUF_V1, &limits, &framer.value),
		TG_OK);
	EXPECT_EQ(frame_all(framer.value, view(header), out), TG_LIMIT);
	EXPECT_TRUE(out.empty());
}

TEST(StreamFraming, EveryIncompletePrefixIsTruncatedAtEof)
{
	fixtures::Bytes wire = fixtures::stream(fixtures::envelope());

	for (size_t prefix = 1; prefix < wire.size(); ++prefix) {
		Framer framer;
		std::vector<fixtures::Bytes> out;

		ASSERT_EQ(tg_framer_create(TG_ENCODING_PROTOBUF_V1, nullptr,
				  &framer.value),
			TG_OK);
		EXPECT_EQ(frame_all(framer.value, {wire.data(), prefix}, out),
			TG_MORE);
		EXPECT_EQ(tg_framer_finish(framer.value), TG_TRUNCATED);
		EXPECT_TRUE(out.empty());
	}
}

TEST(Datagrams, PreservesWholeXmlDatagramIncludingWhitespace)
{
	std::string xml = " \n" + fixtures::extended_xml() + "\r\n";
	Message result;

	ASSERT_EQ(tg_datagram_decode(TG_DATAGRAM_AUTO, view(xml), 0, nullptr,
			  &result.value),
		TG_OK);
	EXPECT_EQ(result.value.encoding, TG_ENCODING_XML);
	EXPECT_EQ(string(result.value.bytes), xml);
}

TEST(Datagrams, MeshVersionIsNotPayloadLength)
{
	fixtures::Bytes payload = fixtures::envelope();
	fixtures::Bytes wire = fixtures::mesh(payload);
	Message result;

	ASSERT_EQ(tg_datagram_decode(TG_DATAGRAM_AUTO, view(wire), 0, nullptr,
			  &result.value),
		TG_OK);
	EXPECT_EQ(result.value.encoding, TG_ENCODING_PROTOBUF_V1);
	EXPECT_EQ(fixtures::Bytes(result.value.bytes.data,
			  result.value.bytes.data + result.value.bytes.size),
		payload);
}

TEST(Datagrams, RejectsWholeTruncatedReceivesAndConcatenatedDocuments)
{
	for (int truncated : {0, 1}) {
		Message result;
		std::string xml = fixtures::xml();

		if (!truncated)
			xml += xml;
		EXPECT_EQ(tg_datagram_decode(TG_DATAGRAM_AUTO, view(xml),
				  truncated, nullptr, &result.value),
			truncated ? TG_TRUNCATED : TG_MALFORMED);
		EXPECT_EQ(result.value.encoding, TG_ENCODING_NONE);
		EXPECT_EQ(result.value.bytes.data, nullptr);
		EXPECT_EQ(result.value.bytes.size, 0u);
	}
}

TEST(Datagrams, RejectsUnknownVersionAndNeverGuessesBinary)
{
	Message unknown;
	fixtures::Bytes wire = fixtures::mesh(fixtures::envelope(), 2);

	EXPECT_EQ(tg_datagram_decode(TG_DATAGRAM_AUTO, view(wire), 0, nullptr,
			  &unknown.value),
		TG_UNSUPPORTED);
	for (const fixtures::Bytes &bad :
		{fixtures::Bytes{0x12, 0x00}, fixtures::Bytes{0xbf, 1, 0xbe},
			fixtures::Bytes{0xbf, 1, 0xbf},
			fixtures::Bytes{0xbf, 0x80}}) {
		Message result;

		EXPECT_EQ(tg_datagram_decode(TG_DATAGRAM_AUTO, view(bad), 0,
				  nullptr, &result.value),
			TG_MALFORMED);
		EXPECT_EQ(result.value.bytes.size, 0u);
	}
}

TEST(Arguments, InvalidSpanDoesNotFailAnOtherwiseUsableFramer)
{
	Framer framer;
	Message message;
	size_t consumed = 123;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr, &framer.value),
		TG_OK);
	EXPECT_EQ(tg_framer_feed(framer.value, {nullptr, 1}, &consumed,
			  &message.value),
		TG_INVALID);
	EXPECT_EQ(consumed, 0u);
	EXPECT_EQ(message.value.bytes.data, nullptr);
	std::vector<fixtures::Bytes> out;

	EXPECT_EQ(frame_all(framer.value, view(fixtures::xml()), out), TG_OK);
	EXPECT_EQ(out.size(), 1u);
}

TEST(Arguments, EmptyInputAndCleanEofAreValid)
{
	Framer framer;
	Message message;
	size_t consumed = 42;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr, &framer.value),
		TG_OK);
	EXPECT_EQ(tg_framer_feed(framer.value, {nullptr, 0}, &consumed,
			  &message.value),
		TG_MORE);
	EXPECT_EQ(consumed, 0u);
	EXPECT_EQ(tg_framer_finish(framer.value), TG_OK);
}

TEST(XmlFraming, SuccessfulFinishIsTerminalUntilReset)
{
	Framer framer;
	std::vector<fixtures::Bytes> events;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr, &framer.value),
		TG_OK);
	ASSERT_EQ(
		frame_all(framer.value, view(fixtures::xml()), events), TG_OK);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(tg_framer_finish(framer.value), TG_OK);
	EXPECT_EQ(tg_framer_finish(framer.value), TG_OK);
	Message rejected;
	size_t consumed = 99;

	EXPECT_EQ(tg_framer_feed(framer.value, view(fixtures::xml()), &consumed,
			  &rejected.value),
		TG_BAD_STATE);
	EXPECT_EQ(consumed, 0u);
	EXPECT_EQ(rejected.value.bytes.data, nullptr);
	EXPECT_EQ(rejected.value.bytes.size, 0u);
	ASSERT_EQ(tg_framer_reset(framer.value), TG_OK);
	ASSERT_EQ(
		frame_all(framer.value, view(fixtures::xml()), events), TG_OK);
	ASSERT_EQ(events.size(), 2u);
	EXPECT_EQ(events[0], events[1]);
}

TEST(XmlFraming, LargeQuotedTokenAndCommentAllowOneByteReads)
{
	std::string xml = "<event uid='" + std::string(32768, '>') + "'><!--" +
			  std::string(32768, '>') + "--></event>";
	Framer framer;

	ASSERT_EQ(tg_framer_create(TG_ENCODING_XML, nullptr, &framer.value),
		TG_OK);
	for (size_t i = 0; i < xml.size(); ++i) {
		Message message;
		size_t consumed = 0;
		tg_status status = tg_framer_feed(framer.value,
			view(xml.substr(i, 1)), &consumed, &message.value);

		ASSERT_EQ(consumed, 1u);
		ASSERT_EQ(status, i + 1 == xml.size() ? TG_OK : TG_MORE);
		if (status == TG_OK)
			EXPECT_EQ(string(message.value.bytes), xml);
	}
}
