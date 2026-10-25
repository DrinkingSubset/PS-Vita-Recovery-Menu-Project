#pragma once
/*
 * https.h — small HTTPS client (mbedTLS) used by the update checker.
 *
 * The Vita's built-in SSL library cannot complete a handshake with GitHub on
 * firmware 3.65, so this talks TLS itself: it opens a plain socket with the
 * sceNet API and runs mbedTLS on top, checking the server against the root
 * certificates bundled in ca_roots.h.
 *
 * Return values of https_get():
 *    0            success (HTTP 200, whole body delivered)
 *   -100..-599    the server answered with that HTTP status (-404 = Not Found)
 *   -2            cancelled by the data callback
 *   0xE1SSMMMM    (as a negative int) transport / TLS failure:
 *                 SS = stage (see HTTPS_STAGE_*), MMMM = low 16 bits of the
 *                 mbedTLS or SCE error code. https_detail() has readable text.
 */

#include <stdint.h>
#include <stddef.h>

enum {
    HTTPS_STAGE_URL = 1, HTTPS_STAGE_DNS, HTTPS_STAGE_CONNECT, HTTPS_STAGE_TLS,
    HTTPS_STAGE_SEND, HTTPS_STAGE_RECV, HTTPS_STAGE_PARSE, HTTPS_STAGE_CERT,
    HTTPS_STAGE_RNG
};

/* Called with each piece of the body. total = Content-Length or 0 if unknown.
 * Return non-zero to cancel the transfer. */
typedef int (*https_data_fn)(void *ctx, const uint8_t *data, size_t len,
                             uint64_t total);

int  https_get(const char *url, const char *accept,
               https_data_fn cb, void *ctx);

/* Extra text for the last error (may be ""). */
const char *https_detail(void);

/* 1 = skip certificate verification (explicit user choice only). */
void https_set_insecure(int on);

/* 1 if the last connection worked but the Vita's clock looked wrong, so the
 * certificate date check was ignored. */
int  https_clock_warning(void);
