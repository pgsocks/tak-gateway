#include <tak_gateway/gateway.h>
#include <tak_gateway/gateway.h>

_Static_assert(sizeof(uint32_t) == 4, "u32be requires four-byte uint32_t");
_Static_assert(TG_UNIMPLEMENTED < 0, "skeleton status is a failure");

/* Compile the C-facing types and declarations in an actual C11 unit. */
int tg_header_c11_contract(void)
{
	tg_limits limits = TG_LIMITS_DEFAULT;
	tg_message message = {TG_ENCODING_NONE, {NULL, 0}};
	tg_status (*create)(tg_encoding, const tg_limits *, tg_framer **) =
		tg_framer_create;
	tg_status (*publish)(tg_delivery *, tg_bytes, tg_publish_report *) =
		tg_delivery_publish;

	(void)limits;
	(void)message;
	(void)create;
	(void)publish;
	return 0;
}
