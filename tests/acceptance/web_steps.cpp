#include "../unit/web_fixtures.hpp"
#include <cucumber-cpp/autodetect.hpp>

using namespace contract;
using namespace web_fixtures;
using cucumber::ScenarioScope;

namespace {

struct WebContext {
	Gateway gateway;
	runtime_fixtures::Socket sender;
	runtime_fixtures::Socket consumer;
	uint16_t input_port = 0;
	uint16_t output_port = 0;
	Response preview;
	Response conflict;
};

static void require_commit(Gateway &gateway, const Response &accepted)
{
	ASSERT_EQ(accepted.status, 202);
	Response response = gateway.wait(accepted, "committed");
	Json job(response.body);

	ASSERT_EQ(text(field(job.root(), "state")), "committed");
	ASSERT_EQ(gateway.state().status, 200);
}

GIVEN("^a local management server with an empty protected state store$")
{
	ScenarioScope<WebContext> context;

	ASSERT_TRUE(context->gateway.prepare());
	ASSERT_EQ(context->gateway.state().status, 200);
	ASSERT_EQ(context->gateway.revision, 0u);
}

WHEN("^HTTP requests create a UDP input and TCP output$")
{
	ScenarioScope<WebContext> context;

	context->input_port = free_port(SOCK_DGRAM);
	context->output_port = free_port(SOCK_STREAM);
	ASSERT_NE(context->input_port, 0);
	ASSERT_NE(context->output_port, 0);
	Response input = context->gateway.request("POST", "/api/inputs",
		context->gateway.edit("input", udp_input("source",
			context->input_port), context->gateway.revision));

	require_commit(context->gateway, input);
	Response output = context->gateway.request("POST", "/api/outputs",
		context->gateway.edit("output", tcp_output("consumer",
			context->output_port), context->gateway.revision));

	require_commit(context->gateway, output);
}

THEN("^a downstream consumer receives an exact event from that UDP input$")
{
	ScenarioScope<WebContext> context;
	bool listening = false;
	auto initialization = std::chrono::steady_clock::now() +
		std::chrono::seconds(3);

	while (!listening &&
		std::chrono::steady_clock::now() < initialization) {
		Response response = context->gateway.state();
		Json state(response.body);
		yyjson_val *status = field(state.root(), "status");
		yyjson_val *outputs = field(status, "outputs");
		yyjson_val *output = yyjson_arr_get(outputs, 0);
		yyjson_val *input = yyjson_arr_get(field(status, "inputs"), 0);

		listening = yyjson_get_bool(field(output, "available")) &&
			text(field(input, "state")) == "LISTENING";
		if (!listening)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(listening);

	ASSERT_TRUE(context->consumer.create(SOCK_STREAM));
	sockaddr_in output = runtime_fixtures::loopback(context->output_port);

	ASSERT_EQ(connect(context->consumer.value,
		reinterpret_cast<sockaddr *>(&output), sizeof(output)), 0);
	ASSERT_TRUE(context->consumer.nonblocking());
	bool connected = false;
	auto ready = std::chrono::steady_clock::now() +
		std::chrono::seconds(3);

	while (!connected && std::chrono::steady_clock::now() < ready) {
		Response response = context->gateway.state();
		Json state(response.body);
		yyjson_val *outputs = field(field(state.root(), "status"),
			"outputs");

		connected = yyjson_get_uint(field(yyjson_arr_get(outputs, 0),
			"clients")) == 1;
		if (!connected)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(connected);
	ASSERT_TRUE(context->sender.create(SOCK_DGRAM));
	sockaddr_in input = runtime_fixtures::loopback(context->input_port);
	std::string event = fixtures::extended_xml();

	ASSERT_EQ(sendto(context->sender.value, event.data(),
		static_cast<int>(event.size()), 0,
		reinterpret_cast<sockaddr *>(&input), sizeof(input)),
		static_cast<int>(event.size()));
	std::string received;
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);

	while (std::chrono::steady_clock::now() < end &&
		received.size() < record(event).size()) {
		char buffer[16384];
		int count = static_cast<int>(recv(context->consumer.value,
			buffer, sizeof(buffer), 0));

		if (count > 0)
			received.append(buffer, static_cast<size_t>(count));
		else
			ASSERT_TRUE(count < 0 &&
				runtime_fixtures::would_block());
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	EXPECT_EQ(received, record(event));
}

WHEN("^another browser submits an output change using the initial revision$")
{
	ScenarioScope<WebContext> context;

	context->conflict = context->gateway.request("PUT",
		"/api/outputs/consumer", context->gateway.edit("output",
			tcp_output("consumer", free_port(SOCK_STREAM)), 0));
}

THEN("^the stale change is rejected and the committed revision is retained$")
{
	ScenarioScope<WebContext> context;

	EXPECT_EQ(context->conflict.status, 409);
	ASSERT_EQ(context->gateway.state().status, 200);
	EXPECT_EQ(context->gateway.revision, 2u);
}

WHEN("^an ATAK package is uploaded over HTTP for preview$")
{
	ScenarioScope<WebContext> context;
	std::string archive = import_fixtures::read_file(
		context->gateway.fixture.wrapped());
	Response accepted = context->gateway.request("POST", "/api/import",
		archive, true, "application/zip");

	ASSERT_EQ(accepted.status, 202);
	context->preview = context->gateway.wait(accepted, "ready");
}

THEN("^its input is shown without credentials and is not yet configured$")
{
	ScenarioScope<WebContext> context;
	Json job(context->preview.body);

	ASSERT_EQ(text(field(job.root(), "state")), "ready") <<
		context->preview.body;
	EXPECT_EQ(context->preview.body.find("test-only-secret"),
		std::string::npos);
	EXPECT_EQ(context->preview.body.find("PRIVATE KEY"), std::string::npos);
	EXPECT_EQ(yyjson_arr_size(field(field(job.root(), "preview"),
		"inputs")), 1u);
	Response response = context->gateway.state();
	Json state(response.body);

	EXPECT_EQ(context->gateway.revision, 0u);
	EXPECT_EQ(yyjson_arr_size(field(field(state.root(), "config"),
		"inputs")), 0u);
}

} /* namespace */
