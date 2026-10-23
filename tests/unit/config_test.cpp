#include "contract_support.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>

using namespace contract;

static std::string config_base_directory()
{
	return std::filesystem::temp_directory_path().string();
}

static std::string udp_config(
	const std::string &port = "8087", const std::string &extra_input = "")
{
	return "version = 1\n"
	       "[[inputs]]\nid = 'receiver'\nkind = 'udp'\n"
	       "bind = '127.0.0.1'\nport = " +
	       port + "\n" + extra_input +
	       "[[outputs]]\nid = 'collector'\nkind = 'tcp'\n"
	       "mode = 'listen'\nbind = '127.0.0.1'\nport = 9000\n";
}

static std::string tls_config(
	const std::string &extra_tls = "", const std::string &extra_tables = "")
{
	return "version = 1\n"
	       "[[inputs]]\nid = 'server'\nkind = 'tls'\n"
	       "host = '127.0.0.1'\nport = 8089\n"
	       "protocol = 'xml-only'\nuid = 'gateway-reader'\n"
	       "[inputs.tls]\nserver_name = 'takserver'\n"
	       "ca_file = 'contract-ca.pem'\n" +
	       extra_tls + extra_tables +
	       "[[outputs]]\nid = 'collector'\nkind = 'tcp'\n"
	       "mode = 'listen'\nbind = '127.0.0.1'\nport = 9000\n";
}

static tg_status parse(const std::string &toml, Config &config,
	tg_diagnostic *diagnostic = nullptr)
{
	std::string base = config_base_directory();

	return tg_config_parse(
		view(toml), base.c_str(), &config.value, diagnostic);
}

static std::string replace_one(std::string text, const std::string &old,
	const std::string &replacement)
{
	size_t offset = text.find(old);

	if (offset == std::string::npos)
		return {};
	text.replace(offset, old.size(), replacement);
	return text;
}

TEST(Configuration, ParsesStrictUdpToTcpWithoutExternalState)
{
	Config config;
	tg_diagnostic diagnostic{};

	diagnostic.status = TG_IO;
	diagnostic.line = 99;
	diagnostic.column = 88;
	strcpy(diagnostic.key, "previous-error");
	ASSERT_EQ(parse(udp_config(), config, &diagnostic), TG_OK);
	ASSERT_NE(config.value, nullptr);
	EXPECT_EQ(diagnostic.status, TG_OK);
	EXPECT_EQ(diagnostic.line, 0u);
	EXPECT_EQ(diagnostic.column, 0u);
	EXPECT_STREQ(diagnostic.key, "");
	EXPECT_EQ(tg_config_validate(config.value, &diagnostic), TG_OK);
	EXPECT_EQ(diagnostic.status, TG_OK);
}

TEST(Configuration, PortBoundsAreInclusive)
{
	for (const std::string port : {"1", "65535"}) {
		Config config;

		SCOPED_TRACE(port);
		EXPECT_EQ(parse(udp_config(port), config), TG_OK);
		EXPECT_NE(config.value, nullptr);
	}
}

TEST(Configuration, RejectsOutOfRangeOrNonintegerPorts)
{
	for (const std::string port : {"0", "65536", "-1", "'8087'"}) {
		Config config;
		tg_diagnostic diagnostic{};

		SCOPED_TRACE(port);
		EXPECT_EQ(parse(udp_config(port), config, &diagnostic),
			TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
		EXPECT_EQ(diagnostic.status, TG_INVALID);
		EXPECT_STREQ(diagnostic.key, "inputs[0].port");
	}
}

TEST(Configuration, RejectsUnknownKeysAtEverySchemaLevel)
{
	std::vector<std::string> invalid = {"mystery = 1\n" + udp_config(),
		udp_config("8087", "mystery = true\n"),
		udp_config() + "mystery = 'value'\n",
		tls_config("verify_peer = false\n"),
		replace_one(udp_config(), "version = 1\n",
			"version = 1\n[limits]\nmax_messages = 20\n")};

	for (const std::string &toml : invalid) {
		Config config;
		tg_diagnostic diagnostic{};

		SCOPED_TRACE(toml);
		EXPECT_EQ(parse(toml, config, &diagnostic), TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
		EXPECT_NE(diagnostic.key[0], '\0');
	}
}

TEST(Configuration, UnsupportedSchemaVersionIsDistinctFromBadSyntax)
{
	Config config;
	tg_diagnostic diagnostic{};
	std::string toml =
		replace_one(udp_config(), "version = 1", "version = 2");

	EXPECT_EQ(parse(toml, config, &diagnostic), TG_UNSUPPORTED);
	EXPECT_EQ(config.value, nullptr);
	EXPECT_EQ(diagnostic.status, TG_UNSUPPORTED);
	EXPECT_STREQ(diagnostic.key, "version");
}

TEST(Configuration, MalformedTomlAndUtf8ReturnNoHandle)
{
	std::vector<std::string> invalid = {"version = 1\n[[inputs]\n",
		"version = 1\nkey = 'unterminated\n",
		"version = 1\ninvalid = '\xc0\xaf'\n"};

	for (const std::string &toml : invalid) {
		Config config;
		tg_diagnostic diagnostic{};

		EXPECT_EQ(parse(toml, config, &diagnostic), TG_MALFORMED);
		EXPECT_EQ(config.value, nullptr);
		EXPECT_EQ(diagnostic.status, TG_MALFORMED);
	}
}

TEST(Configuration, RequiresAnEnabledInputAndOutput)
{
	std::vector<std::string> invalid = {"version = 1\n",
		replace_one(udp_config(), "kind = 'udp'",
			"kind = 'udp'\nenabled = false"),
		replace_one(udp_config(), "kind = 'tcp'",
			"kind = 'tcp'\nenabled = false")};

	for (const std::string &toml : invalid) {
		Config config;

		EXPECT_EQ(parse(toml, config), TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
	}
}

TEST(Configuration, RejectsDuplicateEndpointIds)
{
	std::string duplicate_input =
		udp_config() +
		"[[inputs]]\nid = 'receiver'\nkind = 'udp'\nport = 8088\n";
	std::string duplicate_output =
		udp_config() +
		"[[outputs]]\nid = 'collector'\nkind = 'udp'\nport = 9001\n";

	for (const std::string &toml : {duplicate_input, duplicate_output}) {
		Config config;

		EXPECT_EQ(parse(toml, config), TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
	}
}

TEST(Configuration, RequiresExplicitMulticastInterface)
{
	std::string multicast = replace_one(udp_config(), "kind = 'udp'",
		"kind = 'multicast'\ngroup = '239.2.3.1'");
	Config invalid;

	EXPECT_EQ(parse(multicast, invalid), TG_INVALID);
	Config valid;

	multicast = replace_one(multicast, "group = '239.2.3.1'",
		"group = '239.2.3.1'\ninterface = 'fixture-interface'");
	EXPECT_EQ(parse(multicast, valid), TG_OK);
}

TEST(Configuration, RejectsUnsupportedTransportsAndAccidentalPlaintext)
{
	for (const std::string kind : {"tcp", "websocket", "grpc", "quic"}) {
		Config config;
		std::string toml = replace_one(
			udp_config(), "kind = 'udp'", "kind = '" + kind + "'");

		EXPECT_EQ(parse(toml, config), TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
	}
}

TEST(Configuration, DetectsDirectUdpInputOutputLoop)
{
	std::string toml = replace_one(udp_config(),
		"kind = 'tcp'\nmode = 'listen'\nbind = '127.0.0.1'\n"
		"port = 9000",
		"kind = 'udp'\nhost = '127.0.0.1'\n"
		"port = 8087");
	Config config;

	EXPECT_EQ(parse(toml, config), TG_INVALID);
	EXPECT_EQ(config.value, nullptr);
}

TEST(Configuration, RejectsImpossibleAndZeroLimits)
{
	for (const std::string limits : {"max_message_bytes = 0\n",
		     "max_xml_depth = 0\n", "retained_bytes = 1\n",
		     "queue_bytes = 0\n", "queue_records = 0\n",
		     "max_inputs = 0\n", "max_outputs = 0\n",
		     "max_message_bytes = 4294967296\n"}) {
		Config config;
		std::string toml = replace_one(udp_config(), "version = 1\n",
			"version = 1\n[limits]\n" + limits);

		SCOPED_TRACE(limits);
		EXPECT_EQ(parse(toml, config), TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
	}
}

TEST(Configuration, ParsesTlsWithoutReadingReferencedCredentials)
{
	Config config;
	std::string toml = tls_config(
		"cert_file = 'missing-contract-cert.pem'\n"
		"key_file = 'missing-contract-key.pem'\n"
		"key_password_file = 'missing-contract-passphrase'\n");

	EXPECT_EQ(parse(toml, config), TG_OK);
	EXPECT_NE(config.value, nullptr);
}

TEST(Configuration, KeepsTlsConnectionAddressSeparateFromServerIdentity)
{
	Config config;

	ASSERT_EQ(parse(tls_config(), config), TG_OK);
	Buffer redacted;

	ASSERT_EQ(tg_config_redacted(config.value, &redacted.value, nullptr),
		TG_OK);
	std::string text = string(redacted.value);

	EXPECT_NE(text.find("127.0.0.1"), std::string::npos);
	EXPECT_NE(text.find("takserver"), std::string::npos);
	EXPECT_NE(text.find("server_name"), std::string::npos);
}

TEST(Configuration, RejectsUnpairedOrMixedClientCredentialFormats)
{
	for (const std::string credentials :
		{"cert_file = 'client.pem'\n", "key_file = 'client.key'\n",
			"cert_file = 'client.pem'\nkey_file = 'client.key'\n"
			"pkcs12_file = 'client.p12'\n",
			"key_password_file = 'secret'\n"
			"key_password_env = 'SECRET'\n"}) {
		Config config;

		EXPECT_EQ(parse(tls_config(credentials), config), TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
	}
}

TEST(Configuration, SupportsCertificateAndStreamingPasswordTogether)
{
	Config config;
	std::string toml = tls_config(
		"cert_file = 'client.pem'\nkey_file = 'client.key'\n",
		"[inputs.auth]\nmode = 'password'\nusername = 'reader'\n"
		"password_file = 'reader-password'\n");

	EXPECT_EQ(parse(toml, config), TG_OK);
	EXPECT_NE(config.value, nullptr);
}

TEST(Configuration, RejectsLiteralOrAmbiguousPasswordSources)
{
	for (const std::string auth : {
		     "mode = 'password'\nusername = 'reader'\n",
		     "mode = 'password'\nusername = 'reader'\n"
		     "password = 'literal'\n",
		     "mode = 'password'\nusername = 'reader'\n"
		     "password_file = 'secret'\npassword_env = 'SECRET'\n"}) {
		Config config;

		EXPECT_EQ(
			parse(tls_config("", "[inputs.auth]\n" + auth), config),
			TG_INVALID);
		EXPECT_EQ(config.value, nullptr);
	}
}

TEST(Configuration, FixedProtobufRequiresExplicitSupportedVersion)
{
	std::string fixed = replace_one(tls_config(), "protocol = 'xml-only'",
		"protocol = 'protobuf-only'");
	Config missing;

	EXPECT_EQ(parse(fixed, missing), TG_INVALID);
	Config supported;

	EXPECT_EQ(parse(replace_one(fixed, "uid = 'gateway-reader'",
				"uid = 'gateway-reader'\nversion = 1"),
			  supported),
		TG_OK);
	Config unsupported;

	EXPECT_EQ(parse(replace_one(fixed, "uid = 'gateway-reader'",
				"uid = 'gateway-reader'\nversion = 2"),
			  unsupported),
		TG_UNSUPPORTED);
}

TEST(Configuration, FixedModesRejectNegotiationPreferences)
{
	Config config;
	std::string toml = replace_one(tls_config(), "protocol = 'xml-only'",
		"protocol = 'xml-only'\nprefer = 'protobuf'");

	EXPECT_EQ(parse(toml, config), TG_INVALID);
	EXPECT_EQ(config.value, nullptr);
}

TEST(Configuration, ExplicitMaintenanceRequiresPairedPositiveTimers)
{
	for (const std::string maintenance :
		{"ping_interval_ms = 1000\n", "ping_timeout_ms = 1000\n",
			"ping_interval_ms = -1\nping_timeout_ms = 1000\n"}) {
		Config config;

		EXPECT_EQ(parse(tls_config("",
					"[inputs.maintenance]\n" + maintenance),
				  config),
			TG_INVALID);
	}
	Config enabled;

	EXPECT_EQ(
		parse(tls_config("",
			      "[inputs.maintenance]\n"
			      "ping_interval_ms = 10000\nping_timeout_ms = "
			      "20000\n"
			      "identity_xml_file = 'explicit-location.xml'\n"),
			enabled),
		TG_OK);
}

TEST(Configuration, RedactsReferencesWithoutReadingFilesOrEnvironment)
{
	Config config;
	std::string toml = tls_config(
		"cert_file = 'public-cert.pem'\nkey_file = 'secret-key-path'\n"
		"key_password_env = 'SECRET_KEY_ENVIRONMENT_REFERENCE'\n",
		"[inputs.auth]\nmode = 'password'\nusername = 'reader'\n"
		"password_file = 'secret-password-path'\n");

	ASSERT_EQ(parse(toml, config), TG_OK);
	Buffer redacted;
	tg_diagnostic diagnostic{};

	ASSERT_EQ(
		tg_config_redacted(config.value, &redacted.value, &diagnostic),
		TG_OK);
	std::string text = string(redacted.value);

	EXPECT_NE(text.find("<redacted>"), std::string::npos);
	EXPECT_EQ(text.find("secret-key-path"), std::string::npos);
	EXPECT_EQ(text.find("SECRET_KEY_ENVIRONMENT_REFERENCE"),
		std::string::npos);
	EXPECT_EQ(text.find("secret-password-path"), std::string::npos);
	EXPECT_NE(text.find("public-cert.pem"), std::string::npos);
	EXPECT_EQ(diagnostic.status, TG_OK);
}

TEST(Configuration, RedactedOutputHasExplicitDefaultsAndIndependentLifetime)
{
	Config config;

	ASSERT_EQ(parse(udp_config(), config), TG_OK);
	Buffer redacted;

	ASSERT_EQ(tg_config_redacted(config.value, &redacted.value, nullptr),
		TG_OK);
	std::string expected = string(redacted.value);

	EXPECT_NE(expected.find("max_message_bytes"), std::string::npos);
	EXPECT_NE(expected.find("preserve-opaque"), std::string::npos);
	EXPECT_NE(expected.find("drop-newest"), std::string::npos);
	tg_config_destroy(config.value);
	config.value = nullptr;
	EXPECT_EQ(string(redacted.value), expected);
	tg_buffer_release(&redacted.value);
	EXPECT_EQ(redacted.value.data, nullptr);
	EXPECT_EQ(redacted.value.size, 0u);
	tg_buffer_release(&redacted.value);
}

TEST(Configuration, CopiesTomlAndBaseDirectoryBeforeReturning)
{
	Config config;
	std::string toml = tls_config();
	std::string base = config_base_directory();

	ASSERT_EQ(tg_config_parse(
			  view(toml), base.c_str(), &config.value, nullptr),
		TG_OK);
	toml.assign(toml.size(), 'x');
	base.assign(base.size(), 'x');
	Buffer redacted;

	ASSERT_EQ(tg_config_redacted(config.value, &redacted.value, nullptr),
		TG_OK);
	std::string text = string(redacted.value);

	EXPECT_NE(text.find("contract-ca.pem"), std::string::npos);
	EXPECT_NE(text.find("takserver"), std::string::npos);
	EXPECT_NE(text.find("gateway-reader"), std::string::npos);
}

TEST(Configuration, DiagnosticKeysAreBoundedAndNeverContainValues)
{
	Config config;
	tg_diagnostic diagnostic{};
	std::string toml = std::string(200, 'k') +
			   " = 'sensitive-canary-value'\n" + udp_config();

	EXPECT_EQ(parse(toml, config, &diagnostic), TG_INVALID);
	EXPECT_EQ(config.value, nullptr);
	EXPECT_EQ(diagnostic.key[sizeof(diagnostic.key) - 1], '\0');
	const char *end = std::find(
		diagnostic.key, diagnostic.key + sizeof(diagnostic.key), '\0');
	std::string key(
		diagnostic.key, static_cast<size_t>(end - diagnostic.key));

	EXPECT_EQ(key.find("sensitive-canary-value"), std::string::npos);
}

TEST(Configuration, LocalValidationReportsMissingReferenceWithoutNetwork)
{
	std::error_code error;
	std::filesystem::path base = std::filesystem::temp_directory_path();
	std::string suffix = std::to_string(
		std::chrono::steady_clock::now().time_since_epoch().count());
	std::filesystem::path directory = base / ("tg-config-" + suffix);

	ASSERT_TRUE(std::filesystem::create_directory(directory, error));
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup()
		{
			std::error_code ignored;

			std::filesystem::remove_all(path, ignored);
		}
	} cleanup{directory};
	Config config;
	tg_diagnostic diagnostic{};
	std::string toml = tls_config();
	std::string base_text = directory.string();

	ASSERT_EQ(tg_config_parse(view(toml), base_text.c_str(), &config.value,
			  &diagnostic),
		TG_OK);
	EXPECT_EQ(tg_config_validate(config.value, &diagnostic), TG_IO);
	EXPECT_EQ(diagnostic.status, TG_IO);
	EXPECT_STREQ(diagnostic.key, "inputs[0].tls.ca_file");
	EXPECT_EQ(tg_config_validate(config.value, &diagnostic), TG_IO);
}

TEST(Configuration, InvalidArgumentsLeaveNoConfiguration)
{
	std::string toml = udp_config();
	std::string base = config_base_directory();
	tg_config *config = nullptr;
	tg_diagnostic diagnostic{};

	EXPECT_EQ(tg_config_parse(
			  {nullptr, 1}, base.c_str(), &config, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(config, nullptr);
	EXPECT_EQ(tg_config_parse(
			  view(toml), "relative/path", &config, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(config, nullptr);
	EXPECT_EQ(
		tg_config_parse(view(toml), base.c_str(), nullptr, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(tg_config_validate(nullptr, &diagnostic), TG_INVALID);
	Buffer output;

	EXPECT_EQ(tg_config_redacted(nullptr, &output.value, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(output.value.data, nullptr);
	EXPECT_EQ(output.value.size, 0u);
}

TEST(Configuration, RedactedDefaultsPreservePeersAndExplicitLimits)
{
	Config config;
	std::string toml =
		"version = 1\n[limits]\nqueue_records = 7\n" +
		udp_config("8087", "allow_peers = ['127.0.0.1/32']\n")
			.substr(12);

	ASSERT_EQ(parse(toml, config), TG_OK);
	Buffer redacted;

	ASSERT_EQ(tg_config_redacted(config.value, &redacted.value, nullptr),
		TG_OK);
	Config roundtrip;

	ASSERT_EQ(parse(string(redacted.value), roundtrip), TG_OK);
	EXPECT_NE(string(redacted.value).find("queue_records = 7"),
		std::string::npos);
	EXPECT_NE(
		string(redacted.value).find("127.0.0.1/32"), std::string::npos);
	EXPECT_NE(string(redacted.value).find("max_xml_depth = 128"),
		std::string::npos);
}

TEST(Configuration, RejectsInvalidPeerAddressAndPrefix)
{
	for (const char *peer : {"hostname", "127.0.0.1/33", "::1/129",
		     "127.0.0.1/", "127.0.0.1/-1", "127.0.0.1/+1"}) {
		Config config;
		tg_diagnostic diagnostic{};
		std::string extra =
			"allow_peers = ['" + std::string(peer) + "']\n";

		EXPECT_EQ(parse(udp_config("8087", extra), config, &diagnostic),
			TG_INVALID)
			<< peer;
		EXPECT_STREQ(diagnostic.key, "inputs[0].allow_peers");
	}
}
