set(web_directory "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(web_generated "${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp")
set(web_source "#include \"web_assets.h\"\n")
foreach(asset IN ITEMS index.html app.js styles.css)
	file(READ "${web_directory}/${asset}" web_content HEX)
	string(MAKE_C_IDENTIFIER "${asset}" asset_symbol)
	string(APPEND web_source
		"static const uint8_t ${asset_symbol}[] = {\n")
	# Byte initializers preserve the source encoding and avoid MSVC's
	# string literal size limit, including its concatenated literal limit.
	string(LENGTH "${web_content}" web_length)
	set(web_offset 0)
	while(web_offset LESS web_length)
		string(SUBSTRING "${web_content}" ${web_offset} 24 web_line)
		string(REGEX REPLACE "(..)" "0x\\1, " web_line "${web_line}")
		string(APPEND web_source "\t${web_line}\n")
		math(EXPR web_offset "${web_offset} + 24")
	endwhile()
	string(APPEND web_source "\t0\n};\n")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${web_directory}/${asset}")
endforeach()
string(APPEND web_source [=[
bool tg_web_asset(const char *path, tg_bytes *bytes, const char **mime)
{
	if (!path || !bytes || !mime)
		return false;
	*bytes = {};
	*mime = nullptr;
	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		*bytes = {(const uint8_t *)index_html, sizeof(index_html) - 1};
		*mime = "text/html; charset=utf-8";
	} else if (!strcmp(path, "/app.js")) {
		*bytes = {(const uint8_t *)app_js, sizeof(app_js) - 1};
		*mime = "text/javascript; charset=utf-8";
	} else if (!strcmp(path, "/styles.css")) {
		*bytes = {(const uint8_t *)styles_css, sizeof(styles_css) - 1};
		*mime = "text/css; charset=utf-8";
	}
	return *mime != nullptr;
}
]=])
file(WRITE "${web_generated}" "${web_source}")
