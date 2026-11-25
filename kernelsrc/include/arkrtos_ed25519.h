/**
* @file arkrtos_ed25519.h
* @brief Ed25519 signature verification (RFC 8032), verify-only.
*        Optional: ARKRTOS_USE_ED25519.
* @details The image signature used to be one symmetric HMAC key: a device
*          that can check an image can also *make* one, because it holds the
*          same secret.  This module is the other half of the answer - the
*          device holds a 32-byte public key and can only check.
*
*          Verify-only is a deliberate simplification, not a missing feature:
*          no secret ever enters this file, so there is nothing to leak and
*          no constant-time requirement on the arithmetic.  Only the decision
*          has to be exact.
*
*          Three rejections RFC 8032 requires and a naive port tends to skip:
*            - the y coordinate of A and of R must be canonical (< p),
*            - s must be canonical (< L)  -- otherwise "s + L" is a second
*              signature for the same message,
*            - x == 0 with the sign bit set is not a point encoding.
*          Verification is cofactorless (the strict reading): [s]B is compared
*          with R + [h]A on encodings, so a signature that only verifies up to
*          the cofactor 8 is refused.
*
*          Honest limits:
*            - This proves "the holder of the private key produced this
*              message".  It says nothing about *which* key should be trusted:
*              that is the trust-root question (SEC-G12), i.e. how the public
*              key itself gets onto the device and stays there.
*            - A public key compiled in is only as good as the image build
*              chain; rotating it needs a firmware update.
*            - No side-channel protection is claimed, and none is needed for
*              verification; a future signing path would need its own review.
*
* @author xw
*/
#ifndef __ARKRTOS_ED25519_H__
#define __ARKRTOS_ED25519_H__

#include "arkrtos_types.h"

/* Feature switch.  Until the Kconfig entry exists this is the only place a
 * build can turn it on, and the default keeps the image byte-identical. */
#ifndef ARKRTOS_USE_ED25519
#define ARKRTOS_USE_ED25519                 0
#endif

#define ARKRTOS_ED25519_PUB_LEN             32
#define ARKRTOS_ED25519_SIG_LEN             64

#if (ARKRTOS_USE_ED25519 == 1)

/**
* @brief Verify an Ed25519 signature over a byte string.
* @param sig  64-byte signature, R (32) followed by s (32)
* @param msg  signed bytes; may be NULL only when len is 0
* @param len  message length in bytes
* @param pub  32-byte public key, the RFC 8032 encoding
* @return 0 when the signature is valid, -1 otherwise
*/
int32 arkrtos_ed25519_verify(const uint8 sig[ARKRTOS_ED25519_SIG_LEN],
                             const uint8 *msg,
                             uint32 len,
                             const uint8 pub[ARKRTOS_ED25519_PUB_LEN]);

#endif /* ARKRTOS_USE_ED25519 == 1 */

#endif /* __ARKRTOS_ED25519_H__ */
