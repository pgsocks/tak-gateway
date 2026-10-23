option(TG_USE_SYSTEM_OPENSSL "Use an installed OpenSSL 3 for local builds" OFF)
if(TG_USE_SYSTEM_OPENSSL)
	find_package(OpenSSL 3 REQUIRED GLOBAL)
else()
	CPMAddPackage(NAME GatewayOpenSSL GITHUB_REPOSITORY openssl/openssl
		GIT_TAG openssl-3.5.4 DOWNLOAD_ONLY YES)
	include(ExternalProject)
	find_package(Threads REQUIRED)
	find_package(Perl REQUIRED)
	set(ssl_prefix "${CMAKE_CURRENT_BINARY_DIR}/install")
	file(MAKE_DIRECTORY "${ssl_prefix}/include")
	if(WIN32)
		set(ssl_make nmake)
		# Ruby/MSYS can export MAKE=make; OpenSSL must recurse with NMake.
		set(ssl_make_options MAKE=nmake)
		set(ssl_config VC-WIN64A
			"CC=${CMAKE_C_COMPILER}" "LD=${CMAKE_LINKER}"
			"AR=${CMAKE_AR}" "MT=${CMAKE_MT}")
		find_program(ssl_nasm NAMES nasm)
		if(NOT ssl_nasm)
			list(APPEND ssl_config no-asm)
		endif()
		set(ssl_crypto libcrypto.lib)
		set(ssl_ssl libssl.lib)
	else()
		find_program(ssl_make NAMES gmake make REQUIRED)
		set(ssl_config)
		set(ssl_crypto libcrypto.a)
		set(ssl_ssl libssl.a)
	endif()
	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		list(APPEND ssl_config --debug)
	endif()
	ExternalProject_Add(gateway_openssl_build
		SOURCE_DIR "${GatewayOpenSSL_SOURCE_DIR}"
		DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
		CONFIGURE_COMMAND "${PERL_EXECUTABLE}"
			"${GatewayOpenSSL_SOURCE_DIR}/Configure" ${ssl_config}
			no-shared no-tests no-module "--prefix=${ssl_prefix}"
			--libdir=lib
		BUILD_COMMAND "${ssl_make}" ${ssl_make_options}
		INSTALL_COMMAND "${ssl_make}" ${ssl_make_options} install_sw
		BUILD_BYPRODUCTS "${ssl_prefix}/lib/${ssl_ssl}"
			"${ssl_prefix}/lib/${ssl_crypto}")
	add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
	set_target_properties(OpenSSL::Crypto PROPERTIES
		IMPORTED_LOCATION "${ssl_prefix}/lib/${ssl_crypto}"
		INTERFACE_INCLUDE_DIRECTORIES "${ssl_prefix}/include"
		INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
	add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
	set_target_properties(OpenSSL::SSL PROPERTIES
		IMPORTED_LOCATION "${ssl_prefix}/lib/${ssl_ssl}"
		INTERFACE_INCLUDE_DIRECTORIES "${ssl_prefix}/include"
		INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto;${CMAKE_DL_LIBS}")
	if(WIN32)
		set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
			INTERFACE_LINK_LIBRARIES crypt32 ws2_32 advapi32 user32)
	endif()
	add_dependencies(OpenSSL::Crypto gateway_openssl_build)
	add_dependencies(OpenSSL::SSL gateway_openssl_build)
endif()
