/* ============================================================
 * Control compile time options.
 * ============================================================
 *
 * Compile time options have moved to config.mk.
 */


/* ============================================================
 * Compatibility defines
 *
 * Generally for Windows native support.
 * ============================================================ */


#define uthash_malloc(sz) _mosquitto_malloc(sz)
#define uthash_free(ptr,sz) _mosquitto_free(ptr)

#ifndef EPROTO
#  define EPROTO ECONNABORTED
#endif
