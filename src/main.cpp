#include "config_internal.h"
#include "cli_options.h"
#include "managed_runtime.h"
#include "web_server.h"
#include <signal.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t interrupted;

static void interrupt_handler(int)
{
	interrupted = 1;
}

static int report(tg_status status, const tg_diagnostic *diagnostic, int code)
{
	fprintf(stderr, "tak-gateway: status %d", (int)status);
	if (diagnostic && diagnostic->key[0])
		fprintf(stderr, " at %s", diagnostic->key);
	fputc('\n', stderr);
	return code;
}

static int usage(void)
{
	fputs("usage: tak-gateway {config|run|validate|redact} [OPTIONS]\n"
		"  --state-dir DIRECTORY         Persistent state directory\n"
		"config saves settings offline; run loads saved settings.\n"
		"Configuration arguments (config command only):\n"
		"  --config FILE                 Replace gateway settings\n"
		"  --import-zip ZIP              Add ATAK package connections\n"
		"  --output ID=URI               Replace or append an output\n"
		"    tcp://BIND:PORT | tcp-connect://HOST:PORT\n"
		"    udp://HOST:PORT | pipe:///ABSOLUTE/PATH\n"
		"  --set PATH=TOML_VALUE         Edit any configuration field\n"
		"  --web-bind IP:PORT            Enable and bind the web GUI\n"
		"    Prefer localhost; remote binds allow remote control.\n"
		"    --set web.enabled=false disables the saved GUI.\n"
		"  --status-file PATH            Runtime status snapshot\n"
		"  --log-level LEVEL             info/debug/warning/error\n"
		"  --log-format FORMAT           json/text\n"
		"  --max-message-bytes N         Maximum event size\n"
		"  --max-xml-depth N             Maximum XML nesting\n"
		"  --retained-bytes N            Retained-event budget\n"
		"  --queue-bytes N               Default output byte budget\n"
		"  --queue-records N             Default output record budget\n"
		"  --max-inputs N                Maximum configured inputs\n"
		"  --max-outputs N               Maximum configured outputs\n"
		"  --protobuf-extra-fields VALUE preserve-opaque/reject\n"
		"  --overflow POLICY             drop-newest/fail\n"
		"  --if-uninitialized            Initialize a new store only\n"
		"validate/redact inspect saved settings without changes.\n",
		stderr);
	return 2;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		usage();
		return 0;
	}
	if (argc < 2)
		return usage();
	bool configure = !strcmp(argv[1], "config");
	bool run = !strcmp(argv[1], "run");
	bool validate = !strcmp(argv[1], "validate");
	bool redact = !strcmp(argv[1], "redact");
	if (!configure && !run && !validate && !redact)
		return usage();
	if (argc == 3 && !strcmp(argv[2], "--help")) {
		usage();
		return 0;
	}
	const char **settings = static_cast<const char **>(
		calloc((size_t)argc, sizeof(*settings)));
	if (!settings)
		return report(TG_NO_MEMORY, nullptr, 2);
	const char *selector = nullptr;
	int setting_count = 0;
	bool initialize = false, arguments_valid = true;
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--state-dir")) {
			if (selector || i + 1 == argc || !*argv[i + 1]) {
				arguments_valid = false;
				break;
			}
			selector = argv[++i];
		} else if (configure &&
			!strcmp(argv[i], "--if-uninitialized")) {
			if (initialize)
				arguments_valid = false;
			initialize = true;
		} else if (configure && i + 1 < argc) {
			settings[setting_count++] = argv[i];
			settings[setting_count++] = argv[++i];
		} else {
			arguments_valid = false;
			break;
		}
	}
	if (!arguments_valid || (configure && !initialize && !setting_count)) {
		free(settings);
		return usage();
	}
	char directory[32768];
#ifdef _WIN32
	bool cwd_ok = _getcwd(directory, sizeof(directory)) != nullptr;
#else
	bool cwd_ok = getcwd(directory, sizeof(directory)) != nullptr;
#endif
	if (!cwd_ok) {
		free(settings);
		return report(TG_IO, nullptr, 2);
	}
	tg_diagnostic diagnostic{};
	tg_buffer path{}, source{};
	tg_store *store = nullptr;
	tg_config *config = nullptr;
	tg_import_files *imports = nullptr;
	tg_web_settings web{};
	uint64_t revision = 0;
	tg_status status = tg_store_select(selector, directory, &path,
		&diagnostic);
	if (status == TG_OK)
		status = tg_store_open((const char *)path.data,
			configure || run, &store, &diagnostic);
	if (status == TG_OK)
		status = tg_store_load(store, &source, &web, &revision,
			&diagnostic);
	bool skip = status == TG_OK && configure && initialize && revision;
	if (status == TG_OK && configure && !skip) {
		status = tg_cli_compose({source.data, source.size},
			setting_count, settings, directory, true, &config,
			&imports, &web, &diagnostic);
		uint64_t saved = 0;
		if (status == TG_OK)
			status = tg_store_save_import(store, config, &web,
				revision, imports, &saved, &diagnostic);
		if (status == TG_OK)
			fprintf(stdout, "Saved configuration revision %llu\n",
				(unsigned long long)saved);
		else if (saved)
			fprintf(stderr, "Published revision %llu; durability "
				"could not be confirmed.\n",
				(unsigned long long)saved);
	} else if (status == TG_OK && !configure) {
		if (!revision) {
			fputs("No saved configuration. Initialize with: "
					"tak-gateway config --web-bind "
					"127.0.0.1:8080\n",
				stderr);
			status = TG_INVALID;
		} else {
			status = tg_config_parse_managed(
					{source.data, source.size},
					(const char *)path.data,
					&config, &diagnostic);
		}
	}
	free(settings);
	tg_buffer_release(&source);
	tg_buffer_release(&path);
	if (status == TG_OK && validate)
		status = tg_config_validate(config, &diagnostic);
	if (status == TG_OK && redact) {
		tg_buffer output{};
		status = tg_config_redacted(config, &output, &diagnostic);
		if (status == TG_OK && fwrite(output.data, 1, output.size,
			stdout) != output.size)
			status = TG_IO;
		tg_buffer_release(&output);
		if (status == TG_OK)
			fprintf(stdout,
				"\n[web]\nenabled = %s\nbind = \"%s\"\n",
				web.enabled ? "true" : "false", web.bind);
	}
	tg_web_app *app = nullptr;
	int failure_code = 2;
	if (status == TG_OK && run) {
		failure_code = 3;
		status = tg_web_app_create(store, config, &web, revision, &app,
			&diagnostic);
		if (status == TG_OK) {
			signal(SIGINT, interrupt_handler);
			signal(SIGTERM, interrupt_handler);
			while (!interrupted && status == TG_OK)
				status = tg_web_app_poll(app, 50);
			if (status == TG_OK) {
				status = tg_web_app_stop(app, 2000);
				uint64_t start = tg_monotonic_ms();
				while (status == TG_OK &&
					tg_monotonic_ms() - start < 2000)
					status = tg_web_app_poll(app, 20);
			}
		}
	}
	tg_web_app_destroy(app);
	tg_config_destroy(config);
	tg_import_zip_destroy(imports);
	tg_store_close(store);
	return status < 0 ? report(status, &diagnostic, failure_code) : 0;
}
