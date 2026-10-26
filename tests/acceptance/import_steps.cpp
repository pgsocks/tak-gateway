#include "../unit/import_fixtures.hpp"
#include <cucumber-cpp/autodetect.hpp>

using namespace contract;
using namespace import_fixtures;
using cucumber::ScenarioScope;

namespace {

struct ImportContext {
	Package fixture;
	Imported imported;
	Config config;
	Runtime runtime;
	std::string path;
	tg_status result = TG_BAD_STATE;
};

GIVEN("^an ATAK package with a client certificate and trusted CA$")
{
	ScenarioScope<ImportContext> context;

	ASSERT_TRUE(context->fixture.prepare());
	context->path = context->fixture.write(
		zip(context->fixture.entries()));
	ASSERT_FALSE(context->path.empty());
}

GIVEN("^an enrollment package wrapped with one supplied client identity$")
{
	ScenarioScope<ImportContext> context;

	ASSERT_TRUE(context->fixture.prepare());
	context->path = context->fixture.wrapped();
	ASSERT_FALSE(context->path.empty());
}

GIVEN("^an enrollment package without a supplied client identity$")
{
	ScenarioScope<ImportContext> context;

	ASSERT_TRUE(context->fixture.prepare());
	context->path = context->fixture.write(
		zip(context->fixture.entries(false)));
	ASSERT_FALSE(context->path.empty());
}

WHEN("^its imported input is composed with a local TCP output$")
{
	ScenarioScope<ImportContext> context;

	ASSERT_EQ(context->imported.read(context->path), TG_OK);
	std::string text = configuration(context->imported.toml.value);
	std::string base = std::filesystem::temp_directory_path().string();

	ASSERT_EQ(tg_config_parse(view(text), base.c_str(),
		&context->config.value, nullptr), TG_OK);
	context->result = tg_runtime_create(context->config.value,
		&context->runtime.value, nullptr);
}

THEN("^the gateway can prepare the imported TLS source$")
{
	ScenarioScope<ImportContext> context;

	ASSERT_EQ(context->result, TG_OK);
	ASSERT_NE(context->runtime.value, nullptr);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(context->runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("READY"), std::string::npos);
	EXPECT_NE(string(snapshot.value).find("atak-0"), std::string::npos);
}

THEN("^no package password appears in its configuration or status$")
{
	ScenarioScope<ImportContext> context;
	Buffer redacted;
	Buffer snapshot;

	ASSERT_EQ(tg_config_redacted(context->config.value, &redacted.value,
		nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_snapshot(context->runtime.value, &snapshot.value),
		TG_OK);
	for (const auto &text : {string(context->imported.toml.value),
		     string(redacted.value), string(snapshot.value)}) {
		EXPECT_EQ(text.find("test-only-secret"), std::string::npos);
		EXPECT_EQ(text.find("PRIVATE KEY"), std::string::npos);
	}
}

WHEN("^the ATAK package import is attempted$")
{
	ScenarioScope<ImportContext> context;

	context->result = context->imported.read(context->path);
}

THEN("^it fails without returning configuration or credential files$")
{
	ScenarioScope<ImportContext> context;

	EXPECT_LT(context->result, TG_OK);
	EXPECT_EQ(context->imported.files, nullptr);
	EXPECT_EQ(context->imported.toml.value.data, nullptr);
	EXPECT_EQ(context->imported.toml.value.size, 0u);
}

} /* namespace */
