#include "import_fixtures.hpp"
#include "../../src/config_internal.h"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#endif

using namespace contract;
using namespace import_fixtures;

#ifdef _WIN32
static void expect_token_owner(std::string path)
{
	HANDLE token = nullptr;
	DWORD size = 0;

	ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY,
		&token));
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<uint8_t> storage(size);
	bool read = size && GetTokenInformation(token, TokenUser,
		storage.data(), size, &size);

	CloseHandle(token);
	ASSERT_TRUE(read);
	TOKEN_USER *user = reinterpret_cast<TOKEN_USER *>(storage.data());
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	PSID owner = nullptr;
	DWORD error = GetNamedSecurityInfoA(path.data(), SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
		&descriptor);

	ASSERT_EQ(error, ERROR_SUCCESS);
	EXPECT_TRUE(owner && EqualSid(owner, user->User.Sid));
	LocalFree(descriptor);
}
#endif

static tg_status parse_import(const Imported &imported, Config &config)
{
	std::string text = configuration(imported.toml.value);
	std::string base = std::filesystem::temp_directory_path().string();

	return tg_config_parse(view(text), base.c_str(), &config.value,
		nullptr);
}

TEST(ZipImport, WrappedMissionPackageProducesUsableCertificateInput)
{
	Package fixture;
	Imported imported;
	tg_diagnostic diagnostic{};

	ASSERT_TRUE(fixture.prepare());
	std::string path = fixture.write(zip(fixture.entries()));

	ASSERT_FALSE(path.empty());
	ASSERT_EQ(imported.read(path, &diagnostic), TG_OK);
	ASSERT_NE(imported.files, nullptr);
	EXPECT_EQ(diagnostic.status, TG_OK);
	EXPECT_STREQ(diagnostic.key, "");
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	ASSERT_EQ(config.value->input_count, 1u);
	const tg_input_config &input = config.value->inputs[0];

	EXPECT_STREQ(input.id, "atak-0");
	EXPECT_STREQ(input.host, "tak.example.invalid");
	EXPECT_EQ(input.port, 28089);
	EXPECT_EQ(input.session.protocol, TG_PROTOCOL_XML_ONLY);
	EXPECT_EQ(input.username, nullptr);
	EXPECT_EQ(input.password_file, nullptr);
	EXPECT_NE(input.ca_file, nullptr);
	EXPECT_NE(input.cert_file, nullptr);
	EXPECT_NE(input.key_file, nullptr);
	ASSERT_EQ(tg_config_validate(config.value, nullptr), TG_OK);
	Runtime runtime;

	EXPECT_EQ(tg_runtime_create(config.value, &runtime.value, nullptr),
		TG_OK);
	EXPECT_EQ(string(imported.toml.value).find("test-only-secret"),
		std::string::npos);
	EXPECT_EQ(string(imported.toml.value).find("PRIVATE KEY"),
		std::string::npos);
}

TEST(ZipImport, TransportWrapperSuppliesUniqueIdentityForEnrollmentPackage)
{
	Package fixture;
	Imported imported;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(imported.read(fixture.wrapped()), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	ASSERT_EQ(config.value->input_count, 1u);
	EXPECT_STREQ(config.value->inputs[0].host, "tak.example.invalid");
	EXPECT_EQ(config.value->inputs[0].username, nullptr);
	ASSERT_EQ(tg_config_validate(config.value, nullptr), TG_OK);
	Runtime runtime;

	EXPECT_EQ(tg_runtime_create(config.value, &runtime.value, nullptr),
		TG_OK);
}

TEST(ZipImport, LegacyPkcs12EncryptionImportsWithoutGlobalProviderChanges)
{
	Package fixture;
	Imported imported;
	EVP_CIPHER *before = EVP_CIPHER_fetch(nullptr, "RC2-40-CBC", nullptr);
	bool previously_available = before != nullptr;

	EVP_CIPHER_free(before);
	ERR_clear_error();
	ASSERT_TRUE(fixture.prepare(true));
	ASSERT_EQ(imported.read(fixture.wrapped()), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	Runtime runtime;

	EXPECT_EQ(tg_runtime_create(config.value, &runtime.value, nullptr),
		TG_OK);
	EVP_CIPHER *after = EVP_CIPHER_fetch(nullptr, "RC2-40-CBC", nullptr);

	EXPECT_EQ(after != nullptr, previously_available);
	EVP_CIPHER_free(after);
	ERR_clear_error();
}

TEST(ZipImport, ExplicitEmptyClientPasswordDoesNotUseTheCaPassword)
{
	Package fixture;
	Imported imported;
	std::string pref = preferences();
	std::string original = entry("clientPassword0", "test-only-secret");
	size_t offset = pref.find(original);

	ASSERT_NE(offset, std::string::npos);
	pref.replace(offset, original.size(), entry("clientPassword0", ""));
	ASSERT_TRUE(fixture.prepare(false, ""));
	std::string path = fixture.write(zip(fixture.entries(true, pref)));

	ASSERT_EQ(imported.read(path), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	Runtime runtime;

	EXPECT_EQ(tg_runtime_create(config.value, &runtime.value, nullptr),
		TG_OK);
}

TEST(ZipImport, GlobalCertificatePreferencesSupplyConnectionDefaults)
{
	Package fixture;
	Imported imported;
	std::string pref = "<preferences><preference version='1' "
		"name='cot_streams'>" + entry("count", "1") +
		entry("connectString0", "tak.example.invalid:28089:ssl") +
		entry("enabled0", "true") + "</preference>"
		"<preference version='1' name='com.atakmap.app_preferences'>" +
		entry("caLocation", "cert/trust.p12") +
		entry("caPassword", "test-only-secret") +
		entry("certificateLocation", "cert/client.p12") +
		entry("clientPassword", "test-only-secret") +
		"</preference></preferences>";

	ASSERT_TRUE(fixture.prepare());
	std::string path = fixture.write(zip(fixture.entries(true, pref)));

	ASSERT_EQ(imported.read(path), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	EXPECT_EQ(tg_config_validate(config.value, nullptr), TG_OK);
}

TEST(ZipImport, DisabledStreamsAreNotImportedOrConnected)
{
	Package fixture;
	Imported imported;
	std::string pref = "<preferences><preference version='1' "
		"name='cot_streams'>" + entry("count", "2") +
		entry("connectString0", "disabled.invalid:1234:tcp") +
		entry("enabled0", "false") +
		entry("connectString1", "tak.example.invalid:28089:ssl") +
		entry("enabled1", "true") +
		entry("caLocation1", "cert/trust.p12") +
		entry("caPassword1", "test-only-secret") +
		entry("certificateLocation1", "cert/client.p12") +
		entry("clientPassword1", "test-only-secret") +
		"</preference></preferences>";

	ASSERT_TRUE(fixture.prepare());
	std::string path = fixture.write(zip(fixture.entries(true, pref)));

	ASSERT_EQ(imported.read(path), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	ASSERT_EQ(config.value->input_count, 1u);
	EXPECT_STREQ(config.value->inputs[0].host, "tak.example.invalid");
	EXPECT_EQ(tg_config_validate(config.value, nullptr), TG_OK);
}

TEST(ZipImport, MultipleEnabledStreamsRetainDistinctConnectionSettings)
{
	Package fixture;
	Imported imported;
	std::string pref = "<preferences><preference version='1' "
		"name='cot_streams'>" + entry("count", "2");

	for (unsigned i = 0; i < 2; ++i) {
		std::string index = std::to_string(i);

		pref += entry("connectString" + index, i ?
			"second.example.invalid:28090:ssl" :
			"tak.example.invalid:28089:ssl") +
			entry("enabled" + index, "true") +
			entry("caLocation" + index, "cert/trust.p12") +
			entry("caPassword" + index, "test-only-secret") +
			entry("certificateLocation" + index,
				"cert/client.p12") +
			entry("clientPassword" + index, "test-only-secret");
	}
	pref += "</preference></preferences>";
	ASSERT_TRUE(fixture.prepare());
	std::string path = fixture.write(zip(fixture.entries(true, pref)));

	ASSERT_EQ(imported.read(path), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	ASSERT_EQ(config.value->input_count, 2u);
	EXPECT_STREQ(config.value->inputs[0].host, "tak.example.invalid");
	EXPECT_EQ(config.value->inputs[0].port, 28089);
	EXPECT_STREQ(config.value->inputs[1].host, "second.example.invalid");
	EXPECT_EQ(config.value->inputs[1].port, 28090);
	EXPECT_STRNE(config.value->inputs[0].id, config.value->inputs[1].id);
	EXPECT_STRNE(config.value->inputs[0].session.uid,
		config.value->inputs[1].session.uid);
	EXPECT_EQ(tg_config_validate(config.value, nullptr), TG_OK);
}

TEST(ZipImport, CredentialPemContainsTrustAndMatchingClientIdentity)
{
	Package fixture;
	Imported imported;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(imported.read(fixture.wrapped()), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	const tg_input_config &input = config.value->inputs[0];
	BIO *ca_file = BIO_new_file(input.ca_file, "r");
	BIO *cert_file = BIO_new_file(input.cert_file, "r");
	BIO *key_file = BIO_new_file(input.key_file, "r");

	ASSERT_NE(ca_file, nullptr);
	ASSERT_NE(cert_file, nullptr);
	ASSERT_NE(key_file, nullptr);
	X509 *ca = PEM_read_bio_X509(ca_file, nullptr, nullptr, nullptr);
	X509 *cert = PEM_read_bio_X509(cert_file, nullptr, nullptr, nullptr);
	EVP_PKEY *key = PEM_read_bio_PrivateKey(key_file, nullptr, nullptr,
		nullptr);

	ASSERT_NE(ca, nullptr);
	ASSERT_NE(cert, nullptr);
	ASSERT_NE(key, nullptr);
	EXPECT_EQ(X509_check_private_key(cert, key), 1);
	EXPECT_EQ(X509_check_ca(ca), 1);
	X509_free(ca);
	X509_free(cert);
	EVP_PKEY_free(key);
	BIO_free(ca_file);
	BIO_free(cert_file);
	BIO_free(key_file);
}

TEST(ZipImport, OwnedCredentialsArePrivateAndRemovedOnDestroy)
{
	Package fixture;
	Imported imported;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(imported.read(fixture.wrapped()), TG_OK);
	Config config;

	ASSERT_EQ(parse_import(imported, config), TG_OK);
	const tg_input_config &input = config.value->inputs[0];
	std::vector<std::string> paths = {
		input.ca_file, input.cert_file, input.key_file};
	std::filesystem::path directory =
		std::filesystem::path(input.key_file).parent_path();

	for (const auto &path : paths) {
		EXPECT_TRUE(std::filesystem::is_regular_file(path));
#ifdef _WIN32
		expect_token_owner(path);
#else
		struct stat metadata{};

		ASSERT_EQ(stat(path.c_str(), &metadata), 0);
		EXPECT_EQ(metadata.st_mode & 0777, 0600);
#endif
	}
#ifdef _WIN32
	expect_token_owner(directory.string());
#else
	struct stat metadata{};

	ASSERT_EQ(stat(directory.string().c_str(), &metadata), 0);
	EXPECT_EQ(metadata.st_mode & 0777, 0700);
#endif
	tg_config_destroy(config.value);
	config.value = nullptr;
	tg_import_zip_destroy(imported.files);
	imported.files = nullptr;
	for (const auto &path : paths)
		EXPECT_FALSE(std::filesystem::exists(path));
	EXPECT_FALSE(std::filesystem::exists(directory));
	EXPECT_TRUE(std::filesystem::exists(fixture.directory));
}

static void rejected(const std::string &path)
{
	Imported imported;
	tg_diagnostic diagnostic{};

	EXPECT_LT(imported.read(path, &diagnostic), TG_OK);
	EXPECT_EQ(imported.files, nullptr);
	EXPECT_EQ(imported.toml.value.data, nullptr);
	EXPECT_EQ(imported.toml.value.size, 0u);
	EXPECT_LT(diagnostic.status, TG_OK);
	EXPECT_EQ(std::string(diagnostic.key).find("test-only-secret"),
		std::string::npos);
}

TEST(ZipImport, RejectsArchiveTraversalAbsoluteAndDuplicateNames)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	for (const auto &name : {"../outside", "/absolute", "C:/absolute",
		     "package/../outside", "package\\outside"}) {
		Entries entries = fixture.entries();

		entries.emplace_back(name, Bytes{1});
		SCOPED_TRACE(name);
		rejected(fixture.write(zip(entries)));
	}
	Entries entries = fixture.entries();

	entries.push_back(entries.front());
	rejected(fixture.write(zip(entries)));
}

TEST(ZipImport, RejectsXmlEntitiesAndTraversalReferences)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string entity = "<!DOCTYPE preferences [<!ENTITY leaked SYSTEM "
		"'file:///not-a-real-secret'>]>" + preferences();
	std::string traversal = preferences();
	size_t location = traversal.find("cert/trust.p12");

	ASSERT_NE(location, std::string::npos);
	traversal.replace(location, strlen("cert/trust.p12"), "../trust.p12");
	for (const auto &text : {entity, traversal})
		rejected(fixture.write(zip(fixture.entries(true, text))));
}

TEST(ZipImport, RejectsInvalidEndpointsAndEnabledPlaintextStreams)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	const std::string valid = "tak.example.invalid:28089:ssl";

	for (const std::string endpoint : {":28089:ssl",
		     "tak.example.invalid:0:ssl",
		     "tak.example.invalid:65536:ssl",
		     "tak.example.invalid:no-port:ssl",
		     "tak.example.invalid:28089:tcp",
		     "https://tak.example.invalid:8443:ssl",
		     "user@tak.example.invalid:28089:ssl"}) {
		std::string pref = preferences();
		size_t begin = pref.find(valid);

		ASSERT_NE(begin, std::string::npos);
		pref.replace(begin, valid.size(), endpoint);
		SCOPED_TRACE(endpoint);
		rejected(fixture.write(zip(fixture.entries(true, pref))));
	}
}

TEST(ZipImport, ExplicitEmptyEnabledValueIsInvalid)
{
	Package fixture;
	std::string pref = preferences();
	std::string original = entry("enabled0", "true");
	size_t offset = pref.find(original);

	ASSERT_NE(offset, std::string::npos);
	pref.replace(offset, original.size(), entry("enabled0", ""));
	ASSERT_TRUE(fixture.prepare());
	rejected(fixture.write(zip(fixture.entries(true, pref))));
}

TEST(ZipImport, RejectsMissingIdentityAndAmbiguousOuterIdentities)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	rejected(fixture.write(zip(fixture.entries(false))));
	Bytes inner = zip(fixture.entries(false));

	rejected(fixture.write(zip({{"server.zip", inner},
		{"first.p12", fixture.client},
		{"second.p12", fixture.client}})));
}

TEST(ZipImport, RejectsWrongStorePasswordAndMissingReferences)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	rejected(fixture.write(zip(fixture.entries(true,
		preferences(true, "wrong-test-secret")))));
	Entries entries = fixture.entries();

	entries.pop_back();
	rejected(fixture.write(zip(entries)));
}

TEST(ZipImport, RejectsPkcs12WithoutIntegrityProtection)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare(false, "test-only-secret", -1));
	rejected(fixture.write(zip(fixture.entries())));
}

TEST(ZipImport, RejectsPkcs12MacAboveTheWorkLimit)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare(false, "test-only-secret", 1000001));
	rejected(fixture.write(zip(fixture.entries())));
}

TEST(ZipImport, RejectsAmbiguousPackagesAndExcessiveNesting)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	Bytes inner = zip(fixture.entries());

	rejected(fixture.write(zip({{"one.zip", inner}, {"two.zip", inner}})));
	rejected(fixture.write(zip({{"one.zip", zip({{"two.zip", inner}})}})));
}

TEST(ZipImport, RejectsOversizedXmlAndExcessiveArchiveEntries)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string large(1024 * 1024 + 1, ' ');

	rejected(fixture.write(zip(fixture.entries(true, large))));
	Entries entries = fixture.entries();

	for (unsigned i = 0; i < 257; ++i)
		entries.emplace_back("extra-" + std::to_string(i), Bytes{});
	rejected(fixture.write(zip(entries)));
}

TEST(ZipImport, RejectsArchiveBeyondTheDocumentedReadLimit)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	Bytes oversized(16 * 1024 * 1024 + 1, 0);

	rejected(fixture.write(oversized));
}

TEST(ZipImport, RejectsTruncatedArchiveWithoutPublishingOutputs)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	Bytes archive = zip(fixture.entries());

	archive.resize(archive.size() - 10);
	rejected(fixture.write(archive));
	tg_import_zip_destroy(nullptr);
}

TEST(ZipImport, InvalidOutputArgumentsAreRejectedWithoutMutation)
{
	tg_diagnostic diagnostic{};
	tg_import_files *files = nullptr;
	tg_buffer empty{};
	uint8_t sentinel[] = {'x'};
	tg_buffer nonempty{sentinel, sizeof(sentinel)};

	EXPECT_EQ(tg_import_zip(nullptr, &empty, &files, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(tg_import_zip("", &empty, &files, &diagnostic), TG_INVALID);
	EXPECT_EQ(tg_import_zip("unused.zip", nullptr, &files, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(tg_import_zip("unused.zip", &empty, nullptr, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(tg_import_zip("unused.zip", &nonempty, &files, &diagnostic),
		TG_INVALID);
	EXPECT_EQ(nonempty.data, sentinel);
	EXPECT_EQ(nonempty.size, sizeof(sentinel));
	EXPECT_EQ(empty.data, nullptr);
	EXPECT_EQ(empty.size, 0u);
	EXPECT_EQ(files, nullptr);
}

#ifndef _WIN32
TEST(ZipImport, FailureAfterOneStreamRemovesAllPartialCredentialFiles)
{
	Package fixture;

	ASSERT_TRUE(fixture.prepare());
	std::string pref = preferences();
	std::string original = entry("count", "1");
	size_t count = pref.find(original);

	ASSERT_NE(count, std::string::npos);
	pref.replace(count, original.size(), entry("count", "2"));
	size_t end = pref.find("</preference>");

	ASSERT_NE(end, std::string::npos);
	pref.insert(end, entry("enabled1", "true") +
		entry("connectString1", "broken-endpoint"));
	std::string path = fixture.write(zip(fixture.entries(true, pref)));
	std::filesystem::path temporary = fixture.directory / "temporary";

	ASSERT_TRUE(std::filesystem::create_directory(temporary));
	const char *previous = getenv("TMPDIR");
	bool had_previous = previous != nullptr;
	std::string previous_value = previous ? previous : "";

	ASSERT_EQ(setenv("TMPDIR", temporary.string().c_str(), 1), 0);
	rejected(path);
	int restored = had_previous ?
		setenv("TMPDIR", previous_value.c_str(), 1) :
		unsetenv("TMPDIR");

	EXPECT_EQ(restored, 0);
	EXPECT_TRUE(std::filesystem::is_empty(temporary));
}
#endif
