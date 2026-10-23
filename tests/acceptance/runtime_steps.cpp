#include "../unit/runtime_fixtures.hpp"
#include <cucumber-cpp/autodetect.hpp>
#include <regex>

using namespace contract;
using cucumber::ScenarioScope;

namespace {

struct RuntimeContext {
	runtime_fixtures::GatewayFixture fixture;
	std::string xml = fixtures::extended_xml();
	Buffer snapshot;
};

GIVEN("^a runtime with a loopback UDP source and a TCP listener$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_TRUE(c->fixture.prepare());
	ASSERT_EQ(c->fixture.create(), TG_OK);
	ASSERT_EQ(c->fixture.start(), TG_OK);
}

GIVEN("^a downstream TCP consumer is connected$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_TRUE(c->fixture.connect_consumer());
}

WHEN("^an XML event with custom fields is sent by UDP$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_TRUE(c->fixture.send(c->xml));
}

THEN("^the TCP consumer receives its exact u32be XML record$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_TRUE(c->fixture.await_bytes(record(c->xml).size()));
	EXPECT_EQ(c->fixture.received, record(c->xml));
}

WHEN("^a malformed UDP document is followed by a valid event$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_TRUE(c->fixture.send("<event><broken></event>"));
	ASSERT_TRUE(c->fixture.send(c->xml));
}

THEN("^the TCP consumer receives only the valid complete record$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_TRUE(c->fixture.await_bytes(record(c->xml).size()));
	EXPECT_EQ(c->fixture.received, record(c->xml));
}

WHEN("^local status is read without sending an event$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_EQ(tg_runtime_snapshot(c->fixture.runtime.value,
		&c->snapshot.value), TG_OK);
}

THEN("^the snapshot identifies the running source and available output$")
{
	ScenarioScope<RuntimeContext> c;
	std::string json = string(c->snapshot.value);

	EXPECT_NE(json.find("RUNNING"), std::string::npos);
	EXPECT_NE(json.find("LISTENING"), std::string::npos);
	EXPECT_NE(json.find("capture"), std::string::npos);
	EXPECT_TRUE(std::regex_search(json, std::regex(
		"\"available\"[[:space:]]*:[[:space:]]*true")));
}

THEN("^the snapshot contains no event payload$")
{
	ScenarioScope<RuntimeContext> c;

	EXPECT_EQ(string(c->snapshot.value).find("fixture-1"),
		std::string::npos);
	EXPECT_EQ(string(c->snapshot.value).find("<event"), std::string::npos);
}

WHEN("^an immediate stop is requested twice$")
{
	ScenarioScope<RuntimeContext> c;

	ASSERT_EQ(tg_runtime_stop(c->fixture.runtime.value, 0), TG_OK);
	ASSERT_EQ(tg_runtime_stop(c->fixture.runtime.value, 5000), TG_OK);
}

THEN("^the runtime reaches STOPPED and polling remains harmless$")
{
	ScenarioScope<RuntimeContext> c;

	EXPECT_EQ(tg_runtime_poll(c->fixture.runtime.value, 0), TG_OK);
	ASSERT_EQ(tg_runtime_snapshot(c->fixture.runtime.value,
		&c->snapshot.value), TG_OK);
	EXPECT_NE(string(c->snapshot.value).find("STOPPED"), std::string::npos);
}

} /* namespace */
