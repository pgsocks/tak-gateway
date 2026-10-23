#include "contract_support.hpp"
#include <limits>

using namespace contract;

static tg_status normalize(const fixtures::Bytes &input, Buffer &xml,
	tg_normalize_report &report,
	tg_preservation_policy policy = TG_PRESERVE_OPAQUE,
	const tg_limits *limits = nullptr)
{
	return tg_normalize(TG_ENCODING_PROTOBUF_V1, view(input), policy,
		limits, &xml.value, &report);
}

TEST(Normalization, XmlPreservesBytesWithoutMutatingCallerStorage)
{
	std::string xml = "<?xml version='1.0' encoding='UTF-8'?>" +
			  fixtures::extended_xml();
	const std::string original = xml;
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(tg_normalize(TG_ENCODING_XML, view(xml), TG_PRESERVE_OPAQUE,
			  nullptr, &output.value, &report),
		TG_OK);
	EXPECT_EQ(string(output.value), original);
	EXPECT_EQ(xml, original);
	EXPECT_EQ(report.unknown_fields, 0u);
	EXPECT_EQ(report.had_control, 0);
	EXPECT_EQ(report.preserved_opaque, 0);
	xml.assign(xml.size(), 'x');
	EXPECT_EQ(string(output.value), original);
}

TEST(Normalization, UnrelatedApplicationTypesBeginningWithTRemainEvents)
{
	for (const std::string type : {"t-x-m-n", "t-custom", "t-x-takp-z"}) {
		std::string xml = fixtures::control_xml(type, "<custom/>");
		Buffer output;
		tg_normalize_report report{};

		ASSERT_EQ(tg_normalize(TG_ENCODING_XML, view(xml),
				  TG_PRESERVE_OPAQUE, nullptr, &output.value,
				  &report),
			TG_OK);
		EXPECT_EQ(string(output.value), xml);
	}
}

TEST(Normalization, RecognizedXmlNegotiationControlsAreNotOutput)
{
	for (const std::string &xml :
		{fixtures::offer(), fixtures::response(true)}) {
		Buffer output;
		tg_normalize_report report{};

		EXPECT_EQ(tg_normalize(TG_ENCODING_XML, view(xml),
				  TG_PRESERVE_OPAQUE, nullptr, &output.value,
				  &report),
			TG_NO_EVENT);
		EXPECT_EQ(output.value.data, nullptr);
		EXPECT_EQ(output.value.size, 0u);
		EXPECT_EQ(report.had_control, 1);
	}
}

TEST(Normalization, ControlOnlyProtobufProducesNoApplicationEvent)
{
	Buffer output;
	tg_normalize_report report{};

	EXPECT_EQ(normalize(fixtures::control(), output, report), TG_NO_EVENT);
	EXPECT_EQ(output.value.data, nullptr);
	EXPECT_EQ(output.value.size, 0u);
	EXPECT_EQ(report.had_control, 1);
}

TEST(Normalization, EventOnlyEnvelopeMapsCoreWithoutInventingValues)
{
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(fixtures::envelope(), output, report), TG_OK);
	std::string xml = string(output.value);

	EXPECT_NE(xml.find("fixture-1"), std::string::npos);
	EXPECT_NE(xml.find("a-f-G-U-C"), std::string::npos);
	EXPECT_NE(xml.find("2026-10-22"), std::string::npos);
	EXPECT_NE(xml.find("-77"), std::string::npos);
	EXPECT_EQ(report.had_control, 0);
	EXPECT_EQ(report.preserved_opaque, 0);
	EXPECT_EQ(report.unknown_fields, 0u);
}

TEST(Normalization, CombinedEnvelopeProcessesControlAndRetainsItsEvent)
{
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(fixtures::combined(), output, report), TG_OK);
	EXPECT_NE(string(output.value).find("fixture-1"), std::string::npos);
	EXPECT_EQ(report.had_control, 1);
	EXPECT_EQ(report.preserved_opaque, 0);
}

TEST(Normalization, PreservesExactEventOnlyWireBytesForEveryFallbackKind)
{
	const std::vector<fixtures::Bytes> inputs = {
		fixtures::unknown_event_envelope(),
		fixtures::unknown_nested_envelope(),
		fixtures::timestamp_envelope(),
		fixtures::binary_extension_envelope(),
		fixtures::conflicting_detail_envelope()};

	for (const auto &event_only : inputs) {
		Buffer output;
		tg_normalize_report report{};
		fixtures::Bytes combined = fixtures::combined(event_only);
		const fixtures::Bytes original = combined;

		ASSERT_EQ(normalize(combined, output, report), TG_OK);
		std::string xml = string(output.value);

		EXPECT_EQ(combined, original);
		EXPECT_EQ(report.had_control, 1);
		EXPECT_EQ(report.preserved_opaque, 1);
		EXPECT_NE(xml.find("urn:tak-gateway:preservation:1"),
			std::string::npos);
		fixtures::Bytes recovered;

		ASSERT_TRUE(fixtures::preserved_envelope(xml, recovered));
		EXPECT_EQ(recovered, event_only);
		EXPECT_EQ(xml.find(fixtures::base64(combined)),
			std::string::npos);
		EXPECT_NE(xml.find("fixture-1"), std::string::npos);
	}
}

TEST(Normalization, UnknownNestedFieldsAreCountedAtEveryLevel)
{
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(
		normalize(fixtures::unknown_nested_envelope(), output, report),
		TG_OK);
	EXPECT_GE(report.unknown_fields, 2u);
	EXPECT_EQ(report.preserved_opaque, 1);
}

TEST(Normalization, TopLevelServerTimestampsRemainInOpaqueEnvelope)
{
	Buffer output;
	tg_normalize_report report{};
	fixtures::Bytes input = fixtures::timestamp_envelope();

	ASSERT_EQ(normalize(input, output, report), TG_OK);
	EXPECT_NE(string(output.value).find(fixtures::base64(input)),
		std::string::npos);
	EXPECT_EQ(report.preserved_opaque, 1);
}

TEST(Normalization, RejectPolicyNeverReturnsPartialMappings)
{
	for (const fixtures::Bytes &input : {fixtures::unknown_event_envelope(),
		     fixtures::unknown_nested_envelope(),
		     fixtures::timestamp_envelope(),
		     fixtures::binary_extension_envelope(),
		     fixtures::conflicting_detail_envelope()}) {
		Buffer output;
		tg_normalize_report report{99, 1, 1};

		EXPECT_EQ(normalize(input, output, report, TG_REJECT_LOSSY),
			TG_LOSSY);
		EXPECT_EQ(output.value.data, nullptr);
		EXPECT_EQ(output.value.size, 0u);
		EXPECT_EQ(report.unknown_fields, 0u);
		EXPECT_EQ(report.had_control, 0);
		EXPECT_EQ(report.preserved_opaque, 0);
	}
}

TEST(Normalization, ResidualAndTypedDetailsBothSurviveWhenCompatible)
{
	fixtures::Bytes detail;
	fixtures::Bytes contact;

	fixtures::text(detail, 1, "<extra xmlns='urn:custom'>hello</extra>");
	fixtures::text(contact, 2, "Mapped contact");
	fixtures::blob(detail, 2, contact);
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(fixtures::envelope(fixtures::event(detail)), output,
			  report),
		TG_OK);
	std::string xml = string(output.value);

	EXPECT_NE(xml.find("Mapped contact"), std::string::npos);
	EXPECT_NE(xml.find("urn:custom"), std::string::npos);
	EXPECT_NE(xml.find("hello"), std::string::npos);
	EXPECT_EQ(report.preserved_opaque, 0);
}

TEST(Normalization, RepeatedEventMessagesRetainProtobufMergeSemantics)
{
	fixtures::Bytes input = fixtures::envelope();
	fixtures::Bytes update;

	fixtures::text(update, 5, "last-wire-uid");
	fixtures::blob(input, 2, update);
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(input, output, report), TG_OK);
	EXPECT_NE(
		string(output.value).find("last-wire-uid"), std::string::npos);
	EXPECT_EQ(string(output.value).find("fixture-1"), std::string::npos);
	EXPECT_NE(string(output.value).find("a-f-G-U-C"), std::string::npos);
}

TEST(Normalization, RejectsMalformedAndEmptyEnvelopes)
{
	const std::vector<fixtures::Bytes> invalid = {
		{}, {0}, {0x12, 0x7f, 1}, {0x12, 0x80}, {0x0f, 1}};

	for (const auto &input : invalid) {
		Buffer output;
		tg_normalize_report report{3, 1, 1};

		EXPECT_EQ(normalize(input, output, report), TG_MALFORMED);
		EXPECT_EQ(output.value.data, nullptr);
		EXPECT_EQ(report.unknown_fields, 0u);
		EXPECT_EQ(report.had_control, 0);
	}
}

TEST(Normalization, InvalidCoreNeverUsesOpaqueFallbackToInventXml)
{
	for (double latitude : {91.0, std::numeric_limits<double>::infinity(),
		     std::numeric_limits<double>::quiet_NaN()}) {
		fixtures::Bytes override;

		fixtures::real(override, 10, latitude);
		Buffer output;
		tg_normalize_report report{};

		EXPECT_EQ(normalize(fixtures::envelope(
					    fixtures::event({}, override)),
				  output, report),
			TG_UNSUPPORTED);
		EXPECT_EQ(output.value.data, nullptr);
	}
}

TEST(Normalization, EnforcesLimitAfterOpaqueExpansion)
{
	fixtures::Bytes input = fixtures::unknown_nested_envelope();
	Buffer full;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(input, full, report), TG_OK);
	ASSERT_GT(full.value.size, input.size());
	tg_limits limits = TG_LIMITS_DEFAULT;

	limits.max_message_bytes = full.value.size - 1;
	Buffer rejected;

	EXPECT_EQ(
		normalize(input, rejected, report, TG_PRESERVE_OPAQUE, &limits),
		TG_LIMIT);
	EXPECT_EQ(rejected.value.data, nullptr);
	EXPECT_EQ(report.preserved_opaque, 0);
}

TEST(Ownership, BufferReleaseZerosDescriptorAndAllowsEmptyRelease)
{
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(fixtures::envelope(), output, report), TG_OK);
	ASSERT_NE(output.value.data, nullptr);
	tg_buffer_release(&output.value);
	EXPECT_EQ(output.value.data, nullptr);
	EXPECT_EQ(output.value.size, 0u);
	tg_buffer_release(&output.value);
	tg_buffer_release(nullptr);
}

TEST(Normalization, PreservationNamespaceDoesNotOverwriteExistingPrefix)
{
	fixtures::Bytes detail;
	fixtures::Bytes unknown;

	fixtures::text(detail, 1,
		"<gw:custom xmlns:gw='urn:existing'>keep</gw:custom>");
	fixtures::integer(unknown, 100, 7);
	fixtures::Bytes input =
		fixtures::envelope(fixtures::event(detail, unknown));
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(normalize(input, output, report), TG_OK);
	std::string xml = string(output.value);

	EXPECT_NE(xml.find("urn:existing"), std::string::npos);
	EXPECT_NE(xml.find("keep"), std::string::npos);
	fixtures::Bytes recovered;

	ASSERT_TRUE(fixtures::preserved_envelope(xml, recovered));
	EXPECT_EQ(recovered, input);
}

TEST(Normalization, MalformedControlBodyCannotHideBehindAValidEvent)
{
	fixtures::Bytes input{0x0a, 0x01, 0x80};
	fixtures::Bytes event = fixtures::envelope();

	input.insert(input.end(), event.begin(), event.end());
	Buffer output;
	tg_normalize_report report{};

	EXPECT_EQ(tg_normalize(TG_ENCODING_PROTOBUF_V1, view(input),
			  TG_PRESERVE_OPAQUE, nullptr, &output.value, &report),
		TG_MALFORMED);
	EXPECT_EQ(output.value.data, nullptr);
	EXPECT_EQ(report.had_control, 0);
}
