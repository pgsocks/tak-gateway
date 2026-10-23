set(CMAKE_CXX_STANDARD 17)

if(BUILD_UNIT_TESTS)
	add_subdirectory(unit)
endif()

if(BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(acceptance)
	add_subdirectory(infrastructure)
endif()
