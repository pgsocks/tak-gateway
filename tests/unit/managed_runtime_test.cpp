#include "runtime_fixtures.hpp"
#include "../../src/managed_runtime.h"

using namespace contract;
using namespace runtime_fixtures;

static tg_status managed_parse(const std::string &text, Config &config,
	tg_diagnostic *diagnostic = nullptr)
{
	return tg_config_parse_managed(view(text), base_directory(),
		&config.value, diagnostic);
}

TEST(ManagedConfiguration, EmptyAndDisabledCollectionsKeepPublicParseStrict)
{
	for (const auto &text : {std::string("version=1\n"),
		std::string("version=1\ninputs=[]\noutputs=[]\n")}) {
		Config strict, managed;

		EXPECT_EQ(tg_config_parse(view(text), base_directory(),
			&strict.value, nullptr), TG_INVALID);
		ASSERT_EQ(managed_parse(text, managed), TG_OK);
		EXPECT_EQ(managed.value->input_count, 0u);
		EXPECT_EQ(managed.value->output_count, 0u);
		Runtime runtime;

		ASSERT_EQ(tg_runtime_create_managed(managed.value,
			&runtime.value, nullptr), TG_OK);
		EXPECT_EQ(tg_runtime_poll(runtime.value, 0), TG_OK);
		Buffer snapshot;

		ASSERT_EQ(tg_runtime_snapshot(runtime.value, &snapshot.value),
			TG_OK);
		EXPECT_NE(string(snapshot.value).find("RUNNING"),
			std::string::npos);
	}
}

TEST(ManagedConfiguration, NamesDefaultToIdsAndUnsafeSchemaStillFails)
{
	std::string text = fixtures::minimal_config();
	Config config;

	ASSERT_EQ(managed_parse(text, config), TG_OK);
	EXPECT_STREQ(config.value->inputs[0].name,
		config.value->inputs[0].id);
	EXPECT_STREQ(config.value->outputs[0].name,
		config.value->outputs[0].id);
	for (const auto &bad : {"version=1\nunknown=1\n",
		"version=1\n[[inputs]]\nid='x'\nkind='tls'\n"}) {
		Config invalid;

		EXPECT_LT(managed_parse(bad, invalid), TG_OK);
	}
}

TEST(ManagedConfiguration, ExportPreservesProtectedReferencesAndNames)
{
	std::string text = "version=1\n[[inputs]]\nid='tak'\n"
		"name='Example server'\nkind='tls'\nhost='example.invalid'\n"
		"uid='reader'\n[inputs.tls]\nca_file='ca.pem'\n"
		"verify_hostname=false\n"
		"cert_file='client.pem'\nkey_file='client.key'\n"
		"key_password_env='MANAGED_TEST_SECRET_REFERENCE'\n"
		"[[outputs]]\nid='sink'\nname='Local'\nkind='tcp'\n"
		"port=19000\n";
	Config config, restored;
	Buffer full, redacted;

	ASSERT_EQ(managed_parse(text, config), TG_OK);
	ASSERT_EQ(tg_config_export(config.value, &full.value, nullptr), TG_OK);
	ASSERT_EQ(tg_config_redacted(config.value, &redacted.value, nullptr),
		TG_OK);
	EXPECT_NE(string(full.value).find("MANAGED_TEST_SECRET_REFERENCE"),
		std::string::npos);
	EXPECT_EQ(string(redacted.value).find("MANAGED_TEST_SECRET_REFERENCE"),
		std::string::npos);
	ASSERT_EQ(managed_parse(string(full.value), restored), TG_OK);
	EXPECT_STREQ(restored.value->inputs[0].name, "Example server");
	EXPECT_FALSE(restored.value->inputs[0].verify_hostname);
	EXPECT_STREQ(restored.value->outputs[0].name, "Local");
	EXPECT_STREQ(restored.value->inputs[0].key_file,
		config.value->inputs[0].key_file);
}

static bool start_managed(GatewayFixture &fixture)
{
	if (!fixture.prepare() ||
		managed_parse(fixture.text, fixture.config) != TG_OK ||
		tg_runtime_create_managed(fixture.config.value,
			&fixture.runtime.value, nullptr) != TG_OK)
		return false;
	return fixture.start() == TG_OK && fixture.connect_consumer();
}

TEST(ManagedRuntime, AddBrokenSourcePreservesHealthyConsumerAndDelivery)
{
	GatewayFixture fixture;

	ASSERT_TRUE(start_managed(fixture));
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
	Config candidate;
	std::string text = fixture.text + "\n[[inputs]]\nid='broken'\n"
		"kind='tls'\nhost='bad.example.invalid'\nuid='bad'\n"
		"[inputs.tls]\nca_file='missing-managed-ca.pem'\n";

	ASSERT_EQ(managed_parse(text, candidate), TG_OK);
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_OK);
	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size() * 2));
	EXPECT_EQ(fixture.received, record(xml) + record(xml));
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("broken"), std::string::npos);
	EXPECT_NE(string(snapshot.value).find("credential"), std::string::npos);
}

TEST(ManagedRuntime, LabelsAndReorderedIdsDoNotDisconnectConsumers)
{
	GatewayFixture fixture;

	ASSERT_TRUE(start_managed(fixture));
	Config candidate;
	std::string text = fixture.text;
	size_t at = text.find("id = \"source\"");

	ASSERT_NE(at, std::string::npos);
	text.insert(at, "name='Updated name'\n");
	ASSERT_EQ(managed_parse(text, candidate), TG_OK);
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_OK);
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("Updated name"),
		std::string::npos);
}

TEST(ManagedRuntime, GlobalChangesFailWithoutTouchingExistingEndpoints)
{
	GatewayFixture fixture;

	ASSERT_TRUE(start_managed(fixture));
	Config candidate;

	ASSERT_EQ(managed_parse(fixture.text +
		"\n[limits]\nqueue_records=7\n", candidate), TG_OK);
	EXPECT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_INVALID);
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
}

TEST(ManagedRuntime, InputPortReplacementPreservesExistingOutputClient)
{
	GatewayFixture fixture;
	Socket reservation;
	uint16_t replacement = 0;

	ASSERT_TRUE(start_managed(fixture));
	ASSERT_TRUE(reserve(reservation, SOCK_DGRAM, replacement));
	std::string text = fixture.text;
	std::string port = "port = " + std::to_string(fixture.input_port);
	size_t at = text.find(port);

	ASSERT_NE(at, std::string::npos);
	text.replace(at, port.size(), "port = " + std::to_string(replacement));
	Config candidate;

	ASSERT_EQ(managed_parse(text, candidate), TG_OK);
	reservation.close();
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	fixture.input_port = replacement;
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
}

TEST(ManagedRuntime, BrokenNewOutputDoesNotStopOtherOutput)
{
	GatewayFixture fixture;
	Socket occupied;
	uint16_t port = 0;

	ASSERT_TRUE(start_managed(fixture));
	ASSERT_TRUE(reserve(occupied, SOCK_STREAM, port));
	ASSERT_EQ(listen(occupied.value, 1), 0);
	Config candidate;
	std::string text = fixture.text + "\n[[outputs]]\nid='occupied'\n"
		"kind='tcp'\nbind='127.0.0.1'\nport=" + std::to_string(port);

	ASSERT_EQ(managed_parse(text, candidate), TG_OK);
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("output-initialization"),
		std::string::npos);
}

#ifndef _WIN32
#include "import_fixtures.hpp"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <atomic>
#include <thread>

namespace {

struct DiagnosticServer {
	import_fixtures::Package files;
	Socket listener;
	SSL_CTX *tls = nullptr;
	std::thread worker;
	std::atomic<bool> stopping{false};
	uint16_t port = 0;
	std::string ca_path;
	std::string password_path;
	std::string event;
	const char *ca_name = "Diagnostics CA";
	bool read_auth = false;

	bool prepare(bool expired, bool require_certificate,
		const char *hostname = "127.0.0.1")
	{
		using namespace import_fixtures;

		if (!files.prepare() || !reserve(listener, SOCK_STREAM, port) ||
			listen(listener.value, 1) || !listener.nonblocking())
			return false;
		EVP_PKEY *ca_key = key();
		EVP_PKEY *server_key = key();
		X509 *ca = ca_key ? certificate(ca_key, ca_name,
			ca_key,
			nullptr, true) : nullptr;
		X509 *server = ca && server_key ? certificate(server_key,
			hostname, ca_key, ca, false) : nullptr;
		std::string identity = !strcmp(hostname, "127.0.0.1") ?
			"IP:127.0.0.1" : std::string("DNS:") + hostname;
		X509_EXTENSION *san = X509V3_EXT_conf_nid(nullptr, nullptr,
			NID_subject_alt_name, identity.data());
		bool ok = ca && server && san;

		if (ok) {
			X509_add_ext(server, san, -1);
			if (expired)
				X509_gmtime_adj(X509_getm_notAfter(server), -1);
			ok = X509_sign(server, ca_key, EVP_sha256()) > 0;
		}
		ca_path = (files.directory / "diagnostic-ca.pem").string();
		BIO *output = ok ? BIO_new_file(ca_path.c_str(), "w") : nullptr;

		ok = ok && output && PEM_write_bio_X509(output, ca) == 1;
		BIO_free(output);
		tls = SSL_CTX_new(TLS_server_method());
		ok = ok && tls && SSL_CTX_use_certificate(tls, server) == 1 &&
			SSL_CTX_use_PrivateKey(tls, server_key) == 1;
		int verify = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

		if (ok && require_certificate)
			SSL_CTX_set_verify(tls, verify, nullptr);
		X509_EXTENSION_free(san);
		X509_free(server);
		X509_free(ca);
		EVP_PKEY_free(server_key);
		EVP_PKEY_free(ca_key);
		password_path = files.write(fixtures::bytes(
			"diagnostic-secret"),
			"stream-password");
		if (password_path.empty() || chmod(password_path.c_str(), 0600))
			return false;
		return ok;
	}

	void start(bool auth)
	{
		read_auth = auth;
		worker = std::thread([this] {
			Socket peer;

			while (!stopping) {
				peer.value = accept(listener.value, nullptr,
					nullptr);
				if (peer.value != invalid_socket)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(1));
			}
			if (peer.value == invalid_socket)
				return;
			timeval timeout{2, 0};

			setsockopt(peer.value, SOL_SOCKET, SO_RCVTIMEO,
				&timeout,
				sizeof(timeout));
			SSL *ssl = SSL_new(tls);

			bool accepted = ssl && SSL_set_fd(ssl, peer.value) &&
				SSL_accept(ssl) == 1;

			if (accepted) {
				if (!event.empty())
					SSL_write(ssl, event.data(),
						(int)event.size());
				if (read_auth) {
					char bytes[4096];

					SSL_read(ssl, bytes, sizeof(bytes));
				}
				SSL_shutdown(ssl);
			}
			SSL_free(ssl);
		});
	}

	std::string profile(bool password)
	{
		std::string text = "version=1\n[[inputs]]\nid='diagnostic'\n"
			"kind='tls'\nhost='127.0.0.1'\nport=" +
			std::to_string(port) + "\nuid='reader'\n"
			"protocol='xml-only'\n[inputs.tls]\nca_file='" +
			ca_path + "'\n";

		if (password)
			text +=
				"[inputs.auth]\nmode='password'\nusername='read"
					"er'\n"
				"password_file='" + password_path + "'\n";
		return text;
	}

	~DiagnosticServer()
	{
		stopping = true;
		if (worker.joinable())
			worker.join();
		SSL_CTX_free(tls);
	}
};

static std::string await_diagnostic(Runtime &runtime, const std::string &code)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);

	while (std::chrono::steady_clock::now() < end) {
		if (tg_runtime_poll(runtime.value, 5) != TG_OK)
			return "";
		Buffer snapshot;

		if (tg_runtime_snapshot(runtime.value, &snapshot.value) !=
			TG_OK)
			return "";
		std::string text = string(snapshot.value);

		if (text.find(code) != std::string::npos)
			return text;
	}
	return "";
}

} /* namespace */

TEST(ManagedDiagnostics, DefaultTlsPolicyRejectsAnotherCertificateName)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(false, false, "takserver"));
	server.start(false);
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(false), config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	EXPECT_FALSE(await_diagnostic(runtime,
		"tls-server-hostname-mismatch").empty());
}

TEST(ManagedDiagnostics, AtakTlsPolicyReceivesEventsWithAnotherCertificateName)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(false, false, "takserver"));
	server.event = fixtures::extended_xml();
	server.start(false);
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(false) +
		"verify_hostname=false\n", config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	EXPECT_FALSE(await_diagnostic(runtime,
		"\"events_received\":1").empty());
}

TEST(ManagedDiagnostics, AtakTlsPolicyRejectsAnExpiredServerCertificate)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(true, false, "takserver"));
	server.start(false);
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(false) +
		"verify_hostname=false\n", config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	EXPECT_FALSE(await_diagnostic(runtime,
		"tls-server-certificate-expired").empty());
}

TEST(ManagedDiagnostics, AtakTlsPolicyRejectsAnUntrustedServerCertificate)
{
	DiagnosticServer server, unrelated;

	ASSERT_TRUE(server.prepare(false, false, "takserver"));
	unrelated.ca_name = "Unrelated CA";
	ASSERT_TRUE(unrelated.prepare(false, false));
	server.start(false);
	Config config;
	Runtime runtime;
	std::string text = server.profile(false) + "verify_hostname=false\n";
	size_t at = text.find(server.ca_path);

	ASSERT_NE(at, std::string::npos);
	text.replace(at, server.ca_path.size(), unrelated.ca_path);
	ASSERT_EQ(managed_parse(text, config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	EXPECT_FALSE(await_diagnostic(runtime,
		"tls-server-certificate-untrusted").empty());
}

TEST(ManagedRuntime, ChangingTlsHostnameVerificationReplacesTheSource)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(false, false));
	Config initial, candidate;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(false), initial), TG_OK);
	ASSERT_EQ(managed_parse(server.profile(false) +
		"verify_hostname=false\n", candidate), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(initial.value, &runtime.value,
		nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_apply_config(runtime.value, candidate.value,
		nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_poll(runtime.value, 0), TG_OK);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("\"profile_revision\":2"),
		std::string::npos);
}

TEST(ManagedDiagnostics, ExpiredServerCertificateHasSpecificVerificationCode)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(true, false));
	server.start(false);
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(false), config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	std::string status = await_diagnostic(runtime,
		"tls-server-certificate-expired");

	ASSERT_FALSE(status.empty());
	EXPECT_NE(status.find("\"native_domain\":\"x509\""), std::string::npos);
	EXPECT_NE(status.find("\"attempts\":[{"), std::string::npos);
}

TEST(ManagedDiagnostics, PeerCertificateRequiredAlertIsConfirmedRejection)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(false, true));
	server.start(false);
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(false), config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	std::string status = await_diagnostic(runtime,
		"tls-client-certificate-rejected");

	ASSERT_FALSE(status.empty());
	EXPECT_NE(status.find("\"native_domain\":\"tls-alert\""),
		std::string::npos);
}

TEST(ManagedDiagnostics, CloseAfterCredentialsRetainsUnconfirmedEvidence)
{
	DiagnosticServer server;

	ASSERT_TRUE(server.prepare(false, false));
	server.start(true);
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse(server.profile(true), config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	std::string status = await_diagnostic(runtime, "auth-unconfirmed");

	ASSERT_FALSE(status.empty());
	EXPECT_NE(status.find("\"credentials_sent\":true"), std::string::npos);
	EXPECT_NE(status.find("\"state\":\"BACKOFF\""), std::string::npos);
	EXPECT_EQ(status.find("diagnostic-secret"), std::string::npos);
	EXPECT_EQ(status.find("incorrect password"), std::string::npos);
}
#endif

TEST(ManagedRuntime, ReorderingEndpointsKeepsLiveConsumerIdentity)
{
	GatewayFixture fixture;
	Socket reservation;
	uint16_t port = 0;

	ASSERT_TRUE(start_managed(fixture));
	ASSERT_TRUE(reserve(reservation, SOCK_STREAM, port));
	std::string first = "version=1\n[[inputs]]\nid='first'\nkind='tls'\n"
		"host='bad.example.invalid'\nuid='bad'\n[inputs.tls]\n"
		"ca_file='missing-managed-ca.pem'\n";
	std::string text = fixture.text.substr(fixture.text.find('\n') + 1);
	size_t at = text.find("[[outputs]]");

	ASSERT_NE(at, std::string::npos);
	text.insert(at, "[[outputs]]\nid='new-first'\nkind='tcp'\n"
		"bind='127.0.0.1'\nport=" + std::to_string(port) + "\n");
	Config candidate;

	ASSERT_EQ(managed_parse(first + text, candidate), TG_OK);
	reservation.close();
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_OK);
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
}

TEST(ManagedRuntime, RemovingAllEndpointsClosesOnlyTheirResources)
{
	GatewayFixture fixture;

	ASSERT_TRUE(start_managed(fixture));
	Config empty;

	ASSERT_EQ(managed_parse("version=1\n", empty), TG_OK);
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		empty.value, nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	char byte;

	EXPECT_EQ(recv(fixture.consumer.value, &byte, 1, 0), 0);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("\"inputs\":[]"),
		std::string::npos);
	EXPECT_NE(string(snapshot.value).find("\"outputs\":[]"),
		std::string::npos);
}

TEST(ManagedRuntime, ReplacingFramingRebindsSameListenerPort)
{
	GatewayFixture fixture;

	ASSERT_TRUE(start_managed(fixture));
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
	std::string text = fixture.text;
	size_t at = text.find("u32be");

	ASSERT_NE(at, std::string::npos);
	text.replace(at, 5, "xml-document");
	Config candidate;

	ASSERT_EQ(managed_parse(text, candidate), TG_OK);
	ASSERT_EQ(tg_runtime_apply_config(fixture.runtime.value,
		candidate.value, nullptr), TG_OK);
	ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	fixture.consumer.close();
	ASSERT_TRUE(fixture.connect_consumer());
	fixture.received.clear();
	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(xml.size()));
	EXPECT_EQ(fixture.received, xml);
}

TEST(ManagedConfiguration, DisabledEndpointsAreValidOnlyInManagedMode)
{
	std::string text = "version=1\n[[inputs]]\nid='idle'\nkind='udp'\n"
		"port=19001\nenabled=false\n[[outputs]]\nid='idle-out'\n"
		"kind='tcp'\nport=19002\nenabled=false\n";
	Config strict, managed;

	EXPECT_EQ(tg_config_parse(view(text), base_directory(), &strict.value,
		nullptr), TG_INVALID);
	ASSERT_EQ(managed_parse(text, managed), TG_OK);
	Runtime runtime;

	ASSERT_EQ(tg_runtime_create_managed(managed.value, &runtime.value,
		nullptr), TG_OK);
	EXPECT_EQ(tg_runtime_poll(runtime.value, 0), TG_OK);
}

TEST(ManagedDiagnostics, HistoryKeepsLatestTwentyFailuresAcrossEdits)
{
	Config config;
	Runtime runtime;

	ASSERT_EQ(managed_parse("version=1\n", config), TG_OK);
	ASSERT_EQ(tg_runtime_create_managed(config.value, &runtime.value,
		nullptr), TG_OK);
	for (unsigned i = 0; i < 25; ++i) {
		Config candidate;
		std::string text = "version=1\n[[inputs]]\nid='broken'\n"
			"kind='tls'\nhost='example.invalid'\nuid='reader'\n"
			"[inputs.tls]\nca_file='missing-managed-ca-" +
			std::to_string(i) + ".pem'\n";

		ASSERT_EQ(managed_parse(text, candidate), TG_OK);
		ASSERT_EQ(tg_runtime_apply_config(runtime.value,
			candidate.value, nullptr), TG_OK);
	}
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(runtime.value, &snapshot.value), TG_OK);
	std::string status = string(snapshot.value);
	size_t count = 0, at = 0;

	while ((at = status.find("\"attempt\":", at)) != std::string::npos) {
		++count;
		++at;
	}
	EXPECT_EQ(count, 21u);
	EXPECT_EQ(status.find("\"attempt\":1,"), std::string::npos);
	EXPECT_NE(status.find("\"attempt\":25,"), std::string::npos);
	std::string field = "\"failed_at_ms\":";

	at = status.find(field);
	ASSERT_NE(at, std::string::npos);
	uint64_t timestamp = std::stoull(status.substr(at + field.size()));
	auto now = std::chrono::system_clock::now().time_since_epoch();
	auto milliseconds =
		std::chrono::duration_cast<std::chrono::milliseconds>(now);
	uint64_t utc = static_cast<uint64_t>(milliseconds.count());

	EXPECT_LE(timestamp, utc);
	EXPECT_LT(utc - timestamp, 60000u);
}
