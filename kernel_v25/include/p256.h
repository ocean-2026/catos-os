#ifndef P256_H
#define P256_H

#include <stdint.h>

/* secp256r1 (NIST P-256) ECDHE for the CatOS HTTPS client (D2).
 *
 * Ported from the micro-ecc generic-C reference (BSD 2-clause,
 * Copyright 2014 Kenneth MacKay), trimmed to the p256 curve only.
 *
 * Wire format: public keys are the 65-byte uncompressed point
 * 0x04 || X(32) || Y(32), big-endian, as used by TLS ServerKeyExchange /
 * ClientKeyExchange.  Scalars are 32-byte big-endian.
 */

/* Compute pub = priv * G.  Returns 0 on invalid scalar, 1 on success. */
int p256_pubgen(const uint8_t priv[32], uint8_t pub[65]);

/* Compute shared = priv * peer_pub (X coordinate, 32 bytes BE).
 * Returns 0 on invalid peer point/scalar, 1 on success. */
int p256_shared(const uint8_t priv[32], const uint8_t pub[65],
                uint8_t shared[32]);

#endif
