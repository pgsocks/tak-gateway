#include "cli_fixtures.hpp"

using namespace contract;
using namespace import_fixtures;
using namespace cli_fixtures;

TEST(CliOptions, ZipAndOutputPrepareRuntimeWithoutAConfigurationFile)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--import-zip", fixture.wrapped(),
		"--output", "capture=tcp://127.0.0.1:19000",
		"--status-file", "status.json", "--log-level", "debug",
		"--log-format", "text", "--max-message-bytes", "1048576",
		"--max-xml-depth", "64", "--retained-bytes", "4194304",
		"--queue-bytes", "2097152", "--queue-records", "128",
		"--max-inputs", "4", "--max-outputs", "8",
		"--protobuf-extra-fields", "reject", "--overflow", "fail"},
		fixture.directory.string()), TG_OK);
	ASSERT_NE(result.config, nullptr);
	ASSERT_NE(result.files, nullptr);
	EXPECT_EQ(result.diagnostic.status, TG_OK);
	EXPECT_STREQ(result.diagnostic.key, "");
	EXPECT_EQ(result.config->input_count, 1u);
	ASSERT_EQ(result.config->output_count, 1u);
	EXPECT_STREQ(result.config->outputs[0].id, "capture");
	EXPECT_STREQ(result.config->outputs[0].bind, "127.0.0.1");
	EXPECT_EQ(result.config->outputs[0].port, 19000);
	EXPECT_STREQ(result.config->log_level, "debug");
	EXPECT_STREQ(result.config->log_format, "text");
	EXPECT_EQ(result.config->limits.max_message_bytes, 1048576u);
	EXPECT_EQ(result.config->limits.max_xml_depth, 64u);
	EXPECT_EQ(result.config->limits.retained_bytes, 4194304u);
	EXPECT_EQ(result.config->limits.queue_bytes, 2097152u);
	EXPECT_EQ(result.config->limits.queue_records, 128u);
	EXPECT_EQ(result.config->max_inputs, 4u);
	EXPECT_EQ(result.config->limits.max_outputs, 8u);
	EXPECT_EQ(result.config->overflow, TG_OVERFLOW_FAIL);
	EXPECT_EQ(result.config->preservation, TG_REJECT_LOSSY);
	EXPECT_EQ(std::filesystem::path(result.config->status_file),
		fixture.directory / "status.json");
	ASSERT_EQ(tg_config_validate(result.config, nullptr), TG_OK);
	Runtime runtime;

	EXPECT_EQ(tg_runtime_create(result.config, &runtime.value, nullptr),
		TG_OK);
}

TEST(CliOptions, OverridesPreserveUnrelatedBaseConfiguration)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--config", base_config(fixture),
		"--log-level", "warning", "--queue-records", "17",
		"--set", "outputs[0].framing=\"xml-document\""},
		fixture.directory.string()), TG_OK);
	ASSERT_EQ(result.config->input_count, 1u);
	ASSERT_EQ(result.config->output_count, 1u);
	EXPECT_STREQ(result.config->inputs[0].id, "udp-source");
	EXPECT_EQ(result.config->inputs[0].port, 18087);
	EXPECT_STREQ(result.config->outputs[0].id, "tcp-consumers");
	EXPECT_EQ(result.config->outputs[0].port, 19000);
	EXPECT_EQ(result.config->outputs[0].delivery.framing,
		TG_FRAMING_XML_DOCUMENT);
	EXPECT_EQ(result.config->limits.queue_records, 17u);
	EXPECT_STREQ(result.config->log_level, "warning");
	EXPECT_EQ(result.files, nullptr);
}

TEST(CliOptions, ImportedInputsAcceptNestedProtocolAndMaintenanceOverrides)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	std::string output_only = "version=1\n[[outputs]]\nid='capture'\n"
		"kind='tcp'\nbind='127.0.0.1'\nport=19000\n";
	std::string path = fixture.write(fixtures::bytes(output_only),
		"output-only.toml");

	ASSERT_EQ(result.read({"--config", path, "--import-zip",
		fixture.wrapped(), "--set", "inputs[0].protocol='negotiated'",
		"--set", "inputs[0].prefer='protobuf'", "--set",
		"inputs[0].maintenance.ping_interval_ms=30000", "--set",
		"inputs[0].maintenance.ping_timeout_ms=10000"},
		fixture.directory.string()), TG_OK);
	ASSERT_EQ(result.config->input_count, 1u);
	ASSERT_EQ(result.config->output_count, 1u);
	EXPECT_EQ(result.config->inputs[0].session.protocol,
		TG_PROTOCOL_NEGOTIATED);
	EXPECT_EQ(result.config->inputs[0].session.preference,
		TG_PREFER_PROTOBUF);
	EXPECT_EQ(result.config->inputs[0].session.ping_interval_ms, 30000u);
	EXPECT_EQ(result.config->inputs[0].session.ping_timeout_ms, 10000u);
}

TEST(CliOptions, OutputIdsReplaceCompleteTablesAndAppendNewOutputs)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--config", base_config(fixture),
		"--output", "tcp-consumers=udp://127.0.0.1:19001",
		"--output", "other=tcp-connect://consumer.invalid:19002"},
		fixture.directory.string()), TG_OK);
	ASSERT_EQ(result.config->output_count, 2u);
	EXPECT_STREQ(result.config->outputs[0].id, "tcp-consumers");
	EXPECT_STREQ(result.config->outputs[0].kind, "udp");
	EXPECT_STREQ(result.config->outputs[0].host, "127.0.0.1");
	EXPECT_EQ(result.config->outputs[0].port, 19001);
	EXPECT_EQ(result.config->outputs[0].delivery.framing,
		TG_FRAMING_XML_DOCUMENT);
	EXPECT_STREQ(result.config->outputs[1].kind, "tcp");
	EXPECT_STREQ(result.config->outputs[1].mode, "connect");
	EXPECT_STREQ(result.config->outputs[1].host, "consumer.invalid");
	EXPECT_EQ(result.config->outputs[1].port, 19002);
}

TEST(CliOptions, RepeatedOutputIdsUseTheLastCompleteReplacement)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--config", base_config(fixture),
		"--output", "tcp-consumers=tcp://127.0.0.1:19001",
		"--set", "outputs[0].framing=\"xml-document\"",
		"--output", "tcp-consumers=tcp://127.0.0.1:19002"},
		fixture.directory.string()), TG_OK);
	ASSERT_EQ(result.config->output_count, 1u);
	EXPECT_EQ(result.config->outputs[0].port, 19002);
	EXPECT_EQ(result.config->outputs[0].delivery.framing, TG_FRAMING_U32BE);
}

TEST(CliOptions, BracketedIpv6AndPortBoundsRemainUsable)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string config = base_config(fixture);

	for (const std::string port : {"1", "65535"}) {
		Configured result;

		ASSERT_EQ(result.read({"--config", config, "--output",
			"tcp-consumers=tcp://[::1]:" + port},
			fixture.directory.string()), TG_OK);
		ASSERT_EQ(result.config->output_count, 1u);
		EXPECT_STREQ(result.config->outputs[0].bind, "::1");
		EXPECT_EQ(result.config->outputs[0].port,
			static_cast<uint16_t>(std::stoul(port)));
	}
}

TEST(CliOptions, ScalarAndGeneralSettersFollowCommandLineOrder)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--log-level", "debug",
		"--config", base_config(fixture), "--log-level", "warning",
		"--set", "process.log_level=\"error\"",
		"--queue-records", "11", "--set", "limits.queue_records=12",
		"--queue-records", "13", "--set", "outputs[0].port=19003"},
		fixture.directory.string()), TG_OK);
	EXPECT_STREQ(result.config->log_level, "error");
	EXPECT_EQ(result.config->limits.queue_records, 13u);
	EXPECT_EQ(result.config->outputs[0].port, 19003);
}

TEST(CliOptions, GeneralSettersCreateTablesArraysAndNestedValues)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--set",
		"inputs=[{id='receiver',kind='udp',port=18087}]",
		"--set", "inputs[0].allow_peers=['127.0.0.1/32','::1/128']",
		"--output", "capture=tcp://127.0.0.1:19000", "--set",
		"process={log_format='text',log_level='warning'}",
		"--set", "outputs[0].enabled=true",
		"--set", "outputs[0].max_clients=7"},
		fixture.directory.string()), TG_OK);
	ASSERT_EQ(result.config->input_count, 1u);
	EXPECT_STREQ(result.config->inputs[0].id, "receiver");
	ASSERT_EQ(result.config->inputs[0].peer_count, 2u);
	EXPECT_STREQ(result.config->inputs[0].allow_peers[0], "127.0.0.1/32");
	EXPECT_STREQ(result.config->inputs[0].allow_peers[1], "::1/128");
	EXPECT_STREQ(result.config->log_level, "warning");
	EXPECT_STREQ(result.config->log_format, "text");
	EXPECT_EQ(result.config->outputs[0].max_clients, 7u);
	EXPECT_EQ(tg_config_validate(result.config, nullptr), TG_OK);
}

TEST(CliOptions, IndexedSettersCanBuildDenseEndpointArraysWithoutToml)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(result.read({"--set", "inputs[0].id='receiver'",
		"--set", "inputs[0].kind='udp'",
		"--set", "inputs[0].port=18087",
		"--set", "outputs[0]={id='capture',kind='tcp',port=19000}",
		"--set", "outputs[1]={id='udp',kind='udp',port=19001}"},
		fixture.directory.string()), TG_OK);
	EXPECT_EQ(result.config->input_count, 1u);
	EXPECT_EQ(result.config->output_count, 2u);
}

#ifndef _WIN32
TEST(CliOptions, AdvancedOutputSettingsUseTheOrdinarySchema)
{
	Package fixture;
	Configured result;

	ASSERT_TRUE(fixture.prepare());
	std::string pipe = (fixture.directory / "events.fifo").generic_string();

	ASSERT_EQ(result.read({"--config", base_config(fixture),
		"--output", "network=tcp-connect://127.0.0.1:19001",
		"--set", "outputs[1].framing='xml-document'",
		"--set", "outputs[1].queue_bytes=12345",
		"--output", "datagrams=udp://[::1]:19002",
		"--set", "outputs[2].max_datagram_bytes=1200",
		"--output", "fifo=pipe://" + pipe,
		"--set", "outputs[3].create=true",
		"--set", "outputs[3].permissions='0640'"},
		fixture.directory.string()), TG_OK);
	ASSERT_EQ(result.config->output_count, 4u);
	EXPECT_EQ(result.config->outputs[1].delivery.framing,
		TG_FRAMING_XML_DOCUMENT);
	EXPECT_EQ(result.config->outputs[1].delivery.queue_bytes, 12345u);
	EXPECT_STREQ(result.config->outputs[2].host, "::1");
	EXPECT_EQ(result.config->outputs[2].delivery.max_datagram_bytes, 1200u);
	EXPECT_STREQ(result.config->outputs[3].kind, "pipe");
	EXPECT_EQ(std::filesystem::path(result.config->outputs[3].path),
		std::filesystem::path(pipe));
	EXPECT_TRUE(result.config->outputs[3].create);
	EXPECT_EQ(result.config->outputs[3].permissions, 0640u);
}
#endif

TEST(CliOptions, RelativePathsUseConfigurationDirectoryOrCwd)
{
	Package fixture;
	Configured with_config;
	Configured without_config;

	ASSERT_TRUE(fixture.prepare());
	std::filesystem::path nested = fixture.directory / "nested";

	ASSERT_TRUE(std::filesystem::create_directory(nested));
	fixture.write(fixtures::bytes(fixtures::minimal_config()),
		"nested/gateway.toml");
	ASSERT_EQ(with_config.read({"--config", "nested/gateway.toml",
		"--status-file", "status.json"},
		fixture.directory.string()), TG_OK);
	EXPECT_EQ(std::filesystem::path(with_config.config->status_file),
		nested / "status.json");
	fixture.wrapped();
	ASSERT_EQ(without_config.read({"--import-zip", "config.zip",
		"--output", "capture=tcp://127.0.0.1:19000",
		"--status-file", "status.json"},
		fixture.directory.string()), TG_OK);
	EXPECT_EQ(std::filesystem::path(without_config.config->status_file),
		fixture.directory / "status.json");
}

static void rejected_options(const std::vector<std::string> &arguments,
	const std::string &directory)
{
	Configured result;

	EXPECT_LT(result.read(arguments, directory), TG_OK);
	EXPECT_EQ(result.config, nullptr);
	EXPECT_EQ(result.files, nullptr);
	EXPECT_LT(result.diagnostic.status, TG_OK);
	EXPECT_EQ(std::string(result.diagnostic.key).find("test-only-secret"),
		std::string::npos);
}

TEST(CliOptions, RejectsMissingOrUnknownFlagsAndDuplicateFileSelectors)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string config = base_config(fixture);
	std::vector<std::vector<std::string>> invalid = {
		{}, {"--config"}, {"--unknown", "value"}, {"--output", ""},
		{"--config", ""}, {"--import-zip", ""},
		{"--log-level", ""}, {"--config", config, "--config", config},
		{"--import-zip", "a.zip", "--import-zip", "b.zip"}};

	for (const auto &arguments : invalid)
		rejected_options(arguments, fixture.directory.string());
}

TEST(CliOptions, RejectsMalformedOutputAddressesAndPorts)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string config = base_config(fixture);

	for (const std::string output : {"missing-id", "=tcp://127.0.0.1:1",
		     "bad=http://127.0.0.1:19000", "bad=tcp://:19000",
		     "bad=tcp://127.0.0.1:0", "bad=udp://127.0.0.1:65536",
		     "bad=tcp://127.0.0.1:abc", "bad=tcp://::1:19000",
		     "bad=tcp://[::1:19000", "bad=tcp://[not-ip]:19000",
		     "bad=tcp://[::1]:19000/path",
		     "bad=tcp://user@localhost:19000",
		     "bad=udp://localhost:19000?token=secret",
		     "bad=tcp://localhost:19000#fragment",
		     "bad=pipe:///tmp/a%20b",
		     "bad=pipe://relative-path"}) {
		SCOPED_TRACE(output);
		rejected_options({"--config", config, "--output", output},
			fixture.directory.string());
	}
}

TEST(CliOptions, RejectsMalformedPathsValuesAndSchemaViolations)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string config = base_config(fixture);

	for (const std::string setting : {"no-equals", "=1", "process..x=1",
		     "outputs[-1].port=19000", "outputs[3].port=19000",
		     "outputs[0]port=19000", "outputs[0].port='19000'",
		     "process.log_level=unknown", "process.log_level='invalid'",
		     "outputs[0].mystery=true", "limits.queue_records=0",
		     "limits.queue_records=1\nversion=1", "outputs[0].port="}) {
		SCOPED_TRACE(setting);
		rejected_options({"--config", config, "--set", setting},
			fixture.directory.string());
	}
}

TEST(CliOptions, NumericFlagsRequireValidPositiveDecimalValues)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string config = base_config(fixture);

	for (const std::string number : {"0", "-1", "1e3", "1_000",
		     "184467440737095516160", "not-a-number"}) {
		SCOPED_TRACE(number);
		rejected_options({"--config", config,
			"--queue-records", number},
			fixture.directory.string());
	}
}

TEST(CliOptions, MissingInputsOrOutputsFailTheRegularSchema)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	rejected_options({"--output", "capture=tcp://127.0.0.1:19000"},
		fixture.directory.string());
	rejected_options({"--import-zip", fixture.wrapped()},
		fixture.directory.string());
}

TEST(CliOptions, InvalidApiArgumentsDoNotPublishConfiguration)
{
	tg_config *config = nullptr;
	tg_import_files *files = nullptr;
	tg_diagnostic diagnostic{};
	const char *arguments[] = {"--log-level", "debug"};

	EXPECT_EQ(tg_cli_configure(-1, arguments, base_directory(), &config,
		&files, &diagnostic), TG_INVALID);
	EXPECT_EQ(tg_cli_configure(2, nullptr, base_directory(), &config,
		&files, &diagnostic), TG_INVALID);
	EXPECT_EQ(tg_cli_configure(2, arguments, "relative", &config,
		&files, &diagnostic), TG_INVALID);
	EXPECT_EQ(tg_cli_configure(2, arguments, nullptr, &config,
		&files, &diagnostic), TG_INVALID);
	EXPECT_EQ(tg_cli_configure(2, arguments, base_directory(), nullptr,
		&files, &diagnostic), TG_INVALID);
	EXPECT_EQ(tg_cli_configure(2, arguments, base_directory(), &config,
		nullptr, &diagnostic), TG_INVALID);
	EXPECT_EQ(config, nullptr);
	EXPECT_EQ(files, nullptr);
}

TEST(CliOptions, DeepRawArraysAreRejectedBeforeTomlParserRecursion)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	for (size_t depth : {size_t(33), size_t(50000)}) {
		Configured result;
		std::string assignment = "inputs=" + std::string(depth, '[') +
			"0" + std::string(depth, ']');

		EXPECT_EQ(result.read({"--set", assignment},
			fixture.directory.string()), TG_LIMIT);
		EXPECT_EQ(result.config, nullptr);
		EXPECT_EQ(result.files, nullptr);
		EXPECT_EQ(result.diagnostic.status, TG_LIMIT);
	}
}

TEST(CliOptions, DeepDottedKeysAreRejectedBeforeTomlParserRecursion)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	for (size_t depth : {size_t(32), size_t(100000)}) {
		for (const std::string part : {"a.", "'a'.", "\"a\"."}) {
			Configured result;
			std::string source = "process={";

			for (size_t i = 0; i < depth; ++i)
				source += part;
			source += "z=0}\n";
			std::string path = fixture.write(
				fixtures::bytes(source), "deep.toml");

			EXPECT_EQ(result.read({"--config", path},
				fixture.directory.string()), TG_LIMIT);
			EXPECT_EQ(result.config, nullptr);
			EXPECT_EQ(result.files, nullptr);
			EXPECT_EQ(result.diagnostic.status, TG_LIMIT);
		}
	}
}

TEST(CliOptions, NestingPreflightIgnoresStringsAndComments)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string punctuation(100, '[');

	punctuation += std::string(100, '{') + std::string(100, '.') +
		std::string(100, ']') + std::string(100, '}');
	std::vector<std::string> values = {"\"" + punctuation + "\"",
		"'" + punctuation + "'", "\"\"\"" + punctuation + "\"\"\"",
		"\"\"\"" + punctuation + "\"\"\"\"",
		"\"\"\"" + punctuation + "\"\"\"\"\"",
		"'''" + punctuation + "'''", "'''" + punctuation + "''''",
		"'''" + punctuation + "'''''",
		"\"escaped \\\" " + punctuation + "\""};

	for (const auto &value : values) {
		Configured result;
		std::string source = fixtures::minimal_config() +
			"\n# " + punctuation + "\n[process]\nstatus_file=" +
			value + " # " + punctuation + "\n";
		std::string path = fixture.write(fixtures::bytes(source),
			"strings.toml");

		ASSERT_EQ(result.read({"--config", path, "--set",
			"process.status_file=" + value},
			fixture.directory.string()), TG_OK);
		ASSERT_NE(result.config, nullptr);
		EXPECT_NE(std::string(result.config->status_file).find(
			punctuation), std::string::npos);
	}
}

TEST(CliOptions, MultilineClosingQuotesCannotHideFollowingDeepValues)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	for (char quote : {'\'', '"'}) {
		for (size_t closing : {size_t(3), size_t(4), size_t(5)}) {
			Configured result;
			std::string value = "process={status_file=" +
				std::string(3, quote) + "quoted" +
				std::string(closing, quote) + ",log_level=" +
				std::string(50000, '[') + "0" +
				std::string(50000, ']') + "}";

			EXPECT_EQ(result.read({"--set", value},
				fixture.directory.string()), TG_LIMIT);
			EXPECT_EQ(result.config, nullptr);
			EXPECT_EQ(result.files, nullptr);
		}
	}
}

#ifndef _WIN32
TEST(CliOptions, SchemaFailureAfterImportRemovesAllTemporaryCredentials)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string path = fixture.wrapped();
	std::filesystem::path temporary = fixture.directory / "temporary";

	ASSERT_TRUE(std::filesystem::create_directory(temporary));
	const char *previous = getenv("TMPDIR");
	bool had_previous = previous != nullptr;
	std::string previous_value = previous ? previous : "";

	ASSERT_EQ(setenv("TMPDIR", temporary.string().c_str(), 1), 0);
	rejected_options({"--import-zip", path, "--output",
		"capture=tcp://127.0.0.1:19000", "--set",
		"inputs[0].protocol='unsupported'"},
		fixture.directory.string());
	int restored = had_previous ?
		setenv("TMPDIR", previous_value.c_str(), 1) :
		unsetenv("TMPDIR");

	EXPECT_EQ(restored, 0);
	EXPECT_TRUE(std::filesystem::is_empty(temporary));
}
#endif
