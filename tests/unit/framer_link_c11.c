#include <tak_gateway/gateway.h>

/* Link framing without referencing any codec functions directly. */
int main(void)
{
	tg_framer *framer = NULL;
	tg_status status = tg_framer_create(TG_ENCODING_XML, NULL, &framer);

	tg_framer_destroy(framer);
	return status == TG_OK ? 0 : 1;
}
