# The upstream cucumber-cpp library supplies the native wire-server main.
add_executable(acceptance_steps steps.cpp runtime_steps.cpp)
target_link_libraries(acceptance_steps PRIVATE
	tak_gateway cucumber-cpp GTest::gtest
)
if(WIN32)
	target_link_libraries(acceptance_steps PRIVATE ws2_32)
endif()

add_test(NAME acceptance
	COMMAND "${RUBY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.rb"
		$<TARGET_FILE:acceptance_steps>
)
set_tests_properties(acceptance PROPERTIES
	LABELS acceptance
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
