/* Strong TU-local override of _csilk_send_response for test_gzip.
 *
 * The mock MUST live in its own translation unit: test_gzip.c includes
 * csilk/core/internal.h, which pulls in crypto_dispatch.h's
 * __attribute__((weak)) declaration. Any definition in that TU inherits
 * WEAK linkage, and while GNU ld resolves the weak mock, macOS ld64 picks
 * the product implementation instead (BUS on the 1-byte mock client).
 * This TU includes only csilk/core/types.h (opaque ctx typedef, no weak
 * declarations), so the override stays STRONG and wins on every linker.
 */
#include "csilk/core/types.h"

extern int response_sent;

void
_csilk_send_response(csilk_ctx_t* c)
{
    (void)c;
    response_sent = 1;
}
