#include <tak_gateway/gateway.hpp>
#include <tak_gateway/gateway.hpp>
#include <type_traits>

static_assert(std::is_same<tak_gateway::tg_buffer, tg_buffer>::value,
	"the C++ view must share C ownership and layout");
static_assert(std::is_same<decltype(&tak_gateway::tg_session_create),
	decltype(&tg_session_create)>::value,
	"the C++ view must expose the same session contract");
static_assert(std::is_standard_layout<tg_limits>::value,
	"limits must remain a plain data structure");
static_assert(std::is_trivially_copyable<tg_diagnostic>::value,
	"diagnostics must not own resources");
