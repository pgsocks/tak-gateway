#ifndef TAK_GATEWAY_CLI_FIXTURES_HPP
#define TAK_GATEWAY_CLI_FIXTURES_HPP

#include "import_fixtures.hpp"
#include "../../src/cli_options.h"
#include "../../src/config_internal.h"

namespace cli_fixtures {

struct Configured {
	tg_config *config = nullptr;
	tg_import_files *files = nullptr;
	tg_diagnostic diagnostic{};

	tg_status read(const std::vector<std::string> &arguments,
		const std::string &directory)
	{
		std::vector<const char *> pointers;

		for (const auto &argument : arguments)
			pointers.push_back(argument.c_str());
		return tg_cli_configure(static_cast<int>(pointers.size()),
			pointers.data(), directory.c_str(), &config, &files,
			&diagnostic);
	}

	~Configured()
	{
		tg_config_destroy(config);
		tg_import_zip_destroy(files);
	}
};

inline std::string base_config(import_fixtures::Package &fixture)
{
	return fixture.write(fixtures::bytes(fixtures::minimal_config()),
		"gateway.toml");
}

} /* namespace cli_fixtures */

#endif
