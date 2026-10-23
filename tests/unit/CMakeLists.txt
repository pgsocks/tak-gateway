add_executable(unit_tests main.cpp)
target_link_libraries(unit_tests PRIVATE GTest::gtest)

include(register.cmake)
