add_library(tak_gateway STATIC
	common.cpp wire.cpp normalize.cpp delivery.cpp session.cpp
	config.cpp runtime.cpp
)
target_include_directories(tak_gateway PUBLIC
	"${PROJECT_SOURCE_DIR}/include"
)
find_package(Threads REQUIRED)
# Cot's Expat calls back into Cot. Rescan both static archives when the
# linker requires it, including for consumers that only use XML framing.
if(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED OR
	CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED)
	target_link_libraries(tak_gateway PRIVATE
		"$<LINK_GROUP:RESCAN,cot::cot,expat>")
else()
	target_link_libraries(tak_gateway PRIVATE cot::cot expat)
endif()
target_link_libraries(tak_gateway PRIVATE
	gateway_toml OpenSSL::SSL OpenSSL::Crypto
	c-ares::cares Threads::Threads
)
if(WIN32)
	target_link_libraries(tak_gateway PRIVATE ws2_32 advapi32 iphlpapi)
endif()

add_executable(tak-gateway main.cpp)
target_link_libraries(tak-gateway PRIVATE tak_gateway gateway_toml)
install(TARGETS tak-gateway RUNTIME DESTINATION bin)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/" DESTINATION include)
