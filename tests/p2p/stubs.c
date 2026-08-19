/* Minimal stand-ins so ikcp.c links in a host test: the congestion control
 * under test touches none of these paths.
 *
 * Pacing used to be stubbed out here too. It is linked for real now - it takes
 * part in every flush, so a test that stubbed it would be measuring a send path
 * the device does not run. */
#include <stddef.h>
void tuya_mbuf_free(void *m) { (void)m; }
