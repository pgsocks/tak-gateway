add_library(header_contracts OBJECT header_c11.c header_cpp17.cpp)
target_link_libraries(header_contracts PRIVATE tak_gateway)

add_executable(framer_link_c11 framer_link_c11.c)
target_link_libraries(framer_link_c11 PRIVATE tak_gateway)
add_test(NAME framer_link_c11 COMMAND framer_link_c11)
set_tests_properties(framer_link_c11 PROPERTIES LABELS unit)

add_executable(unit_tests
	wire_test.cpp
	normalization_test.cpp
	session_test.cpp
	delivery_test.cpp
	config_test.cpp
	runtime_test.cpp
	import_test.cpp
	cli_test.cpp
	managed_runtime_test.cpp
	web_test.cpp
	store_test.cpp
	managed_cli_test.cpp
)
target_link_libraries(unit_tests PRIVATE
	tak_gateway GTest::gtest_main gateway_import_zip gateway_web gateway_toml
	OpenSSL::Crypto yyjson
)
target_compile_definitions(unit_tests PRIVATE
	TG_WEB_ASSET_DIRECTORY="${PROJECT_SOURCE_DIR}/src/web"
)
if(WIN32)
	target_link_libraries(unit_tests PRIVATE ws2_32)
endif()

include(GoogleTest)
gtest_discover_tests(unit_tests PROPERTIES LABELS unit)
