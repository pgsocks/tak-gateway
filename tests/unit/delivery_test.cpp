#include "contract_support.hpp"
#include <cstring>

using namespace contract;

static std::string span_text(tg_bytes bytes)
{
	if (!bytes.size)
		return {};
	return {reinterpret_cast<const char *>(bytes.data), bytes.size};
}

static tg_output_options datagram_output(size_t maximum = 65507)
{
	return {TG_OUTPUT_DATAGRAM, TG_FRAMING_XML_DOCUMENT, maximum, 0, 0};
}

static void expect_empty_report(const tg_publish_report &report)
{
	EXPECT_EQ(report.aggregate_sequence, 0u);
	EXPECT_EQ(report.accepted_outputs, 0u);
	EXPECT_EQ(report.dropped_outputs, 0u);
}

TEST(Delivery, NoOutputsIsAnObservableLossAndStillAdvancesSequence)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.queued_records, 0u);
	EXPECT_EQ(info.retained_bytes, 0u);
	EXPECT_EQ(info.failed, 0);
	tg_publish_report report{};
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_DROPPED);
	EXPECT_EQ(report.aggregate_sequence, 1u);
	EXPECT_EQ(report.accepted_outputs, 0u);
	EXPECT_EQ(report.dropped_outputs, 0u);
	tg_output_id output = 0;
	tg_output_options options = stream_output();
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	ASSERT_NE(output, 0u);
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(report.aggregate_sequence, 2u);
	EXPECT_EQ(report.accepted_outputs, 1u);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
}

TEST(Delivery, HealthyOutputsHaveIdenticalOrderAndPreserveDuplicates)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id first = 0, second = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &first),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &second),
		TG_OK);
	std::string expected;
	uint64_t sequence = 0;
	for (const std::string &xml : {fixtures::with_uid("source-a-1"),
		fixtures::with_uid("source-b-1"),
		fixtures::with_uid("source-a-1"),
		fixtures::with_uid("source-a-2")}) {
		tg_publish_report report{};
		ASSERT_EQ(tg_delivery_publish(delivery.value, view(xml),
			&report),
			TG_OK);
		EXPECT_EQ(report.aggregate_sequence, ++sequence);
		EXPECT_EQ(report.accepted_outputs, 2u);
		EXPECT_EQ(report.dropped_outputs, 0u);
		expected += record(xml);
	}
	std::string first_wire, second_wire;
	ASSERT_EQ(drain(delivery.value, first, first_wire, true), TG_OK);
	ASSERT_EQ(drain(delivery.value, second, second_wire), TG_OK);
	EXPECT_EQ(first_wire, expected);
	EXPECT_EQ(second_wire, expected);
	EXPECT_EQ(second_wire, first_wire);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.records_completed, 8u);
	EXPECT_EQ(info.queued_records, 0u);
	EXPECT_EQ(info.retained_bytes, 0u);
}

TEST(Delivery, U32beCountsUtf8BytesAndAddsNoTerminatorOrSeparator)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	std::string xml = "<?xml version='1.0'?>" + fixtures::extended_xml();
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(xml), &report),
		TG_OK);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire, true), TG_OK);
	ASSERT_EQ(wire.size(), xml.size() + 4);
	uint32_t length = 0;
	for (size_t i = 0; i < 4; ++i)
		length = (length << 8) | static_cast<uint8_t>(wire[i]);
	EXPECT_EQ(length, xml.size());
	EXPECT_EQ(wire.substr(4), xml);
	EXPECT_EQ(wire.back(), '>');
}

TEST(Delivery, XmlDocumentFramingHasNoLengthPrefixOrAddedWhitespace)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	options.framing = TG_FRAMING_XML_DOCUMENT;
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	for (unsigned i = 0; i < 2; ++i)
		ASSERT_EQ(tg_delivery_publish(delivery.value,
			view(fixtures::xml()), &report), TG_OK);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire, true), TG_OK);
	EXPECT_EQ(wire, fixtures::xml() + fixtures::xml());
}

TEST(Delivery, PublicationOwnsItsBytesBeforeCallerMutatesInput)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	std::string input = fixtures::extended_xml();
	const std::string original = input;
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(input), &report),
		TG_OK);
	EXPECT_EQ(input, original);
	input.assign(input.size(), 'x');
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire), TG_OK);
	EXPECT_EQ(wire, record(original));
}

TEST(Delivery, BorrowSurvivesPublicationsAndOtherOutputOperations)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id first = 0, second = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &first),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &second),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	tg_bytes borrowed{};
	ASSERT_EQ(tg_delivery_peek(delivery.value, first, &borrowed), TG_OK);
	const std::string original = span_text(borrowed);
	ASSERT_FALSE(original.empty());
	ASSERT_EQ(tg_delivery_publish(delivery.value,
		view(fixtures::with_uid("later")), &report), TG_OK);
	std::string other_wire;
	ASSERT_EQ(drain(delivery.value, second, other_wire), TG_OK);
	ASSERT_EQ(tg_delivery_remove_output(delivery.value, second), TG_OK);
	EXPECT_EQ(span_text(borrowed), original);
	std::string first_wire;
	ASSERT_EQ(drain(delivery.value, first, first_wire), TG_OK);
	EXPECT_EQ(first_wire, record(fixtures::xml()) +
		record(fixtures::with_uid("later")));
}

TEST(Delivery, ShortWritesKeepWholeRecordChargeUntilCompletion)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	const std::string expected = record(fixtures::xml());
	std::string wire;
	for (size_t i = 0; i < expected.size(); ++i) {
		tg_bytes span{};
		ASSERT_EQ(tg_delivery_peek(delivery.value, output, &span),
			TG_OK);
		ASSERT_GT(span.size, 0u);
		wire.push_back(static_cast<char>(span.data[0]));
		ASSERT_EQ(tg_delivery_consume(delivery.value, output, 0),
			TG_OK);
		ASSERT_EQ(tg_delivery_consume(delivery.value, output, 1),
			TG_OK);
		tg_delivery_info info{};
		ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info),
			TG_OK);
		bool complete = i + 1 == expected.size();
		EXPECT_EQ(info.queued_records, complete ? 0u : 1u);
		EXPECT_EQ(info.queued_bytes, complete ? 0u : expected.size());
		EXPECT_EQ(info.retained_bytes,
			complete ? 0u : fixtures::xml().size());
		EXPECT_EQ(info.records_completed, complete ? 1u : 0u);
	}
	EXPECT_EQ(wire, expected);
	tg_bytes empty{reinterpret_cast<const uint8_t *>(expected.data()), 1};
	EXPECT_EQ(tg_delivery_peek(delivery.value, output, &empty), TG_MORE);
	EXPECT_EQ(empty.data, nullptr);
	EXPECT_EQ(empty.size, 0u);
}

TEST(Delivery, ByteQueueBoundaryIncludesPrefixAndPartialRecordCharge)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	options.queue_bytes = record(fixtures::xml()).size();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	tg_bytes span{};
	ASSERT_EQ(tg_delivery_peek(delivery.value, output, &span), TG_OK);
	ASSERT_EQ(tg_delivery_consume(delivery.value, output, 1), TG_OK);
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_DROPPED);
	EXPECT_EQ(report.aggregate_sequence, 2u);
	EXPECT_EQ(report.accepted_outputs, 0u);
	EXPECT_EQ(report.dropped_outputs, 1u);
	std::string suffix;
	ASSERT_EQ(drain(delivery.value, output, suffix), TG_OK);
	EXPECT_EQ(suffix, record(fixtures::xml()).substr(1));
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(report.aggregate_sequence, 3u);
}

TEST(Delivery, TooSmallQueueIsValidAndReportsWholeRecordLoss)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	options.queue_bytes = fixtures::xml().size() + 3;
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_DROPPED);
	EXPECT_EQ(report.dropped_outputs, 1u);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, output, &info), TG_OK);
	EXPECT_EQ(info.queued_bytes, 0u);
	EXPECT_EQ(info.queued_records, 0u);
	EXPECT_EQ(info.records_dropped, 1u);
	EXPECT_EQ(info.failed, 0);
}

TEST(Delivery, FullSlowOutputDoesNotSuppressHealthyOutput)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options slow_options = stream_output();
	slow_options.queue_records = 1;
	tg_output_options healthy_options = stream_output();
	tg_output_id slow = 0, healthy = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &slow_options, &slow),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &healthy_options,
		&healthy), TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	std::string later = fixtures::with_uid("second");
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(later), &report),
		TG_DROPPED);
	EXPECT_EQ(report.accepted_outputs, 1u);
	EXPECT_EQ(report.dropped_outputs, 1u);
	std::string healthy_wire, slow_wire;
	ASSERT_EQ(drain(delivery.value, healthy, healthy_wire), TG_OK);
	ASSERT_EQ(drain(delivery.value, slow, slow_wire), TG_OK);
	EXPECT_EQ(healthy_wire, record(fixtures::xml()) + record(later));
	EXPECT_EQ(slow_wire, record(fixtures::xml()));
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, slow, &info), TG_OK);
	EXPECT_EQ(info.records_dropped, 1u);
	EXPECT_EQ(info.records_completed, 1u);
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, healthy, &info), TG_OK);
	EXPECT_EQ(info.records_dropped, 0u);
	EXPECT_EQ(info.records_completed, 2u);
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(later), &report),
		TG_OK);
	EXPECT_EQ(report.accepted_outputs, 2u);
}

TEST(Delivery, LimitsAndOutputOptionsAreCopied)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	limits.queue_records = 1;
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(&limits, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	limits.queue_records = 100;
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	options.queue_records = 100;
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_DROPPED);
	EXPECT_EQ(report.dropped_outputs, 1u);
}

TEST(Delivery, SustainedBurstsStayBoundedWhileHealthyConsumerProgresses)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	limits.queue_records = 2;
	limits.retained_bytes = 4 * fixtures::xml().size();
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(&limits, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id slow = 0, healthy = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &slow),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &healthy),
		TG_OK);
	std::string first_two;
	for (uint64_t i = 0; i < 200; ++i) {
		std::string xml = fixtures::with_uid("burst-" +
			std::to_string(i));
		tg_publish_report report{};
		ASSERT_EQ(tg_delivery_publish(delivery.value, view(xml),
			&report), i < 2 ? TG_OK : TG_DROPPED);
		EXPECT_EQ(report.aggregate_sequence, i + 1);
		EXPECT_EQ(report.accepted_outputs, i < 2 ? 2u : 1u);
		EXPECT_EQ(report.dropped_outputs, i < 2 ? 0u : 1u);
		std::string healthy_wire;
		ASSERT_EQ(drain(delivery.value, healthy, healthy_wire), TG_OK);
		EXPECT_EQ(healthy_wire, record(xml));
		if (i < 2)
			first_two += record(xml);
		tg_delivery_info totals{};
		ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &totals),
			TG_OK);
		EXPECT_LE(totals.queued_records, 2u);
		EXPECT_LE(totals.retained_bytes, 2 * fixtures::xml().size());
		EXPECT_EQ(totals.failed, 0);
	}
	std::string slow_wire;
	ASSERT_EQ(drain(delivery.value, slow, slow_wire), TG_OK);
	EXPECT_EQ(slow_wire, first_two);
	tg_delivery_info totals{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &totals), TG_OK);
	EXPECT_EQ(totals.records_dropped, 198u);
	EXPECT_EQ(totals.records_completed, 202u);
	EXPECT_EQ(totals.retained_bytes, 0u);
}

TEST(Delivery, GlobalRetainedBudgetCountsSharedPayloadOnce)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	limits.retained_bytes = fixtures::xml().size();
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(&limits, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id first = 0, second = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &first),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &second),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	tg_delivery_info totals{}, local{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &totals), TG_OK);
	EXPECT_EQ(totals.retained_bytes, fixtures::xml().size());
	EXPECT_EQ(totals.queued_bytes, 2 * record(fixtures::xml()).size());
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, first, &local), TG_OK);
	EXPECT_EQ(local.retained_bytes, 0u);
	report = {99, 99, 99};
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_LIMIT);
	expect_empty_report(report);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, first, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &totals), TG_OK);
	EXPECT_EQ(totals.retained_bytes, fixtures::xml().size());
	EXPECT_EQ(totals.failed, 0);
	wire.clear();
	ASSERT_EQ(drain(delivery.value, second, wire), TG_OK);
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &totals), TG_OK);
	EXPECT_EQ(totals.retained_bytes, 0u);
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(report.aggregate_sequence, 2u);
}

TEST(Delivery, MessageSizeBoundaryRejectsBeforeQueueOrSequenceMutation)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	limits.max_message_bytes = fixtures::xml().size();
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(&limits, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	report = {99, 99, 99};
	EXPECT_EQ(tg_delivery_publish(delivery.value,
		view(fixtures::extended_xml()), &report), TG_LIMIT);
	expect_empty_report(report);
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(report.aggregate_sequence, 2u);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()) + record(fixtures::xml()));
}

TEST(Delivery, NewOutputsReceiveOnlyFuturePublications)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id first = 0, later = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &first),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &later),
		TG_OK);
	tg_bytes span{};
	EXPECT_EQ(tg_delivery_peek(delivery.value, later, &span), TG_MORE);
	EXPECT_EQ(span.size, 0u);
	std::string next = fixtures::with_uid("future-only");
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(next), &report),
		TG_OK);
	std::string first_wire, later_wire;
	ASSERT_EQ(drain(delivery.value, first, first_wire), TG_OK);
	ASSERT_EQ(drain(delivery.value, later, later_wire), TG_OK);
	EXPECT_EQ(first_wire, record(fixtures::xml()) + record(next));
	EXPECT_EQ(later_wire, record(next));
}

TEST(Delivery, RemovedPartialRecordsAreDroppedAndNotReplayedToReplacement)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id old = 0, replacement = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &old),
		TG_OK);
	tg_publish_report report{};
	for (unsigned i = 0; i < 2; ++i)
		ASSERT_EQ(tg_delivery_publish(delivery.value,
			view(fixtures::xml()), &report), TG_OK);
	tg_bytes span{};
	ASSERT_EQ(tg_delivery_peek(delivery.value, old, &span), TG_OK);
	ASSERT_EQ(tg_delivery_consume(delivery.value, old, 1), TG_OK);
	ASSERT_EQ(tg_delivery_remove_output(delivery.value, old), TG_OK);
	tg_delivery_info totals{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &totals), TG_OK);
	EXPECT_EQ(totals.records_dropped, 2u);
	EXPECT_EQ(totals.queued_records, 0u);
	EXPECT_EQ(totals.retained_bytes, 0u);
	EXPECT_EQ(tg_delivery_remove_output(delivery.value, old), TG_INVALID);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options,
		&replacement),
		TG_OK);
	EXPECT_NE(replacement, old);
	EXPECT_NE(replacement, 0u);
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, replacement, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
}

TEST(Delivery, RemovingOneOutputKeepsOtherReferencesAndTheirOrder)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id failed = 0, healthy = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &failed),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &healthy),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	ASSERT_EQ(tg_delivery_remove_output(delivery.value, failed), TG_OK);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.retained_bytes, fixtures::xml().size());
	EXPECT_EQ(info.queued_records, 1u);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, healthy, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.retained_bytes, 0u);
	EXPECT_EQ(info.records_dropped, 1u);
	EXPECT_EQ(info.records_completed, 1u);
}

TEST(Delivery, OutputLimitReleasesSlotsWithoutReusingIdentifiers)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	limits.max_outputs = 1;
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(&limits, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id first = 0, rejected = 99, replacement = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &first),
		TG_OK);
	EXPECT_EQ(tg_delivery_add_output(delivery.value, &options, &rejected),
		TG_LIMIT);
	EXPECT_EQ(rejected, 0u);
	ASSERT_EQ(tg_delivery_remove_output(delivery.value, first), TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options,
		&replacement),
		TG_OK);
	EXPECT_NE(replacement, first);
}

TEST(Delivery, DatagramIsUnframedAndCannotBePartiallyConsumed)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = datagram_output(fixtures::xml().size());
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	tg_bytes span{};
	ASSERT_EQ(tg_delivery_peek(delivery.value, output, &span), TG_OK);
	EXPECT_EQ(span_text(span), fixtures::xml());
	EXPECT_EQ(tg_delivery_consume(delivery.value, output, 0), TG_OK);
	EXPECT_EQ(tg_delivery_consume(delivery.value, output, 1), TG_INVALID);
	tg_bytes unchanged{};
	ASSERT_EQ(tg_delivery_peek(delivery.value, output, &unchanged), TG_OK);
	EXPECT_EQ(span_text(unchanged), fixtures::xml());
	ASSERT_EQ(tg_delivery_consume(delivery.value, output, unchanged.size),
		TG_OK);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, output, &info), TG_OK);
	EXPECT_EQ(info.records_completed, 1u);
	EXPECT_EQ(info.records_dropped, 0u);
	EXPECT_EQ(info.queued_bytes, 0u);
}

TEST(Delivery, DatagramOversizeDropsOnlyAffectedDestination)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options udp_options =
		datagram_output(fixtures::xml().size() - 1);
	tg_output_options tcp_options = stream_output();
	tg_output_id udp = 0, tcp = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &udp_options, &udp),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &tcp_options, &tcp),
		TG_OK);
	tg_publish_report report{};
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_DROPPED);
	EXPECT_EQ(report.accepted_outputs, 1u);
	EXPECT_EQ(report.dropped_outputs, 1u);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, udp, &info), TG_OK);
	EXPECT_EQ(info.udp_oversize, 1u);
	EXPECT_EQ(info.records_dropped, 1u);
	EXPECT_EQ(info.queued_records, 0u);
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, tcp, &info), TG_OK);
	EXPECT_EQ(info.udp_oversize, 0u);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, tcp, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
}

TEST(Delivery, InvalidOutputCombinationsDoNotInstallAnOutput)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	std::vector<tg_output_options> invalid;
	tg_output_options value = datagram_output();
	value.framing = TG_FRAMING_U32BE;
	invalid.push_back(value);
	value = datagram_output(0);
	invalid.push_back(value);
	value = datagram_output(65508);
	invalid.push_back(value);
	value = stream_output();
	value.max_datagram_bytes = 1;
	invalid.push_back(value);
	for (const tg_output_options &options : invalid) {
		tg_output_id output = 99;
		EXPECT_EQ(tg_delivery_add_output(delivery.value, &options,
			&output), TG_INVALID);
		EXPECT_EQ(output, 0u);
	}
	tg_publish_report report{};
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_DROPPED);
	EXPECT_EQ(report.accepted_outputs, 0u);
	EXPECT_EQ(report.dropped_outputs, 0u);
}

TEST(Delivery, OverconsumptionDoesNotAdvanceAStreamRecord)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	tg_bytes span{};
	ASSERT_EQ(tg_delivery_peek(delivery.value, output, &span), TG_OK);
	EXPECT_EQ(tg_delivery_consume(delivery.value, output, span.size + 1),
		TG_INVALID);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
}

TEST(Delivery, FatalQueueOverflowPreflightsAllOutputsAndKeepsOldQueues)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_FAIL,
		&delivery.value), TG_OK);
	tg_output_options slow_options = stream_output();
	slow_options.queue_records = 1;
	tg_output_options healthy_options = stream_output();
	tg_output_id slow = 0, healthy = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &slow_options, &slow),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &healthy_options,
		&healthy), TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	report = {99, 99, 99};
	EXPECT_EQ(tg_delivery_publish(delivery.value,
		view(fixtures::with_uid("must-not-partially-admit")), &report),
		TG_LIMIT);
	expect_empty_report(report);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.failed, 1);
	EXPECT_EQ(info.queued_records, 2u);
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_BAD_STATE);
	tg_output_id rejected = 99;
	EXPECT_EQ(tg_delivery_add_output(delivery.value, &healthy_options,
		&rejected), TG_BAD_STATE);
	EXPECT_EQ(rejected, 0u);
	std::string slow_wire, healthy_wire;
	ASSERT_EQ(drain(delivery.value, slow, slow_wire), TG_OK);
	ASSERT_EQ(drain(delivery.value, healthy, healthy_wire), TG_OK);
	EXPECT_EQ(slow_wire, record(fixtures::xml()));
	EXPECT_EQ(healthy_wire, record(fixtures::xml()));
	EXPECT_EQ(tg_delivery_remove_output(delivery.value, slow), TG_OK);
	EXPECT_EQ(tg_delivery_remove_output(delivery.value, healthy), TG_OK);
}

TEST(Delivery, FatalSharedBudgetFailurePreservesExistingRecordForDrain)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	limits.retained_bytes = fixtures::xml().size();
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(&limits, TG_OVERFLOW_FAIL,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_LIMIT);
	expect_empty_report(report);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.failed, 1);
	EXPECT_EQ(info.retained_bytes, fixtures::xml().size());
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire, true), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_BAD_STATE);
}

TEST(Delivery, FatalUdpOversizeDoesNotAdmitToHealthyStream)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_FAIL,
		&delivery.value), TG_OK);
	tg_output_options tcp_options = stream_output();
	tg_output_options udp_options =
		datagram_output(fixtures::xml().size() - 1);
	tg_output_id tcp = 0, udp = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &tcp_options, &tcp),
		TG_OK);
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &udp_options, &udp),
		TG_OK);
	tg_publish_report report{99, 99, 99};
	EXPECT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_LIMIT);
	expect_empty_report(report);
	tg_bytes span{};
	EXPECT_EQ(tg_delivery_peek(delivery.value, tcp, &span), TG_MORE);
	EXPECT_EQ(span.size, 0u);
	tg_delivery_info info{};
	ASSERT_EQ(tg_delivery_snapshot(delivery.value, 0, &info), TG_OK);
	EXPECT_EQ(info.failed, 1);
	EXPECT_EQ(info.queued_records, 0u);
	EXPECT_EQ(info.retained_bytes, 0u);
}

TEST(Delivery, InvalidXmlDoesNotChangeQueuesOrAggregateSequence)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	tg_output_options options = stream_output();
	tg_output_id output = 0;
	ASSERT_EQ(tg_delivery_add_output(delivery.value, &options, &output),
		TG_OK);
	for (const std::string &xml : {std::string("<other/>"),
		std::string("<event><detail></event>"),
		fixtures::xml() + fixtures::xml(),
		std::string("<!DOCTYPE event><event/>")}) {
		tg_publish_report report{99, 99, 99};
		EXPECT_EQ(tg_delivery_publish(delivery.value, view(xml),
			&report),
			TG_MALFORMED);
		expect_empty_report(report);
	}
	tg_publish_report report{};
	ASSERT_EQ(tg_delivery_publish(delivery.value, view(fixtures::xml()),
		&report), TG_OK);
	EXPECT_EQ(report.aggregate_sequence, 1u);
	std::string wire;
	ASSERT_EQ(drain(delivery.value, output, wire), TG_OK);
	EXPECT_EQ(wire, record(fixtures::xml()));
}

TEST(Delivery, UnknownOutputSnapshotClearsResultAndRemovalRejectsZero)
{
	Delivery delivery;
	ASSERT_EQ(tg_delivery_create(nullptr, TG_OVERFLOW_DROP_NEWEST,
		&delivery.value), TG_OK);
	EXPECT_EQ(tg_delivery_remove_output(delivery.value, 0), TG_INVALID);
	EXPECT_EQ(tg_delivery_remove_output(delivery.value, 1), TG_INVALID);
	tg_delivery_info info{99, 99, 99, 99, 99, 99, 1};
	EXPECT_EQ(tg_delivery_snapshot(delivery.value, 1, &info), TG_INVALID);
	EXPECT_EQ(info.queued_records, 0u);
	EXPECT_EQ(info.queued_bytes, 0u);
	EXPECT_EQ(info.retained_bytes, 0u);
	EXPECT_EQ(info.records_completed, 0u);
	EXPECT_EQ(info.records_dropped, 0u);
	EXPECT_EQ(info.udp_oversize, 0u);
	EXPECT_EQ(info.failed, 0);
}

TEST(Delivery, NullDestructionIsHarmless)
{
	tg_delivery_destroy(nullptr);
}
