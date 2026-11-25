#ifndef TLS_H
#define TLS_H

#include <stdint.h>

/*
 * CatOS minimal TLS 1.2 HTTPS client (D2).
 *
 * Supported: TLS 1.2, single suite TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256
 * (0xC027), secp256r1 ECDHE, AES-128-CBC + HMAC-SHA256 record protection.
 *
 * SECURITY NOTE: this client performs NO certificate chain verification and
 * NO hostname verification.  The ServerKeyExchange signature is read and
 * discarded; only the ephemeral public key is consumed.  The connection is
 * encrypted only and is vulnerable to a man-in-the-middle attacker.  Trust
 * anchor / chain validation is deferred to a later stage.
 */

/*
 * Perform a full HTTPS GET: connect to peer:port, complete the TLS 1.2
 * handshake, send `GET <path> HTTP/1.0` on virtual host `host`, decrypt the
 * reply and copy the response body into `out` (capacity `max`).
 *
 * Returns body length (>= 0) on success; negative error code otherwise.
 * Error codes mirror netstack HTTP codes where applicable.
 */
int tls_https_get(const uint8_t peer[4], uint16_t port,
                  const char *host, const char *path,
                  uint8_t *out, int max, int *status_out);

#endif /* TLS_H */
