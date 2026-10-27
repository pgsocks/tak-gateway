add_subdirectory(googletest)

if(BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(nlohmann-json)
	add_subdirectory(asio)
	add_subdirectory(tclap)
	add_subdirectory(cucumber-cpp)
endif()
