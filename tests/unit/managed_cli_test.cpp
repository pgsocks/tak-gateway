#include "cli_fixtures.hpp"
#include "../../src/managed_runtime.h"

using namespace contract;
using namespace import_fixtures;

static tg_status compose(const std::string &base,
	const std::vector<std::string> &arguments, const std::string &cwd,
	Config &config, tg_web_settings &web)
{
	std::vector<const char *> pointers;
	tg_import_files *files = nullptr;

	for (const auto &argument : arguments)
		pointers.push_back(argument.c_str());
	tg_status status = tg_cli_compose(view(base), (int)pointers.size(),
		pointers.data(), cwd.c_str(), true, &config.value, &files, &web,
		nullptr);

	tg_import_zip_destroy(files);
	return status;
}

TEST(ManagedCli, EmptyManagedConfigurationKeepsTheWebControllerAvailable)
{
	Package fixture;
	Config config;
	tg_web_settings web{};

	ASSERT_TRUE(fixture.prepare());
	tg_web_settings_default(&web);
	ASSERT_EQ(compose("", {}, fixture.directory.string(), config, web),
		TG_OK);
	EXPECT_EQ(config.value->input_count, 0u);
	EXPECT_EQ(config.value->output_count, 0u);
	EXPECT_FALSE(web.enabled);
}

TEST(ManagedCli, StoredGatewaySettingsAreOverlaidAndWebSettingsStaySeparate)
{
	Package fixture;
	Config config;
	tg_web_settings web{};

	ASSERT_TRUE(fixture.prepare());
	tg_web_settings_default(&web);
	web.enabled = true;
	snprintf(web.bind, sizeof(web.bind), "0.0.0.0:18080");
	ASSERT_EQ(compose(fixtures::minimal_config(), {"--log-level", "debug",
		"--set", "inputs[0].name='Friendly receiver'"},
		fixture.directory.string(), config, web), TG_OK);
	EXPECT_STREQ(config.value->log_level, "debug");
	EXPECT_STREQ(config.value->inputs[0].name, "Friendly receiver");
	EXPECT_TRUE(web.enabled);
	EXPECT_STREQ(web.bind, "0.0.0.0:18080");
}

TEST(ManagedCli, WebFlagsAndGeneralSettersRespectCommandOrder)
{
	Package fixture;
	Config config;
	tg_web_settings web{};

	ASSERT_TRUE(fixture.prepare());
	tg_web_settings_default(&web);
	ASSERT_EQ(compose("", {"--web-bind", "0.0.0.0:18080",
		"--set", "web.enabled=false", "--set",
		"web.bind='127.0.0.1:19080'"}, fixture.directory.string(),
		config, web), TG_OK);
	EXPECT_FALSE(web.enabled);
	EXPECT_STREQ(web.bind, "127.0.0.1:19080");
}

TEST(ManagedCli, ReplacementFilePreservesOmittedSavedWebSettings)
{
	Package fixture;
	Config config;
	tg_web_settings web{};

	ASSERT_TRUE(fixture.prepare());
	std::string path = fixture.write(fixtures::bytes(
		"version=1\n[process]\nlog_level='error'\n"), "replace.toml");

	tg_web_settings_default(&web);
	web.enabled = true;
	ASSERT_EQ(compose(fixtures::minimal_config(), {"--config", path},
		fixture.directory.string(), config, web), TG_OK);
	EXPECT_EQ(config.value->input_count, 0u);
	EXPECT_STREQ(config.value->log_level, "error");
	EXPECT_TRUE(web.enabled);
}

TEST(ManagedCli, FileWebFieldsAreRemovedBeforeGatewaySchemaValidation)
{
	Package fixture;
	Config config;
	tg_web_settings web{};

	ASSERT_TRUE(fixture.prepare());
	tg_web_settings_default(&web);
	ASSERT_EQ(compose("version=1\n[web]\nenabled=true\n"
		"bind='0.0.0.0:18080'\n", {}, fixture.directory.string(),
		config, web), TG_OK);
	EXPECT_TRUE(web.enabled);
	EXPECT_STREQ(web.bind, "0.0.0.0:18080");
}

TEST(ManagedCli, InvalidWebFieldsDoNotMutateCallerSettings)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	for (const auto &setting : {"web.enabled='true'", "web.mystery=1",
		     "web.bind=''", "web.bind='127.0.0.1:0'"}) {
		Config config;
		tg_web_settings web{};

		tg_web_settings_default(&web);
		EXPECT_LT(compose("", {"--set", setting},
			fixture.directory.string(), config, web), TG_OK);
		EXPECT_EQ(config.value, nullptr);
		EXPECT_FALSE(web.enabled);
		EXPECT_STREQ(web.bind, "127.0.0.1:8080");
	}
}

TEST(ManagedCli, RepeatedZipImportsAllocateFreshConnectionIds)
{
	Package fixture;
	Config first, second;
	Buffer document;
	tg_web_settings web{};

	ASSERT_TRUE(fixture.prepare());
	std::string package = fixture.wrapped();

	tg_web_settings_default(&web);
	ASSERT_EQ(compose("", {"--import-zip", package},
		fixture.directory.string(), first, web), TG_OK);
	ASSERT_EQ(tg_config_export(first.value, &document.value, nullptr),
		TG_OK);
	ASSERT_EQ(compose(string(document.value), {"--import-zip", package},
		fixture.directory.string(), second, web), TG_OK);
	ASSERT_EQ(second.value->input_count, 2u);
	EXPECT_STREQ(second.value->inputs[0].id, "atak-0");
	EXPECT_STREQ(second.value->inputs[1].id, "atak-1");
	EXPECT_STREQ(second.value->inputs[1].session.uid, "atak-1");
}
