#include "contract_support.hpp"

using namespace contract;

static void create_session(
	Session &session, tg_protocol_preference preference = TG_PREFER_XML)
{
	tg_session_options options =
		session_options(TG_PROTOCOL_NEGOTIATED, preference);

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
}

static void request_upgrade(Session &session, uint64_t now = 10)
{
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(
			  session.value, view(fixtures::offer()), now, events),
		TG_OK);
	EXPECT_TRUE(events.empty());
	std::string written;

	ASSERT_EQ(write_all(session.value, now, written), TG_OK);
	EXPECT_NE(written.find("TakRequest"), std::string::npos);
	EXPECT_NE(written.find("transaction-1"), std::string::npos);
}

TEST(Sessions, DefaultsToXmlWithoutManufacturingAnIdentityOrUpgrade)
{
	Session session;

	create_session(session);
	ASSERT_NE(session.value, nullptr);
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(
		receive_all(session.value, view(fixtures::offer()), 1, events),
		TG_OK);
	EXPECT_TRUE(events.empty());
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
	EXPECT_EQ(info.protocol_offer_received, 1);
	tg_bytes write{};

	EXPECT_EQ(tg_session_peek_write(session.value, &write), TG_MORE);
	EXPECT_EQ(write.size, 0u);
}

TEST(Sessions, NoOfferMeansOrdinaryXmlContinuesEvenWhenProtobufPreferred)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	std::vector<fixtures::Bytes> events;

	EXPECT_GE(receive_all(session.value, view(fixtures::xml()), 1, events),
		TG_OK);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], fixtures::bytes(fixtures::xml()));
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
	EXPECT_EQ(info.protocol_offer_received, 0);
}

TEST(Sessions, UpgradeAcceptanceAndBinaryInSameReadSwitchAtExactBoundary)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	request_upgrade(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	fixtures::Bytes input = fixtures::bytes(fixtures::response(true));
	fixtures::Bytes binary = fixtures::stream(fixtures::envelope());

	input.insert(input.end(), binary.begin(), binary.end());
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(session.value, view(input), 11, events), TG_OK);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], fixtures::envelope());
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_PROTOBUF);
	EXPECT_EQ(info.events_received, 1u);
}

TEST(Sessions, EverySplitOfAcceptanceAndBinaryRemainsValid)
{
	fixtures::Bytes input = fixtures::bytes(fixtures::response(true));
	fixtures::Bytes binary = fixtures::stream(fixtures::combined());

	input.insert(input.end(), binary.begin(), binary.end());
	for (size_t split = 0; split <= input.size(); ++split) {
		SCOPED_TRACE(split);
		Session session;

		create_session(session, TG_PREFER_PROTOBUF);
		ASSERT_NE(session.value, nullptr);
		request_upgrade(session);
		ASSERT_FALSE(testing::Test::HasFatalFailure());
		std::vector<fixtures::Bytes> events;

		EXPECT_GE(receive_all(session.value, {input.data(), split}, 11,
				  events),
			TG_OK);
		EXPECT_GE(receive_all(session.value,
				  {input.data() + split, input.size() - split},
				  12, events),
			TG_OK);
		ASSERT_EQ(events.size(), 1u);
		EXPECT_EQ(events[0], fixtures::envelope());
	}
}

TEST(Sessions, OrdinaryXmlIsForwardedWhileUpgradePending)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	request_upgrade(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(session.value, view(fixtures::extended_xml()), 11,
			  events),
		TG_OK);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], fixtures::bytes(fixtures::extended_xml()));
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_UPGRADE_PENDING);
	tg_bytes write{};

	EXPECT_EQ(tg_session_peek_write(session.value, &write), TG_MORE);
}

TEST(Sessions, RejectedUpgradeReturnsToXml)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	request_upgrade(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	std::vector<fixtures::Bytes> events;
	std::string input = fixtures::response(false) + fixtures::xml();

	ASSERT_GE(receive_all(session.value, view(input), 11, events), TG_OK);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], fixtures::bytes(fixtures::xml()));
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
}

TEST(Sessions, MismatchedNegotiationUidFailsOnlyThatSession)
{
	Session first;
	Session second;

	create_session(first, TG_PREFER_PROTOBUF);
	ASSERT_NE(first.value, nullptr);
	create_session(second);
	ASSERT_NE(second.value, nullptr);
	request_upgrade(first);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	std::vector<fixtures::Bytes> events;

	EXPECT_EQ(receive_all(first.value,
			  view(fixtures::response(true, "wrong-transaction")),
			  11, events),
		TG_MALFORMED);
	EXPECT_GE(receive_all(second.value, view(fixtures::xml()), 11, events),
		TG_OK);
	ASSERT_EQ(events.size(), 1u);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(first.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_FAILED);
}

TEST(Sessions, NegotiationDeadlineStartsAfterLastActuallyWrittenByte)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(
		receive_all(session.value, view(fixtures::offer()), 1, events),
		TG_OK);
	tg_bytes write{};

	ASSERT_EQ(tg_session_peek_write(session.value, &write), TG_OK);
	ASSERT_GT(write.size, 1u);
	ASSERT_EQ(tg_session_consume_write(session.value, write.size - 1, 2),
		TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 65002, 0), TG_OK);
	ASSERT_EQ(tg_session_peek_write(session.value, &write), TG_OK);
	ASSERT_EQ(write.size, 1u);
	ASSERT_EQ(tg_session_consume_write(session.value, 1, 65003), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 130002, 0), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 130003, 0), TG_TIMEOUT);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_FAILED);
	EXPECT_EQ(tg_session_tick(session.value, 130004, 0), TG_BAD_STATE);
}

TEST(Sessions, PartialMessageDeadlineHasAnInclusiveBoundary)
{
	Session session;

	create_session(session);
	ASSERT_NE(session.value, nullptr);
	std::vector<fixtures::Bytes> events;

	ASSERT_EQ(receive_all(session.value, view(std::string("<event")), 10,
			  events),
		TG_MORE);
	EXPECT_EQ(tg_session_tick(session.value, 30009, 0), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 30010, 0), TG_TIMEOUT);
	EXPECT_TRUE(events.empty());
}

TEST(Sessions, CredentialsAreXmlEscapedAndPrecedeAllNegotiationBytes)
{
	tg_session_options options =
		session_options(TG_PROTOCOL_NEGOTIATED, TG_PREFER_PROTOBUF);

	options.username = "user<&\"'";
	options.password = "secret<&\"'";
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(
		receive_all(session.value, view(fixtures::offer()), 1, events),
		TG_OK);
	tg_bytes first{};

	ASSERT_EQ(tg_session_peek_write(session.value, &first), TG_OK);
	std::string preamble(
		reinterpret_cast<const char *>(first.data), first.size);

	EXPECT_NE(preamble.find("<auth"), std::string::npos);
	EXPECT_EQ(preamble.find("TakRequest"), std::string::npos);
	EXPECT_NE(preamble.find("&lt;"), std::string::npos);
	EXPECT_NE(preamble.find("&amp;"), std::string::npos);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.credentials_sent, 0);
	std::string written;

	ASSERT_EQ(write_all(session.value, 2, written, true), TG_OK);
	EXPECT_LT(written.find("<auth"), written.find("TakRequest"));
	EXPECT_TRUE(events.empty());
	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.credentials_sent, 1);
}

TEST(Sessions, FixedModesDoNotNegotiate)
{
	for (tg_protocol_mode mode :
		{TG_PROTOCOL_XML_ONLY, TG_PROTOCOL_PROTOBUF_ONLY}) {
		Session session;
		tg_session_options options = session_options(mode);

		ASSERT_EQ(
			tg_session_create(&options, nullptr, 0, &session.value),
			TG_OK);
		std::vector<fixtures::Bytes> events;
		fixtures::Bytes wire =
			mode == TG_PROTOCOL_XML_ONLY
				? fixtures::bytes(
					  fixtures::offer() + fixtures::xml())
				: fixtures::stream(fixtures::combined());

		ASSERT_GE(receive_all(session.value, view(wire), 1, events),
			TG_OK);
		ASSERT_EQ(events.size(), 1u);
		EXPECT_EQ(events[0], mode == TG_PROTOCOL_XML_ONLY
					     ? fixtures::bytes(fixtures::xml())
					     : fixtures::envelope());
		tg_bytes write{};

		EXPECT_EQ(
			tg_session_peek_write(session.value, &write), TG_MORE);
	}
}

TEST(Sessions, ReconnectDiscardsPartialXmlAndResetsNegotiationCounters)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	std::vector<fixtures::Bytes> events;

	ASSERT_EQ(receive_all(session.value,
			  view(fixtures::xml().substr(0, 31)), 1, events),
		TG_MORE);
	ASSERT_EQ(tg_session_reset(session.value, 2), TG_OK);
	ASSERT_GE(receive_all(session.value, view(fixtures::xml()), 3, events),
		TG_OK);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], fixtures::bytes(fixtures::xml()));
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
	EXPECT_EQ(info.protocol_offer_received, 0);
	EXPECT_EQ(info.controls_consumed, 0u);
	EXPECT_EQ(info.events_received, 1u);
}

TEST(Sessions, BackwardsTimeAndOversizedWriteAcknowledgmentPreserveState)
{
	Session session;

	create_session(session, TG_PREFER_PROTOBUF);
	ASSERT_NE(session.value, nullptr);
	request_upgrade(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	tg_session_info before{};
	tg_session_info after{};

	ASSERT_EQ(tg_session_snapshot(session.value, &before), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 9, 0), TG_INVALID);
	ASSERT_EQ(tg_session_snapshot(session.value, &after), TG_OK);
	EXPECT_EQ(before.state, after.state);
	EXPECT_EQ(before.controls_consumed, after.controls_consumed);
	EXPECT_EQ(tg_session_consume_write(session.value, 1, 11), TG_INVALID);
}

TEST(Sessions, RejectsUnsupportedVersionsAndUnsafeDeadlineConfiguration)
{
	for (int defect = 0; defect < 4; ++defect) {
		Session session;
		tg_session_options options = session_options();

		if (defect == 0)
			options.version = 2;
		if (defect == 1)
			options.response_timeout_ms = 59999;
		if (defect == 2)
			options.partial_timeout_ms = 0;
		if (defect == 3)
			options.username = "username-without-password";
		EXPECT_EQ(
			tg_session_create(&options, nullptr, 0, &session.value),
			defect == 0 ? TG_UNSUPPORTED : TG_INVALID);
		EXPECT_EQ(session.value, nullptr);
	}
}

TEST(Maintenance, IdentityIsExplicitCopiedAndFollowsCredentialsBeforeRequest)
{
	tg_session_options options =
		session_options(TG_PROTOCOL_NEGOTIATED, TG_PREFER_PROTOBUF);
	std::string identity = fixtures::with_uid(options.uid);
	const std::string original = identity;

	options.username = "reader";
	options.password = "password";
	options.identity_xml = view(identity);
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	identity.assign(identity.size(), 'x');
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(
		receive_all(session.value, view(fixtures::offer()), 1, events),
		TG_OK);
	std::string written;

	ASSERT_EQ(write_all(session.value, 2, written), TG_OK);
	size_t credential = written.find("<auth");
	size_t position = written.find(original);
	size_t request = written.find("TakRequest");

	ASSERT_NE(credential, std::string::npos);
	ASSERT_NE(position, std::string::npos);
	ASSERT_NE(request, std::string::npos);
	EXPECT_LT(credential, position);
	EXPECT_LT(position, request);
	EXPECT_TRUE(events.empty());
}

TEST(Maintenance, IdentityWithDifferentUidIsRejectedWithoutCreatingSession)
{
	tg_session_options options = session_options();
	std::string identity = fixtures::with_uid("wrong-source-identity");

	options.identity_xml = view(identity);
	Session session;

	EXPECT_EQ(tg_session_create(&options, nullptr, 0, &session.value),
		TG_INVALID);
	EXPECT_EQ(session.value, nullptr);
}

TEST(Maintenance, FixedProtobufIdentityIsEncodedInCurrentMode)
{
	tg_session_options options = session_options(TG_PROTOCOL_PROTOBUF_ONLY);
	std::string identity = fixtures::with_uid(options.uid);

	options.identity_xml = view(identity);
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	std::string written;

	ASSERT_EQ(write_all(session.value, 1, written), TG_OK);
	ASSERT_FALSE(written.empty());
	EXPECT_EQ(static_cast<uint8_t>(written[0]), 0xbf);
	Framer framer;
	std::vector<fixtures::Bytes> messages;

	ASSERT_EQ(tg_framer_create(
			  TG_ENCODING_PROTOBUF_V1, nullptr, &framer.value),
		TG_OK);
	ASSERT_GE(frame_all(framer.value, view(written), messages), TG_OK);
	ASSERT_EQ(messages.size(), 1u);
	Buffer output;
	tg_normalize_report report{};

	ASSERT_EQ(tg_normalize(TG_ENCODING_PROTOBUF_V1, view(messages[0]),
			  TG_REJECT_LOSSY, nullptr, &output.value, &report),
		TG_OK);
	EXPECT_NE(string(output.value).find(options.uid), std::string::npos);
}

TEST(Maintenance, PingUsesUtcAndTimeoutStartsAfterFinalActualWrite)
{
	tg_session_options options = session_options(TG_PROTOCOL_XML_ONLY);

	options.ping_interval_ms = 10;
	options.ping_timeout_ms = 20;
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	constexpr uint64_t utc = UINT64_C(1792670400000);

	EXPECT_EQ(tg_session_tick(session.value, 9, utc), TG_OK);
	tg_bytes bytes{};

	EXPECT_EQ(tg_session_peek_write(session.value, &bytes), TG_MORE);
	ASSERT_EQ(tg_session_tick(session.value, 10, utc), TG_OK);
	ASSERT_EQ(tg_session_peek_write(session.value, &bytes), TG_OK);
	std::string ping(
		reinterpret_cast<const char *>(bytes.data), bytes.size);

	EXPECT_NE(ping.find("t-x-c-t"), std::string::npos);
	EXPECT_NE(ping.find("2026-10-22"), std::string::npos);
	ASSERT_GT(bytes.size, 1u);
	ASSERT_EQ(tg_session_consume_write(session.value, bytes.size - 1, 11),
		TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 99, utc), TG_OK);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.pings_sent, 0u);
	ASSERT_EQ(tg_session_peek_write(session.value, &bytes), TG_OK);
	ASSERT_EQ(bytes.size, 1u);
	ASSERT_EQ(tg_session_consume_write(session.value, 1, 100), TG_OK);
	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.pings_sent, 1u);
	EXPECT_EQ(info.ping_outstanding, 1);
	EXPECT_EQ(tg_session_tick(session.value, 119, utc), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 120, utc), TG_TIMEOUT);
}

TEST(Maintenance, OfficialUncorrelatedPongSatisfiesOneOutstandingPing)
{
	tg_session_options options = session_options(TG_PROTOCOL_XML_ONLY);

	options.ping_interval_ms = 10;
	options.ping_timeout_ms = 20;
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	ASSERT_EQ(tg_session_tick(session.value, 10, UINT64_C(1792670400000)),
		TG_OK);
	std::string written;

	ASSERT_EQ(write_all(session.value, 10, written), TG_OK);
	std::string pong = fixtures::control_xml("t-x-c-t-r", "", "takPong");
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(session.value, view(pong), 11, events), TG_OK);
	EXPECT_TRUE(events.empty());
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.ping_outstanding, 0);
	EXPECT_EQ(info.pongs_received, 1u);
	EXPECT_EQ(tg_session_tick(session.value, 19, UINT64_C(1792670400000)),
		TG_OK);
	tg_bytes bytes{};

	EXPECT_EQ(tg_session_peek_write(session.value, &bytes), TG_MORE);
}

TEST(Maintenance, PingPausesDuringUpgradeAndResumesInBinaryEncoding)
{
	tg_session_options options =
		session_options(TG_PROTOCOL_NEGOTIATED, TG_PREFER_PROTOBUF);

	options.ping_interval_ms = 10;
	options.ping_timeout_ms = 20;
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	request_upgrade(session, 1);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	ASSERT_EQ(tg_session_tick(session.value, 10, UINT64_C(1792670400000)),
		TG_OK);
	tg_bytes bytes{};

	EXPECT_EQ(tg_session_peek_write(session.value, &bytes), TG_MORE);
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(session.value, view(fixtures::response(true)), 11,
			  events),
		TG_OK);
	ASSERT_EQ(tg_session_tick(session.value, 12, UINT64_C(1792670400000)),
		TG_OK);
	ASSERT_EQ(tg_session_peek_write(session.value, &bytes), TG_OK);
	ASSERT_GT(bytes.size, 0u);
	EXPECT_EQ(bytes.data[0], 0xbf);
}

TEST(Maintenance, BackwardsTimeAndInvalidUtcDoNotQueuePing)
{
	tg_session_options options = session_options();

	options.ping_interval_ms = 10;
	options.ping_timeout_ms = 20;
	Session session;

	ASSERT_EQ(
		tg_session_create(&options, nullptr, 5, &session.value), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 4, UINT64_C(1792670400000)),
		TG_INVALID);
	EXPECT_EQ(tg_session_tick(session.value, 15, UINT64_C(253402300780000)),
		TG_INVALID);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.pending_write_bytes, 0u);
	EXPECT_EQ(info.pings_sent, 0u);
}

static void upgrade_after_partial_ping(Session &session)
{
	tg_session_options options =
		session_options(TG_PROTOCOL_NEGOTIATED, TG_PREFER_PROTOBUF);

	options.ping_interval_ms = 10;
	options.ping_timeout_ms = 20;
	ASSERT_EQ(
		tg_session_create(&options, nullptr, 0, &session.value), TG_OK);
	ASSERT_EQ(tg_session_tick(session.value, 10, UINT64_C(1792670400000)),
		TG_OK);
	tg_bytes ping{};

	ASSERT_EQ(tg_session_peek_write(session.value, &ping), TG_OK);
	ASSERT_GT(ping.size, 1u);
	std::string whole(reinterpret_cast<const char *>(ping.data), ping.size);

	ASSERT_EQ(tg_session_consume_write(session.value, 1, 11), TG_OK);
	tg_bytes remainder{};

	ASSERT_EQ(tg_session_peek_write(session.value, &remainder), TG_OK);
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(
		receive_all(session.value, view(fixtures::offer()), 12, events),
		TG_OK);
	EXPECT_TRUE(events.empty());
	tg_bytes after_offer{};

	ASSERT_EQ(tg_session_peek_write(session.value, &after_offer), TG_OK);
	EXPECT_EQ(after_offer.data, remainder.data);
	EXPECT_EQ(after_offer.size, remainder.size);
	EXPECT_EQ(std::string(reinterpret_cast<const char *>(after_offer.data),
			  after_offer.size),
		whole.substr(1));
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
	EXPECT_EQ(info.protocol_offer_received, 1);
	ASSERT_EQ(tg_session_consume_write(session.value, after_offer.size, 13),
		TG_OK);
	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_UPGRADE_PENDING);
	EXPECT_EQ(info.ping_outstanding, 1);
	EXPECT_EQ(info.pings_sent, 1u);
	std::string request;

	ASSERT_EQ(write_all(session.value, 14, request), TG_OK);
	EXPECT_NE(request.find("TakRequest"), std::string::npos);
	EXPECT_EQ(request.find("t-x-c-t"), std::string::npos);
	tg_bytes next{};

	EXPECT_EQ(tg_session_peek_write(session.value, &next), TG_MORE);
}

TEST(Maintenance, PartialPingFinishesBeforeUpgradeAndXmlPongClearsOnlyPing)
{
	Session session;

	upgrade_after_partial_ping(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	std::vector<fixtures::Bytes> events;
	std::string pong = fixtures::control_xml("t-x-c-t-r", "", "takPong");

	ASSERT_GE(receive_all(session.value, view(pong), 15, events), TG_OK);
	EXPECT_TRUE(events.empty());
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_UPGRADE_PENDING);
	EXPECT_EQ(info.ping_outstanding, 0);
	EXPECT_EQ(info.pongs_received, 1u);
	EXPECT_EQ(
		tg_session_tick(session.value, 65013, UINT64_C(1792670400000)),
		TG_OK);
	tg_bytes next{};

	EXPECT_EQ(tg_session_peek_write(session.value, &next), TG_MORE);
	EXPECT_EQ(
		tg_session_tick(session.value, 65014, UINT64_C(1792670400000)),
		TG_TIMEOUT);
}

TEST(Maintenance, OutstandingPingDeadlineStillExpiresDuringUpgrade)
{
	Session session;

	upgrade_after_partial_ping(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	EXPECT_EQ(tg_session_tick(session.value, 32, UINT64_C(1792670400000)),
		TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 33, UINT64_C(1792670400000)),
		TG_TIMEOUT);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_FAILED);
}

TEST(Maintenance, BinaryPongCanResolvePingSubmittedBeforeUpgrade)
{
	Session session;

	upgrade_after_partial_ping(session);
	ASSERT_FALSE(testing::Test::HasFatalFailure());
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(session.value, view(fixtures::response(true)), 15,
			  events),
		TG_OK);
	fixtures::Bytes extra;

	fixtures::text(extra, 1, "t-x-c-t-r");
	fixtures::text(extra, 5, "takPong");
	fixtures::Bytes pong = fixtures::stream(
		fixtures::envelope(fixtures::event({}, extra)));

	ASSERT_GE(receive_all(session.value, view(pong), 16, events), TG_OK);
	EXPECT_TRUE(events.empty());
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_PROTOBUF);
	EXPECT_EQ(info.ping_outstanding, 0);
	EXPECT_EQ(info.pongs_received, 1u);
	EXPECT_EQ(tg_session_tick(session.value, 22, UINT64_C(1792670400000)),
		TG_OK);
}

TEST(Sessions, TransportWhitespaceDoesNotStartPartialMessageDeadline)
{
	Session session;

	create_session(session);
	std::vector<fixtures::Bytes> events;

	ASSERT_GE(receive_all(session.value, view(" \n\t"), 1, events), TG_OK);
	EXPECT_EQ(tg_session_tick(session.value, 100000, 1700000000000), TG_OK);
	tg_session_info info{};

	ASSERT_EQ(tg_session_snapshot(session.value, &info), TG_OK);
	EXPECT_EQ(info.state, TG_SESSION_XML);
}
