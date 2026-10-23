CPMAddPackage(NAME Tomlc99 GITHUB_REPOSITORY cktan/tomlc99
	GIT_TAG 29076dfd095bbbbd50a3c1b2760d29f4b83e74ac
	DOWNLOAD_ONLY YES)
add_library(gateway_toml STATIC "${Tomlc99_SOURCE_DIR}/toml.c")
target_include_directories(gateway_toml PUBLIC "${Tomlc99_SOURCE_DIR}")
