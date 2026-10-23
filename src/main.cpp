#include "config_internal.h"
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
	fputs("usage: tak-gateway {run|validate|redact} --config FILE\n",
		stderr);
	return 2;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		usage();
		return 0;
	}
	if (argc != 4 || strcmp(argv[2], "--config"))
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
	tg_vec path{};
	bool absolute = argv[3][0] == '/';
#ifdef _WIN32
	absolute = absolute || (strlen(argv[3]) > 2 && argv[3][1] == ':') ||
		   argv[3][0] == '\\';
#endif
	if ((!absolute &&
		    (!tg_text(&path, directory) || !tg_text(&path, "/"))) ||
		!tg_text(&path, argv[3])) {
		tg_vec_free(&path);
		return report(TG_NO_MEMORY, nullptr, 2);
	}
	tg_buffer source{};
	tg_status status = tg_read_file(
		(const char *)path.data, 8 * 1024 * 1024, false, &source);
	char *slash = strrchr((char *)path.data, '/');
#ifdef _WIN32
	char *backslash = strrchr((char *)path.data, '\\');
	if (backslash && (!slash || backslash > slash))
		slash = backslash;
#endif
	if (slash)
		slash[slash == (char *)path.data ? 1 : 0] = 0;
	tg_config *config = nullptr;
	tg_diagnostic diagnostic{};
	if (status == TG_OK)
		status = tg_config_parse({source.data, source.size},
			(const char *)path.data, &config, &diagnostic);
	tg_buffer_release(&source);
	tg_vec_free(&path);
	if (status < 0)
		return report(status, &diagnostic, 2);
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
		return status < 0 ? report(status, &diagnostic, 2) : 0;
	}

	tg_runtime *runtime = nullptr;
	status = tg_runtime_create(config, &runtime, &diagnostic);
	tg_config_destroy(config);
	if (status < 0)
		return report(status, &diagnostic, 3);
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
	return status < 0 ? report(status, nullptr, 3) : 0;
}
