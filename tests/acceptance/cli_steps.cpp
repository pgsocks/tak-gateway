#include "../unit/cli_fixtures.hpp"
#include <cucumber-cpp/autodetect.hpp>

using namespace contract;
using namespace import_fixtures;
using namespace cli_fixtures;
using cucumber::ScenarioScope;

namespace {

struct CliContext {
	Package fixture;
	Configured configured;
	Runtime runtime;
	std::string path;
	tg_status status = TG_BAD_STATE;
};

GIVEN("^an ATAK zip and no local TOML configuration$")
{
	ScenarioScope<CliContext> context;

	ASSERT_TRUE(context->fixture.prepare());
	context->path = context->fixture.wrapped();
	ASSERT_FALSE(context->path.empty());
}

WHEN("^arguments select a TCP output and bounded delivery queues$")
{
	ScenarioScope<CliContext> context;

	context->status = context->configured.read({"--import-zip",
		context->path,
		"--output", "capture=tcp://127.0.0.1:19000",
		"--queue-records", "256", "--overflow", "fail"},
		context->fixture.directory.string());
}

THEN("^the composed gateway can prepare its TLS source and TCP output$")
{
	ScenarioScope<CliContext> context;

	ASSERT_EQ(context->status, TG_OK);
	ASSERT_NE(context->configured.config, nullptr);
	ASSERT_EQ(context->configured.config->input_count, 1u);
	ASSERT_EQ(context->configured.config->output_count, 1u);
	EXPECT_STREQ(context->configured.config->inputs[0].kind, "tls");
	EXPECT_STREQ(context->configured.config->outputs[0].kind, "tcp");
	ASSERT_EQ(tg_config_validate(context->configured.config, nullptr),
		TG_OK);
	EXPECT_EQ(tg_runtime_create(context->configured.config,
		&context->runtime.value, nullptr), TG_OK);
}

THEN("^its queue limit and overflow policy match the arguments$")
{
	ScenarioScope<CliContext> context;

	ASSERT_NE(context->configured.config, nullptr);
	EXPECT_EQ(context->configured.config->limits.queue_records, 256u);
	EXPECT_EQ(context->configured.config->overflow, TG_OVERFLOW_FAIL);
}

GIVEN("^a saved UDP gateway configuration$")
{
	ScenarioScope<CliContext> context;

	ASSERT_TRUE(context->fixture.prepare());
	context->path = base_config(context->fixture);
	ASSERT_FALSE(context->path.empty());
}

WHEN("^arguments replace its output and select XML document framing$")
{
	ScenarioScope<CliContext> context;

	context->status = context->configured.read({"--config", context->path,
		"--output", "tcp-consumers=tcp-connect://127.0.0.1:19001",
		"--set", "outputs[0].framing='xml-document'"},
		context->fixture.directory.string());
}

THEN("^its saved input and replacement output are both retained$")
{
	ScenarioScope<CliContext> context;

	ASSERT_EQ(context->status, TG_OK);
	ASSERT_NE(context->configured.config, nullptr);
	ASSERT_EQ(context->configured.config->input_count, 1u);
	ASSERT_EQ(context->configured.config->output_count, 1u);
	EXPECT_STREQ(context->configured.config->inputs[0].id, "udp-source");
	EXPECT_EQ(context->configured.config->inputs[0].port, 18087);
	EXPECT_STREQ(context->configured.config->outputs[0].mode, "connect");
	EXPECT_EQ(context->configured.config->outputs[0].port, 19001);
	EXPECT_EQ(context->configured.config->outputs[0].delivery.framing,
		TG_FRAMING_XML_DOCUMENT);
}

WHEN("^arguments select an invalid imported protocol$")
{
	ScenarioScope<CliContext> context;

	context->status = context->configured.read({"--import-zip",
		context->path,
		"--output", "capture=tcp://127.0.0.1:19000",
		"--set", "inputs[0].protocol='unsupported'"},
		context->fixture.directory.string());
}

THEN("^command line configuration fails without publishing resources$")
{
	ScenarioScope<CliContext> context;

	EXPECT_LT(context->status, TG_OK);
	EXPECT_EQ(context->configured.config, nullptr);
	EXPECT_EQ(context->configured.files, nullptr);
}

} /* namespace */
