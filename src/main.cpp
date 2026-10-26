#include "config_internal.h"
#include "cli_options.h"
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
	fputs("usage: tak-gateway {run|validate|redact} [OPTIONS]\n"
		"  --config FILE                 Optional base TOML\n"
		"  --import-zip ZIP              Append ATAK package inputs\n"
		"  --output ID=URI               Replace or append an output\n"
		"    tcp://BIND:PORT | tcp-connect://HOST:PORT\n"
		"    udp://HOST:PORT | pipe:///ABSOLUTE/PATH\n"
		"  --set PATH=TOML_VALUE         Set any schema field\n"
		"    Example: --set 'outputs[0].framing=\"xml-document\"'\n"
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
		"Setters apply after TOML/import, in command-line order.\n"
		"Relative setting paths use the TOML directory, or cwd.\n",
		stderr);
	return 2;
}

int main(int argc, char **argv)
{
	if ((argc == 2 && !strcmp(argv[1], "--help")) ||
		(argc == 3 && !strcmp(argv[2], "--help"))) {
		usage();
		return 0;
	}
	if (argc < 2)
		return usage();
	bool run = !strcmp(argv[1], "run");
	bool validate = !strcmp(argv[1], "validate");
	bool redact = !strcmp(argv[1], "redact");
	if (!run && !validate && !redact)
		return usage();
	char directory[32768];
#ifdef _WIN32
	if (!_getcwd(directory, sizeof(directory)))
#else
	if (!getcwd(directory, sizeof(directory)))
#endif
		return report(TG_IO, nullptr, 2);
	tg_config *config = nullptr;
	tg_diagnostic diagnostic{};
	tg_import_files *import_files = nullptr;
	tg_status status = tg_cli_configure(argc - 2, argv + 2, directory,
		&config, &import_files, &diagnostic);
	if (status < 0) {
		tg_import_zip_destroy(import_files);
		return report(status, &diagnostic, 2);
	}
	if (redact) {
		tg_buffer output{};
		status = tg_config_redacted(config, &output, &diagnostic);
		if (status == TG_OK && fwrite(output.data, 1, output.size,
					       stdout) != output.size)
			status = TG_IO;
		tg_buffer_release(&output);
	} else {
		status = tg_config_validate(config, &diagnostic);
	}
	if (status < 0 || !run) {
		tg_config_destroy(config);
		tg_import_zip_destroy(import_files);
		return status < 0 ? report(status, &diagnostic, 2) : 0;
	}

	tg_runtime *runtime = nullptr;
	status = tg_runtime_create(config, &runtime, &diagnostic);
	tg_config_destroy(config);
	if (status < 0) {
		tg_import_zip_destroy(import_files);
		return report(status, &diagnostic, 3);
	}
	signal(SIGINT, interrupt_handler);
	signal(SIGTERM, interrupt_handler);
	while (!interrupted && status == TG_OK)
		status = tg_runtime_poll(runtime, 100);
	if (status == TG_OK) {
		status = tg_runtime_stop(runtime, 2000);
		uint64_t start = tg_monotonic_ms();
		while (status == TG_OK && tg_monotonic_ms() - start < 2000) {
			status = tg_runtime_poll(runtime, 20);
			tg_buffer snapshot{};
			if (tg_runtime_snapshot(runtime, &snapshot) == TG_OK &&
				strstr((const char *)snapshot.data,
					"\"state\":\"STOPPED\"")) {
				tg_buffer_release(&snapshot);
				break;
			}
			tg_buffer_release(&snapshot);
		}
	}
	tg_runtime_destroy(runtime);
	tg_import_zip_destroy(import_files);
	return status < 0 ? report(status, nullptr, 3) : 0;
}
