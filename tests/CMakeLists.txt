add_subdirectory(unit)

if(BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(acceptance)
endif()
