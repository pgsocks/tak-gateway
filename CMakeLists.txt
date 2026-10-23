cmake_minimum_required(VERSION 3.24)
project(tak_gateway LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
option(BUILD_UNIT_TESTS "Build GoogleTest unit tests" ON)
option(BUILD_ACCEPTANCE_TESTS "Build Cucumber acceptance tests" OFF)

include(cmake/CPM.cmake)
add_subdirectory(cmake/dependencies/gateway-codec)
add_subdirectory(cmake/dependencies/tomlc99)
add_subdirectory(cmake/dependencies/openssl)
add_subdirectory(cmake/dependencies/cares)

add_subdirectory(src)

if(BUILD_TESTING AND (BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS))
	if(BUILD_ACCEPTANCE_TESTS)
		include(cmake/AcceptancePrerequisites.cmake)
	endif()
	add_subdirectory(cmake/dependencies EXCLUDE_FROM_ALL)
	add_subdirectory(tests)
endif()
