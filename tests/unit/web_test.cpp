#include "web_fixtures.hpp"
#include <openssl/pem.h>
#include <fstream>
#include <iterator>

using namespace contract;
using namespace web_fixtures;

TEST(WebManagement, RejectsInvalidCreateArguments)
{
	tg_web_app *app = nullptr;
	tg_diagnostic diagnostic{};

	EXPECT_EQ(tg_web_app_create(nullptr, nullptr, nullptr, 0, &app,
		&diagnostic), TG_INVALID);
	EXPECT_EQ(app, nullptr);
	EXPECT_EQ(diagnostic.status, TG_INVALID);
	tg_web_app_destroy(nullptr);
}

TEST(WebManagement, ServesStateAndEmbeddedPageOverRealLoopbackHttp)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	Response response = gateway.state();

	ASSERT_EQ(response.status, 200);
	Json state(response.body);

	ASSERT_NE(state.document, nullptr);
	EXPECT_EQ(gateway.revision, 0u);
	EXPECT_EQ(gateway.token.size(), 64u);
	EXPECT_TRUE(yyjson_is_obj(field(state.root(), "config")));
	EXPECT_TRUE(yyjson_is_obj(field(state.root(), "status")));
	EXPECT_EQ(yyjson_arr_size(field(field(state.root(), "config"),
		"inputs")), 0u);
	Response page = gateway.request("GET", "/");

	EXPECT_EQ(page.status, 200);
	EXPECT_NE(page.headers.find("text/html"), std::string::npos);
	EXPECT_NE(page.body.find("<!"), std::string::npos);
	EXPECT_EQ(gateway.poll_error, TG_OK);
}

TEST(WebManagement, ServesCompleteAssetsWithOriginalBytes)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	for (const char *asset : {"index.html", "app.js", "styles.css"}) {
		SCOPED_TRACE(asset);
		std::ifstream source(std::string(TG_WEB_ASSET_DIRECTORY) +
			"/" + asset, std::ios::binary);

		ASSERT_TRUE(source.is_open());
		std::string expected((std::istreambuf_iterator<char>(source)),
			std::istreambuf_iterator<char>());
		Response response = gateway.request("GET",
			std::string("/") + asset);

		EXPECT_EQ(response.status, 200);
		EXPECT_EQ(response.body, expected);
	}
}

TEST(WebManagement, RejectsMutationsWithoutTokenAndCrossOriginRequests)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	std::string body = gateway.edit("output",
		tcp_output("consumer", free_port(SOCK_STREAM)),
		gateway.revision);

	EXPECT_EQ(gateway.request("POST", "/api/outputs", body, false).status,
		403);
	EXPECT_EQ(gateway.request("POST", "/api/outputs", body, true,
		"application/json", "http://untrusted.invalid").status, 403);
	ASSERT_EQ(gateway.state().status, 200);
	EXPECT_EQ(gateway.revision, 0u);
}

TEST(WebManagement, RejectsUntrustedHostBeforePublishingBootstrapState)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	Response blocked = gateway.exchange(
		"GET /api/state HTTP/1.1\r\nHost: attacker.invalid:" +
		std::to_string(gateway.port) + "\r\nConnection: close\r\n\r\n");
	Json error(blocked.body);

	EXPECT_EQ(blocked.status, 403);
	EXPECT_EQ(field(error.root(), "token"), nullptr);
	EXPECT_EQ(gateway.state().status, 200);
}

TEST(WebManagement, CreatesEditsAndRemovesOutputsWithRevisionConflicts)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	uint16_t first_port = free_port(SOCK_STREAM);
	uint16_t second_port = free_port(SOCK_STREAM);
	std::string created = tcp_output("consumer", first_port);
	Response accepted = gateway.request("POST", "/api/outputs",
		gateway.edit("output", created, gateway.revision));

	ASSERT_EQ(accepted.status, 202);
	Response completion = gateway.wait(accepted, "committed");
	Json job(completion.body);

	ASSERT_EQ(text(field(job.root(), "state")), "committed");
	ASSERT_EQ(gateway.state().status, 200);
	ASSERT_EQ(gateway.revision, 1u);
	EXPECT_EQ(gateway.request("PUT", "/api/outputs/consumer",
		gateway.edit("output", tcp_output("consumer", second_port), 0)).
		status, 409);
	accepted = gateway.request("PUT", "/api/outputs/consumer",
		gateway.edit("output", tcp_output("consumer", second_port), 1));
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json edited(completion.body);

	ASSERT_EQ(text(field(edited.root(), "state")), "committed");
	Response state_response = gateway.state();
	Json state(state_response.body);
	yyjson_val *outputs = field(field(state.root(), "config"), "outputs");

	ASSERT_EQ(yyjson_arr_size(outputs), 1u);
	EXPECT_EQ(yyjson_get_uint(field(yyjson_arr_get(outputs, 0), "port")),
		second_port);
	EXPECT_EQ(gateway.revision, 2u);
	accepted = gateway.request("DELETE", "/api/outputs/consumer",
		"{\"revision\":2}");
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json removed(completion.body);

	EXPECT_EQ(text(field(removed.root(), "state")), "committed");
	state_response = gateway.state();
	Json final_state(state_response.body);

	EXPECT_EQ(gateway.revision, 3u);
	EXPECT_EQ(yyjson_arr_size(field(field(final_state.root(), "config"),
		"outputs")), 0u);
}

TEST(WebManagement, CreatesEditsAndRemovesInputsWithoutRequiringOutputs)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	std::string input = udp_input("source", free_port(SOCK_DGRAM));
	Response accepted = gateway.request("POST", "/api/inputs",
		gateway.edit("input", input, gateway.revision));

	ASSERT_EQ(accepted.status, 202);
	Response completion = gateway.wait(accepted, "committed");
	Json job(completion.body);

	ASSERT_EQ(text(field(job.root(), "state")), "committed");
	ASSERT_EQ(gateway.state().status, 200);
	uint16_t changed_port = free_port(SOCK_DGRAM);

	accepted = gateway.request("PUT", "/api/inputs/source",
		gateway.edit("input", udp_input("source", changed_port),
			gateway.revision));
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json changed(completion.body);

	ASSERT_EQ(text(field(changed.root(), "state")), "committed") <<
		completion.body;
	Response state_response = gateway.state();
	Json state(state_response.body);
	yyjson_val *inputs = field(field(state.root(), "config"), "inputs");

	ASSERT_EQ(yyjson_arr_size(inputs), 1u);
	EXPECT_EQ(yyjson_get_uint(field(yyjson_arr_get(inputs, 0), "port")),
		changed_port);
	accepted = gateway.request("DELETE", "/api/inputs/source",
		"{\"revision\":" + std::to_string(gateway.revision) + "}");
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json removed(completion.body);

	EXPECT_EQ(text(field(removed.root(), "state")), "committed");
	state_response = gateway.state();
	Json final_state(state_response.body);

	EXPECT_EQ(yyjson_arr_size(field(field(final_state.root(), "config"),
		"inputs")), 0u);
}

TEST(WebManagement, CollectionEditsPreserveExistingUtf8Ids)
{
	Gateway gateway;
	std::string id = u8"TAK Server / \u6771\u4eac.example";

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	Response accepted = gateway.request("POST", "/api/inputs",
		gateway.edit("input", udp_input(id, free_port(SOCK_DGRAM)), 0));

	ASSERT_EQ(accepted.status, 202);
	Response completion = gateway.wait(accepted, "committed");
	Json created(completion.body);

	ASSERT_EQ(text(field(created.root(), "state")), "committed");
	ASSERT_EQ(gateway.state().status, 200);
	uint16_t edited_port = free_port(SOCK_DGRAM);
	std::string edit = "{\"revision\":" +
		std::to_string(gateway.revision) + ",\"id\":" + quote(id) +
		",\"input\":" + udp_input(id, edited_port) + "}";

	accepted = gateway.request("PUT", "/api/inputs", edit);
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json updated(completion.body);

	ASSERT_EQ(text(field(updated.root(), "state")), "committed");
	Response response = gateway.state();
	Json state(response.body);
	yyjson_val *inputs = field(field(state.root(), "config"), "inputs");

	ASSERT_EQ(yyjson_arr_size(inputs), 1u);
	EXPECT_EQ(text(field(yyjson_arr_get(inputs, 0), "id")), id);
	EXPECT_EQ(yyjson_get_uint(field(yyjson_arr_get(inputs, 0), "port")),
		edited_port);
	std::string remove = "{\"revision\":" +
		std::to_string(gateway.revision) + ",\"id\":" + quote(id) + "}";

	accepted = gateway.request("DELETE", "/api/inputs", remove);
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json removed(completion.body);

	EXPECT_EQ(text(field(removed.root(), "state")), "committed");
	response = gateway.state();
	Json final_state(response.body);

	EXPECT_EQ(yyjson_arr_size(field(field(final_state.root(), "config"),
		"inputs")), 0u);
}

TEST(WebManagement, StateOmitsSecretsAndCredentialReferenceValues)
{
	Gateway gateway;
	std::string initial = "version=1\n[[inputs]]\nid='tls'\nkind='tls'\n"
		"host='127.0.0.1'\nport=18089\nuid='reader'\n[inputs.tls]\n"
		"ca_file='CA_REFERENCE_MARKER'\n"
		"cert_file='CERT_REFERENCE_MARKER'\n"
		"key_file='KEY_REFERENCE_MARKER'\n"
		"key_password_file='KEY_PASSWORD_REFERENCE_MARKER'\n"
		"[inputs.auth]\nmode='password'\nusername='reader'\n"
		"password_file='PASSWORD_REFERENCE_MARKER'\n";

	ASSERT_TRUE(gateway.prepare(initial));
	Response response = gateway.state();

	ASSERT_EQ(response.status, 200);
	EXPECT_EQ(response.body.find("REFERENCE_MARKER"), std::string::npos);
	Json state(response.body);
	yyjson_val *input = yyjson_arr_get(field(field(state.root(), "config"),
		"inputs"), 0);

	ASSERT_NE(input, nullptr);
	EXPECT_EQ(field(field(input, "tls"), "ca_file"), nullptr);
	EXPECT_EQ(field(field(input, "tls"), "key_file"), nullptr);
	EXPECT_EQ(field(field(input, "auth"), "password_file"), nullptr);
	EXPECT_TRUE(yyjson_get_bool(field(field(input, "tls"), "has_ca")));
	EXPECT_TRUE(yyjson_get_bool(
		field(field(input, "auth"), "has_password")));
}

TEST(WebManagement, CommittedConfigurationSurvivesAStoreRestart)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	uint16_t output_port = free_port(SOCK_STREAM);
	std::string output = tcp_output("persistent", output_port);
	Response accepted = gateway.request("POST", "/api/outputs",
		gateway.edit("output", output, 0));

	ASSERT_EQ(accepted.status, 202);
	Response completion = gateway.wait(accepted, "committed");
	Json job(completion.body);

	ASSERT_EQ(text(field(job.root(), "state")), "committed");
	ASSERT_TRUE(gateway.restart());
	Response response = gateway.state();
	Json state(response.body);
	yyjson_val *outputs = field(field(state.root(), "config"), "outputs");

	ASSERT_EQ(response.status, 200);
	EXPECT_EQ(gateway.revision, 1u);
	ASSERT_EQ(yyjson_arr_size(outputs), 1u);
	EXPECT_EQ(text(field(yyjson_arr_get(outputs, 0), "id")), "persistent");
	EXPECT_EQ(yyjson_get_uint(field(yyjson_arr_get(outputs, 0), "port")),
		output_port);
}

TEST(WebManagement, CompletedJobsDoNotExhaustTheLifetimeEditCapacity)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	uint16_t port = free_port(SOCK_STREAM);

	for (unsigned edit = 0; edit < 70; ++edit) {
		std::string output = edit ? "{\"name\":" +
			quote("Consumer " + std::to_string(edit)) + "}" :
			tcp_output("consumer", port);
		Response accepted = gateway.request(edit ? "PUT" : "POST",
			edit ? "/api/outputs/consumer" : "/api/outputs",
			gateway.edit("output", output, gateway.revision));

		ASSERT_EQ(accepted.status, 202) << "edit " << edit;
		Response completion = gateway.wait(accepted, "committed");
		Json job(completion.body);

		ASSERT_EQ(text(field(job.root(), "state")), "committed") <<
			"edit " << edit << ": " << completion.body;
		ASSERT_EQ(gateway.state().status, 200);
		ASSERT_EQ(gateway.revision, edit + 1u);
	}
}

TEST(WebManagement, ZipImportRequiresPreviewConfirmationBeforeMutation)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	std::string archive = import_fixtures::read_file(
		gateway.fixture.wrapped());
	Response submitted = gateway.request("POST", "/api/import", archive,
		true, "application/zip");

	ASSERT_EQ(submitted.status, 202);
	Response preview = gateway.wait(submitted, "ready");
	Json job(preview.body);

	ASSERT_EQ(text(field(job.root(), "state")), "ready") << preview.body;
	EXPECT_EQ(preview.body.find("test-only-secret"), std::string::npos);
	EXPECT_EQ(preview.body.find("PRIVATE KEY"), std::string::npos);
	EXPECT_EQ(preview.body.find("ca_file"), std::string::npos);
	ASSERT_EQ(gateway.state().status, 200);
	EXPECT_EQ(gateway.revision, 0u);
	std::string id = identifier(field(job.root(), "id"));
	yyjson_val *inputs = field(field(job.root(), "preview"), "inputs");

	ASSERT_EQ(yyjson_arr_size(inputs), 1u);
	size_t size = 0;
	char *serialized = yyjson_val_write(inputs, 0, &size);

	ASSERT_NE(serialized, nullptr);
	std::string confirmation = "{\"revision\":0,\"inputs\":" +
		std::string(serialized, size) + "}";

	free(serialized);
	Response accepted = gateway.request("POST", "/api/import/" + id +
		"/confirm", confirmation);

	ASSERT_EQ(accepted.status, 202);
	Response completed = gateway.wait(accepted, "committed");
	Json committed(completed.body);

	ASSERT_EQ(text(field(committed.root(), "state")), "committed");
	Response response = gateway.state();
	Json state(response.body);

	EXPECT_EQ(gateway.revision, 1u);
	EXPECT_EQ(yyjson_arr_size(field(field(state.root(), "config"),
		"inputs")), 1u);
	ASSERT_TRUE(gateway.restart());
	response = gateway.state();
	EXPECT_EQ(response.status, 200);
	EXPECT_EQ(response.body.find("test-only-secret"), std::string::npos);
	EXPECT_EQ(gateway.revision, 1u);
}

TEST(WebManagement, RejectsMalformedBodiesAndDeclaredOversizedRequests)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	std::vector<std::string> malformed = {"{broken", "[]",
		"{\"revision\":0,\"revision\":0,\"output\":{}}",
		"{\"revision\":0,\"output\":{\"id\":\"safe\\u0000hidden\","
		"\"kind\":\"tcp\",\"port\":19000}}",
		"{\"revision\":0,\"output\":" + std::string(1000, '[') +
		"0" + std::string(1000, ']') + "}"};

	for (const auto &body : malformed)
		EXPECT_EQ(gateway.request("POST", "/api/outputs", body).status,
			400);
	std::string wire = "POST /api/import HTTP/1.1\r\nHost: 127.0.0.1:" +
		std::to_string(gateway.port) + "\r\nX-TG-Token: " +
		gateway.token +
		"\r\nContent-Type: application/zip\r\n"
		"Content-Length: 16777217\r\nConnection: close\r\n\r\n";

	EXPECT_EQ(gateway.exchange(wire).status, 413);
	ASSERT_EQ(gateway.state().status, 200);
	EXPECT_EQ(gateway.revision, 0u);
}

TEST(WebManagement, FailedZipPreviewDoesNotChangeTheCommittedRevision)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	Response submitted = gateway.request("POST", "/api/import",
		"not-a-zip", true, "application/zip");

	ASSERT_EQ(submitted.status, 202);
	Response failed = gateway.wait(submitted, "failed");
	Json job(failed.body);

	EXPECT_EQ(text(field(job.root(), "state")), "failed");
	EXPECT_TRUE(yyjson_is_obj(field(job.root(), "error")));
	ASSERT_EQ(gateway.state().status, 200);
	EXPECT_EQ(gateway.revision, 0u);
}

static std::string fixture_ca_pem(const import_fixtures::Package &fixture)
{
	const unsigned char *bytes = fixture.client.data();
	PKCS12 *bundle = d2i_PKCS12(nullptr, &bytes, fixture.client.size());
	EVP_PKEY *key = nullptr;
	X509 *certificate = nullptr;
	STACK_OF(X509) *chain = nullptr;
	BIO *output = BIO_new(BIO_s_mem());
	bool ok = bundle && output && PKCS12_parse(bundle, "test-only-secret",
		&key, &certificate, &chain) == 1 && sk_X509_num(chain) > 0 &&
		PEM_write_bio_X509(output, sk_X509_value(chain, 0)) == 1;
	char *text = nullptr;
	long size = ok ? BIO_get_mem_data(output, &text) : 0;
	std::string result = size > 0 ?
		std::string(text, static_cast<size_t>(size)) : "";

	BIO_free(output);
	EVP_PKEY_free(key);
	X509_free(certificate);
	sk_X509_pop_free(chain, X509_free);
	PKCS12_free(bundle);
	return result;
}

TEST(WebManagement, OmittedCredentialEditsRetainPrivateManagedAssets)
{
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	std::string ca = fixture_ca_pem(gateway.fixture);

	ASSERT_FALSE(ca.empty());
	Response uploaded_ca = gateway.request("POST", "/api/assets?kind=ca",
		ca, true, "application/octet-stream");
	std::string bundle(gateway.fixture.client.begin(),
		gateway.fixture.client.end());
	Response uploaded_client = gateway.request("POST",
		"/api/assets?kind=pkcs12", bundle, true,
		"application/octet-stream");

	ASSERT_GE(uploaded_ca.status, 200);
	ASSERT_LT(uploaded_ca.status, 300);
	ASSERT_GE(uploaded_client.status, 200);
	ASSERT_LT(uploaded_client.status, 300);
	Json ca_asset(uploaded_ca.body);
	Json client_asset(uploaded_client.body);
	std::string ca_id = text(field(ca_asset.root(), "asset_id"));
	std::string client_id = text(field(client_asset.root(), "asset_id"));

	ASSERT_FALSE(ca_id.empty());
	ASSERT_FALSE(client_id.empty());
	std::string port = std::to_string(free_port(SOCK_STREAM));
	std::string identity = "{\"id\":\"tls\",\"name\":\"TLS reader\","
		"\"kind\":\"tls\",\"host\":\"127.0.0.1\",\"port\":" + port + ","
		"\"uid\":\"web-reader\",\"callsign\":null,"
		"\"protocol\":\"xml-only\",\"identity_mode\":\"pkcs12\","
		"\"tls\":{\"server_name\":null,\"has_ca\":false,\"ca_asset\":" +
		quote(ca_id) + ",\"pkcs12_asset\":" + quote(client_id) +
		",\"pkcs12_password\":\"test-only-secret\"}}";
	Response accepted = gateway.request("POST", "/api/inputs",
		gateway.edit("input", identity, gateway.revision));

	ASSERT_EQ(accepted.status, 202);
	Response completion = gateway.wait(accepted, "committed");
	Json created(completion.body);

	ASSERT_EQ(text(field(created.root(), "state")), "committed") <<
		completion.body;
	Response configured = gateway.state();
	Json configured_state(configured.body);
	yyjson_val *configured_input = yyjson_arr_get(field(field(
		configured_state.root(), "config"), "inputs"), 0);
	std::string mode = text(field(configured_input, "identity_mode"));

	ASSERT_EQ(configured.status, 200);
	ASSERT_TRUE(mode == "pem" || mode == "pkcs12");
	identity = "{\"id\":\"tls\",\"name\":\"Renamed reader\","
		"\"kind\":\"tls\",\"host\":\"127.0.0.1\",\"port\":" + port + ","
		"\"uid\":\"web-reader\",\"protocol\":\"xml-only\","
		"\"identity_mode\":" + quote(mode) + ",\"tls\":{}}";
	accepted = gateway.request("PUT", "/api/inputs/tls",
		gateway.edit("input", identity, gateway.revision));
	ASSERT_EQ(accepted.status, 202);
	completion = gateway.wait(accepted, "committed");
	Json renamed(completion.body);

	ASSERT_EQ(text(field(renamed.root(), "state")), "committed");
	ASSERT_TRUE(gateway.restart());
	Response response = gateway.state();
	Json state(response.body);
	yyjson_val *input = yyjson_arr_get(field(field(state.root(), "config"),
		"inputs"), 0);
	yyjson_val *tls = field(input, "tls");

	ASSERT_EQ(response.status, 200);
	EXPECT_EQ(text(field(input, "name")), "Renamed reader");
	EXPECT_TRUE(yyjson_get_bool(field(tls, "has_ca")));
	EXPECT_TRUE(yyjson_get_bool(field(tls, "has_pkcs12")) ||
		(yyjson_get_bool(field(tls, "has_certificate")) &&
		 yyjson_get_bool(field(tls, "has_key"))));
	EXPECT_EQ(response.body.find("test-only-secret"), std::string::npos);
	EXPECT_EQ(response.body.find(ca_id), std::string::npos);
	EXPECT_EQ(response.body.find(client_id), std::string::npos);
	gateway.stop();
	Buffer persisted;
	tg_web_settings settings{};
	uint64_t revision = 0;
	Config saved;

	ASSERT_EQ(tg_store_load(gateway.store, &persisted.value, &settings,
		&revision, nullptr), TG_OK);
	ASSERT_EQ(tg_config_parse_managed({persisted.value.data,
		persisted.value.size}, gateway.state_path().c_str(),
		&saved.value, nullptr), TG_OK);
	EXPECT_EQ(tg_config_validate(saved.value, nullptr), TG_OK);
	ASSERT_EQ(saved.value->input_count, 1u);
	const tg_input_config &saved_input = saved.value->inputs[0];

	EXPECT_STREQ(saved_input.server_name, "127.0.0.1");
	EVP_PKEY *private_key = nullptr;
	X509 *certificate = nullptr;

	if (saved_input.key_file) {
		BIO *key_file = BIO_new_file(saved_input.key_file, "r");
		BIO *certificate_file = BIO_new_file(saved_input.cert_file,
			"r");

		if (key_file)
			private_key = PEM_read_bio_PrivateKey(key_file, nullptr,
				nullptr, nullptr);
		if (certificate_file)
			certificate = PEM_read_bio_X509(certificate_file,
				nullptr, nullptr, nullptr);
		BIO_free(key_file);
		BIO_free(certificate_file);
	} else {
		ASSERT_NE(saved_input.pkcs12_file, nullptr);
		std::string bytes = import_fixtures::read_file(
			saved_input.pkcs12_file);
		const unsigned char *cursor = reinterpret_cast<
			const unsigned char *>(bytes.data());
		PKCS12 *pkcs12 = d2i_PKCS12(nullptr, &cursor, bytes.size());

		if (pkcs12)
			PKCS12_parse(pkcs12, "test-only-secret", &private_key,
				&certificate, nullptr);
		PKCS12_free(pkcs12);
	}
	EXPECT_NE(private_key, nullptr);
	EXPECT_NE(certificate, nullptr);
	if (private_key && certificate)
		EXPECT_EQ(X509_check_private_key(certificate, private_key), 1);
	EVP_PKEY_free(private_key);
	X509_free(certificate);
}

struct WebPasswordEnvironment {
	const char *name = "TG_WEB_TEST_PKCS12_PASSWORD";
	std::string previous;
	bool present = false;

	WebPasswordEnvironment()
	{
		const char *value = getenv(name);

		present = value != nullptr;
		if (value)
			previous = value;
	}

	int set(const char *value)
	{
#ifdef _WIN32
		return _putenv_s(name, value ? value : "");
#else
		return value ? setenv(name, value, 1) : unsetenv(name);
#endif
	}

	~WebPasswordEnvironment()
	{
		set(present ? previous.c_str() : nullptr);
	}
};

TEST(WebManagement, ManualPkcs12AcceptsEnvironmentPassword)
{
	WebPasswordEnvironment password;

	ASSERT_EQ(password.set("test-only-secret"), 0);
	Gateway gateway;

	ASSERT_TRUE(gateway.prepare());
	ASSERT_EQ(gateway.state().status, 200);
	std::string ca = fixture_ca_pem(gateway.fixture);

	ASSERT_FALSE(ca.empty());
	Response ca_upload = gateway.request("POST", "/api/assets?kind=ca",
		ca, true, "application/octet-stream");
	std::string bundle(gateway.fixture.client.begin(),
		gateway.fixture.client.end());
	Response bundle_upload = gateway.request("POST",
		"/api/assets?kind=pkcs12", bundle, true,
		"application/octet-stream");

	ASSERT_EQ(ca_upload.status, 200);
	ASSERT_EQ(bundle_upload.status, 200);
	Json ca_asset(ca_upload.body);
	Json bundle_asset(bundle_upload.body);
	std::string identity = "{\"id\":\"environment-identity\","
		"\"kind\":\"tls\",\"host\":\"127.0.0.1\",\"port\":" +
		std::to_string(free_port(SOCK_STREAM)) + ",\"uid\":\"web-env\","
		"\"protocol\":\"xml-only\",\"identity_mode\":\"pkcs12\","
		"\"tls\":{\"ca_asset\":" +
		quote(text(field(ca_asset.root(), "asset_id"))) +
		",\"pkcs12_asset\":" +
		quote(text(field(bundle_asset.root(), "asset_id"))) +
		",\"pkcs12_password_env\":" + quote(password.name) + "}}";
	Response accepted = gateway.request("POST", "/api/inputs",
		gateway.edit("input", identity, gateway.revision));

	ASSERT_EQ(accepted.status, 202);
	Response completion = gateway.wait(accepted, "committed");
	Json job(completion.body);

	ASSERT_EQ(text(field(job.root(), "state")), "committed") <<
		completion.body;
	Response configured = gateway.state();
	Json state(configured.body);
	yyjson_val *input = yyjson_arr_get(field(field(state.root(), "config"),
		"inputs"), 0);

	ASSERT_EQ(configured.status, 200);
	EXPECT_EQ(text(field(input, "identity_mode")), "pem");
	EXPECT_EQ(configured.body.find("test-only-secret"), std::string::npos);
	EXPECT_EQ(configured.body.find(password.name), std::string::npos);
}
