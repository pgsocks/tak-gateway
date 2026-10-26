#include "import_fixtures.hpp"
#include "../../src/store.h"
#include "../../src/config_internal.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

using namespace contract;
using namespace import_fixtures;

namespace {

struct Store {
	tg_store *value = nullptr;
	~Store() { tg_store_close(value); }
};

static tg_status configuration_for(Package &fixture, Config &config)
{
	std::string text = fixtures::minimal_config();

	return tg_config_parse(view(text), fixture.directory.string().c_str(),
		&config.value, nullptr);
}

#ifdef _WIN32
static void expect_private_user_owned(std::string path)
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
	PACL acl = nullptr;
	DWORD result = GetNamedSecurityInfoA(path.data(), SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		&owner, nullptr, &acl, nullptr, &descriptor);

	ASSERT_EQ(result, ERROR_SUCCESS) << path;
	EXPECT_TRUE(owner && EqualSid(owner, user->User.Sid)) << path;
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;

	EXPECT_TRUE(GetSecurityDescriptorControl(descriptor, &control,
		&revision));
	EXPECT_NE(control & SE_DACL_PROTECTED, 0) << path;
	EXPECT_NE(acl, nullptr) << path;
	bool user_allowed = false, system_allowed = false;

	for (DWORD i = 0; acl && i < acl->AceCount; ++i) {
		void *entry = nullptr;

		if (!GetAce(acl, i, &entry)) {
			ADD_FAILURE() << "Cannot read ACL entry for " << path;
			break;
		}
		ACE_HEADER *header = static_cast<ACE_HEADER *>(entry);

		EXPECT_EQ(header->AceType, ACCESS_ALLOWED_ACE_TYPE) << path;
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
			continue;
		ACCESS_ALLOWED_ACE *ace =
			static_cast<ACCESS_ALLOWED_ACE *>(entry);
		PSID sid = &ace->SidStart;
		bool current = EqualSid(sid, user->User.Sid) != 0;
		bool system = IsWellKnownSid(sid, WinLocalSystemSid) != 0;

		EXPECT_TRUE(current || system) << path;
		EXPECT_EQ(header->AceFlags & INHERIT_ONLY_ACE, 0) << path;
		EXPECT_EQ(ace->Mask & FILE_ALL_ACCESS, FILE_ALL_ACCESS) << path;
		user_allowed = user_allowed || current;
		system_allowed = system_allowed || system;
	}
	EXPECT_TRUE(user_allowed) << path;
	EXPECT_TRUE(system_allowed) << path;
	LocalFree(descriptor);
}

TEST(Store, WindowsStateAndCredentialsHaveExplicitPrivateUserOwnership)
{
	Package fixture;
	Store store;
	Config config;
	Buffer asset;
	tg_web_settings web{};
	tg_diagnostic diagnostic{};
	uint64_t revision = 0;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(configuration_for(fixture, config), TG_OK);
	std::filesystem::path path = fixture.directory / "state";

	ASSERT_EQ(tg_store_open(path.string().c_str(), true, &store.value,
		&diagnostic), TG_OK) << diagnostic.key;
	ASSERT_EQ(tg_store_write_asset(store.value, "pem", view("test-data"),
		&asset.value, &diagnostic), TG_OK) << diagnostic.key;
	tg_web_settings_default(&web);
	ASSERT_EQ(tg_store_save(store.value, config.value, &web, 0, &revision,
		&diagnostic), TG_OK) << diagnostic.key;
	tg_store_close(store.value);
	store.value = nullptr;
	expect_private_user_owned(path.string());
	for (const auto &entry : std::filesystem::recursive_directory_iterator(
		path))
		expect_private_user_owned(entry.path().string());
}
#endif

TEST(Store, UninitializedReaderDoesNotCreateState)
{
	Package fixture;
	Store reader;
	Buffer text;
	tg_web_settings web{};
	uint64_t revision = 99;

	ASSERT_TRUE(fixture.prepare());
	std::string path = (fixture.directory / "state").string();

	ASSERT_EQ(tg_store_open(path.c_str(), false, &reader.value, nullptr),
		TG_OK);
	ASSERT_EQ(tg_store_load(reader.value, &text.value, &web, &revision,
		nullptr), TG_OK);
	EXPECT_EQ(revision, 0u);
	EXPECT_FALSE(web.enabled);
	EXPECT_STREQ(web.bind, "127.0.0.1:8080");
	EXPECT_NE(string(text.value).find("version"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Store, WriterLockExcludesWritersWhileReadersSeeAtomicRevisions)
{
	Package fixture;
	Store writer, competing, reader;
	Config config;
	Buffer text;
	tg_web_settings web{};
	uint64_t revision = 0, saved = 0;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(configuration_for(fixture, config), TG_OK);
	std::string path = (fixture.directory / "state").string();

	ASSERT_EQ(tg_store_open(path.c_str(), true, &writer.value, nullptr),
		TG_OK);
	EXPECT_EQ(tg_store_open(path.c_str(), true, &competing.value, nullptr),
		TG_BAD_STATE);
	EXPECT_EQ(competing.value, nullptr);
	ASSERT_EQ(tg_store_open(path.c_str(), false, &reader.value, nullptr),
		TG_OK);
	tg_web_settings_default(&web);
	web.enabled = true;
	snprintf(web.bind, sizeof(web.bind), "0.0.0.0:18080");
	ASSERT_EQ(tg_store_save(writer.value, config.value, &web, 0, &saved,
		nullptr), TG_OK);
	EXPECT_GT(saved, 0u);
	ASSERT_EQ(tg_store_load(reader.value, &text.value, &web, &revision,
		nullptr), TG_OK);
	EXPECT_EQ(revision, saved);
	EXPECT_TRUE(web.enabled);
	EXPECT_STREQ(web.bind, "0.0.0.0:18080");
	uint64_t stale = 99;

	EXPECT_EQ(tg_store_save(writer.value, config.value, &web, 0, &stale,
		nullptr), TG_BAD_STATE);
	EXPECT_EQ(stale, 0u);
	EXPECT_EQ(tg_store_save(reader.value, config.value, &web, saved,
		&stale, nullptr), TG_BAD_STATE);
}

TEST(Store, CorruptPublishedStateCannotBeOpenedOrOverwritten)
{
	Package fixture;
	Store writer, reader;
	Config config;
	tg_web_settings web{};
	uint64_t revision = 0;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(configuration_for(fixture, config), TG_OK);
	std::filesystem::path path = fixture.directory / "state";

	ASSERT_EQ(tg_store_open(path.string().c_str(), true, &writer.value,
		nullptr), TG_OK);
	std::ofstream pointer(path / "CURRENT");

	pointer << "corrupt-state\n";
	pointer.close();
	std::filesystem::permissions(path / "CURRENT",
		std::filesystem::perms::owner_read |
		std::filesystem::perms::owner_write);
	EXPECT_LT(tg_store_open(path.string().c_str(), false, &reader.value,
		nullptr), TG_OK);
	tg_web_settings_default(&web);
	EXPECT_LT(tg_store_save(writer.value, config.value, &web, 0, &revision,
		nullptr), TG_OK);
	std::ifstream unchanged(path / "CURRENT");
	std::string content((std::istreambuf_iterator<char>(unchanged)), {});

	EXPECT_EQ(content, "corrupt-state\n");
	EXPECT_EQ(revision, 0u);
}

TEST(Store, ImportedCredentialsSurviveTemporaryImportDestruction)
{
	Package fixture;
	Imported imported;
	Store store;
	Config config, persisted;
	Buffer text;
	tg_web_settings web{};
	uint64_t revision = 0;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(imported.read(fixture.wrapped()), TG_OK);
	std::string source = configuration(imported.toml.value);
	std::string path = (fixture.directory / "state").string();

	ASSERT_EQ(tg_config_parse(view(source),
		fixture.directory.string().c_str(), &config.value, nullptr),
		TG_OK);
	ASSERT_EQ(tg_store_open(path.c_str(), true, &store.value, nullptr),
		TG_OK);
	tg_web_settings_default(&web);
	ASSERT_EQ(tg_store_save_import(store.value, config.value, &web, 0,
		imported.files, &revision, nullptr), TG_OK);
	tg_import_zip_destroy(imported.files);
	imported.files = nullptr;
	ASSERT_EQ(tg_store_load(store.value, &text.value, &web, &revision,
		nullptr), TG_OK);
	ASSERT_EQ(tg_config_parse(view(string(text.value)), path.c_str(),
		&persisted.value, nullptr), TG_OK);
	ASSERT_EQ(tg_config_validate(persisted.value, nullptr), TG_OK);
	EXPECT_NE(std::string(persisted.value->inputs[0].key_file).find(path),
		std::string::npos);
	EXPECT_NE(std::string(persisted.value->inputs[0].key_file),
		std::string(config.value->inputs[0].key_file));
}

TEST(Store, AssetsArePrivateAndRemovalIsConfinedToOwnedDirectory)
{
	Package fixture;
	Store store;
	Buffer asset;

	ASSERT_TRUE(fixture.prepare());
	std::string path = (fixture.directory / "state").string();
	std::string outside = fixture.write(Bytes{1}, "outside");

	ASSERT_EQ(tg_store_open(path.c_str(), true, &store.value, nullptr),
		TG_OK);
	ASSERT_EQ(tg_store_write_asset(store.value, "pem", view("test-data"),
		&asset.value, nullptr), TG_OK);
	std::string saved = string(asset.value);

	EXPECT_TRUE(std::filesystem::is_regular_file(saved));
#ifndef _WIN32
	struct stat info{};

	ASSERT_EQ(stat(path.c_str(), &info), 0);
	EXPECT_EQ(info.st_mode & 0777, 0700);
	ASSERT_EQ(stat(saved.c_str(), &info), 0);
	EXPECT_EQ(info.st_mode & 0777, 0600);
#endif
	EXPECT_EQ(tg_store_remove_asset(store.value, outside.c_str(), nullptr),
		TG_INVALID);
	EXPECT_TRUE(std::filesystem::exists(outside));
	EXPECT_EQ(tg_store_remove_asset(store.value, saved.c_str(), nullptr),
		TG_OK);
	EXPECT_FALSE(std::filesystem::exists(saved));
	EXPECT_EQ(tg_store_remove_asset(store.value, saved.c_str(), nullptr),
		TG_OK);
}

TEST(Store, ExplicitStateSelectorResolvesAgainstCwd)
{
	Package fixture;
	Buffer selected;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(tg_store_select("selected",
		fixture.directory.string().c_str(), &selected.value, nullptr),
		TG_OK);
	EXPECT_EQ(std::filesystem::path(string(selected.value)),
		fixture.directory / "selected");
}

#ifndef _WIN32
struct EnvironmentValue {
	const char *name;
	bool existed;
	std::string value;

	explicit EnvironmentValue(const char *key) : name(key)
	{
		const char *previous = getenv(key);

		existed = previous != nullptr;
		value = previous ? previous : "";
	}

	~EnvironmentValue()
	{
		if (existed)
			setenv(name, value.c_str(), 1);
		else
			unsetenv(name);
	}
};

TEST(Store, StateSelectorPrefersCliThenEnvironmentThenPlatformState)
{
	Package fixture;
	EnvironmentValue override("TAK_GATEWAY_STATE_DIR");
	EnvironmentValue platform("XDG_STATE_HOME");
	Buffer selected;

	ASSERT_TRUE(fixture.prepare());
	std::string cwd = fixture.directory.string();
	std::string xdg = (fixture.directory / "platform").string();

	ASSERT_EQ(setenv("TAK_GATEWAY_STATE_DIR", "environment", 1), 0);
	ASSERT_EQ(setenv("XDG_STATE_HOME", xdg.c_str(), 1), 0);
	ASSERT_EQ(tg_store_select("command", cwd.c_str(), &selected.value,
		nullptr), TG_OK);
	EXPECT_EQ(std::filesystem::path(string(selected.value)),
		fixture.directory / "command");
	tg_buffer_release(&selected.value);
	ASSERT_EQ(tg_store_select(nullptr, cwd.c_str(), &selected.value,
		nullptr), TG_OK);
	EXPECT_EQ(std::filesystem::path(string(selected.value)),
		fixture.directory / "environment");
	tg_buffer_release(&selected.value);
	ASSERT_EQ(unsetenv("TAK_GATEWAY_STATE_DIR"), 0);
	ASSERT_EQ(tg_store_select(nullptr, cwd.c_str(), &selected.value,
		nullptr), TG_OK);
	EXPECT_EQ(std::filesystem::path(string(selected.value)),
		fixture.directory / "platform" / "tak-gateway");
}
#endif

TEST(Store, ExternalCredentialAndEnvironmentReferencesAreRetained)
{
	Package fixture;
	Config config, persisted;
	Store store;
	Buffer text;
	tg_web_settings web{};
	uint64_t revision = 0;

	ASSERT_TRUE(fixture.prepare());
	const char source[] = "version=1\n[[inputs]]\nid='external'\n"
		"kind='tls'\nhost='server.invalid'\nuid='external'\n"
		"[inputs.tls]\nca_file='external-ca.pem'\n"
		"[inputs.auth]\nmode='password'\nusername='reader'\n"
		"password_env='CONTRACT_SECRET_REFERENCE_ONLY'\n"
		"[[outputs]]\nid='capture'\nkind='udp'\nport=19000\n";
	std::string path = (fixture.directory / "state").string();

	ASSERT_EQ(tg_config_parse(view(source),
		fixture.directory.string().c_str(), &config.value, nullptr),
		TG_OK);
	ASSERT_EQ(tg_store_open(path.c_str(), true, &store.value, nullptr),
		TG_OK);
	tg_web_settings_default(&web);
	ASSERT_EQ(tg_store_save(store.value, config.value, &web, 0, &revision,
		nullptr), TG_OK);
	ASSERT_EQ(tg_store_load(store.value, &text.value, &web, &revision,
		nullptr), TG_OK);
	ASSERT_EQ(tg_config_parse(view(string(text.value)), path.c_str(),
		&persisted.value, nullptr), TG_OK);
	EXPECT_STREQ(persisted.value->inputs[0].ca_file,
		config.value->inputs[0].ca_file);
	EXPECT_STREQ(persisted.value->inputs[0].password_env,
		"CONTRACT_SECRET_REFERENCE_ONLY");
	EXPECT_TRUE(std::filesystem::is_empty(
		fixture.directory / "state" / "assets"));
}

TEST(Store, PrefixNamedExternalCredentialRemainsAnExternalReference)
{
	Package fixture;
	Imported imported;
	Config config, persisted;
	Store store;
	Buffer text;
	tg_web_settings web{};
	uint64_t revision = 0;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(imported.read(fixture.wrapped()), TG_OK);
	std::filesystem::path external =
		fixture.directory / "tak-gateway-production";

	ASSERT_TRUE(std::filesystem::create_directory(external));
	std::ofstream certificate(external / "ca.pem");

	certificate << "rotation-managed-external-certificate";
	certificate.close();
	std::filesystem::permissions(external / "ca.pem",
		std::filesystem::perms::owner_read |
		std::filesystem::perms::owner_write);
	const char source[] = "version=1\n[[inputs]]\nid='external'\n"
		"kind='tls'\nhost='server.invalid'\nuid='external'\n"
		"[inputs.tls]\nca_file='tak-gateway-production/ca.pem'\n"
		"[[outputs]]\nid='capture'\nkind='udp'\nport=19000\n";
	std::string path = (fixture.directory / "state").string();

	ASSERT_EQ(tg_config_parse(view(source),
		fixture.directory.string().c_str(), &config.value, nullptr),
		TG_OK);
	ASSERT_EQ(tg_store_open(path.c_str(), true, &store.value, nullptr),
		TG_OK);
	tg_web_settings_default(&web);
	ASSERT_EQ(tg_store_save_import(store.value, config.value, &web, 0,
		imported.files, &revision, nullptr), TG_OK);
	ASSERT_EQ(tg_store_load(store.value, &text.value, &web, &revision,
		nullptr), TG_OK);
	ASSERT_EQ(tg_config_parse(view(string(text.value)), path.c_str(),
		&persisted.value, nullptr), TG_OK);
	EXPECT_STREQ(persisted.value->inputs[0].ca_file,
		config.value->inputs[0].ca_file);
	EXPECT_TRUE(std::filesystem::is_empty(
		fixture.directory / "state" / "assets"));
}

#ifndef _WIN32
TEST(Store, FifoCurrentIsRejectedWithoutBlocking)
{
	Package fixture;
	Store store;

	ASSERT_TRUE(fixture.prepare());
	std::filesystem::path path = fixture.directory / "state";

	ASSERT_EQ(tg_store_open(path.string().c_str(), true, &store.value,
		nullptr), TG_OK);
	ASSERT_EQ(mkfifo((path / "CURRENT").string().c_str(), 0600), 0);
	pid_t child = fork();

	ASSERT_GE(child, 0);
	if (!child) {
		tg_buffer text{};
		tg_web_settings web{};
		uint64_t revision = 0;
		tg_status status = tg_store_load(store.value, &text, &web,
			&revision, nullptr);

		_exit(status < 0 ? 0 : 1);
	}
	bool finished = false;
	int status = 0;

	for (unsigned attempt = 0; attempt < 100; ++attempt) {
		if (waitpid(child, &status, WNOHANG) == child) {
			finished = true;
			break;
		}
		usleep(10000);
	}
	if (!finished) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
	}
	EXPECT_TRUE(finished) << "store read blocked on FIFO CURRENT";
	if (finished) {
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), 0);
	}
}
#endif

} /* namespace */
