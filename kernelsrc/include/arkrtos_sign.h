/**
* @file arkrtos_sign.h
* @brief App image signature check (HMAC-SHA256).  Optional: ARKRTOS_USE_IMAGE_SIGN.
* @details The image header has always carried a `signature[64]` placeholder that
*          nothing read.  This module finally fills it in: the packer computes an
*          HMAC-SHA256 over the image and stores it in the first 32 bytes of that
*          field; the kernel recomputes it while installing and refuses an image
*          whose value does not match.
*
*          Scope and honest limits:
*            - It is a **symmetric** scheme with one pre-shared key
*              (ARKRTOS_IMAGE_SIGN_KEY).  It answers "was this image produced by
*              somebody who holds the key, and is it unchanged?".  It is NOT a
*              trust chain: whoever can read the key out of a device can sign a
*              new image.  A public-key scheme (device holds only a public key)
*              is the real fix and is not implemented here.
*            - There are two backends, chosen at build time:
*                ARKRTOS_USE_IMAGE_SIGN alone              -> HMAC-SHA256 (symmetric)
*                ARKRTOS_USE_IMAGE_SIGN_ED25519 + ..._ED25519 -> Ed25519 (public key)
*              In the Ed25519 build the device holds only a 32-byte public key
*              (ARKRTOS_IMAGE_SIGN_PUBKEY) and the 64-byte signature in
*              signature[64] is checked over a SHA-256 digest of the same
*              covered bytes.  The device can then no longer sign an image,
*              which is the whole point: reading a device used to yield the
*              power to mint new firmware.  What that does *not* solve is where
*              the public key itself comes from and how it is kept - that is the
*              trust-root question (SEC-G12), not this module.
*            - It is checked at install time only.  Image already in Flash are
*              trusted on the next boot; the point of the check is to keep a bad
*              image out, not to re-audit the pool.
*            - The signature covers the same bytes the CRC does, plus the
*              signature field itself zeroed: header fields that change after
*              install (crc32, state, runtime_ram_base) are excluded, so an
*              installed image keeps a valid signature after being moved.
*
* @author xw
*/
#ifndef __ARKRTOS_SIGN_H__
#define __ARKRTOS_SIGN_H__

#include "arkrtos_types.h"
#include "arkrtos_app_image.h"

/* Fallback only: arkrtos_features.h is the authoritative place this switch is
 * declared.  This keeps the header usable on its own (e.g. from the packer's
 * generated checks) without a second definition when features.h did the job. */
#ifndef ARKRTOS_USE_IMAGE_SIGN
#define ARKRTOS_USE_IMAGE_SIGN      0
#endif

/* Same fallback for the exec/ld payload gate; features.h is authoritative. */
#ifndef ARKRTOS_USE_EXEC_SIGN
#define ARKRTOS_USE_EXEC_SIGN       0
#endif

/* Fallbacks for the asymmetric backend; features.h is authoritative and is
 * also where the "both switches + a public key" checks live. */
#ifndef ARKRTOS_USE_IMAGE_SIGN_ED25519
#define ARKRTOS_USE_IMAGE_SIGN_ED25519  0
#endif
#ifndef ARKRTOS_USE_ED25519
#define ARKRTOS_USE_ED25519             0
#endif

/** @brief Signature length in bytes (HMAC-SHA256 output).
 *  @note Stored in signature[0..32); signature[32..64) stays zero. */
#define ARKRTOS_SIGN_LEN            (32u)

/** @brief Pre-shared key length in bytes. */
#define ARKRTOS_SIGN_KEY_LEN        (32u)

#if (ARKRTOS_USE_IMAGE_SIGN == 1)

#if (ARKRTOS_USE_IMAGE_SIGN_ED25519 == 0)

/**
* @brief Compute an image's signature into out[ARKRTOS_SIGN_LEN].
* @param image  start of the image (header + reloc table + payload)
* @param p_hdr  its parsed header
* @param out    receives ARKRTOS_SIGN_LEN bytes
* @note Only exists in the HMAC build.  The Ed25519 build has no private key on
*       the device, so it genuinely cannot compute a signature - the symbol is
*       absent rather than present-and-wrong.
*/
void arkrtos_sign_image(const uint8 *image, const arkrtos_app_header_t *p_hdr,
                      uint8 out[ARKRTOS_SIGN_LEN]);

#else /* ARKRTOS_USE_IMAGE_SIGN_ED25519 == 1 */

/**
* @brief SHA-256 digest of exactly the bytes the signature covers.
* @param image  start of the image (header + reloc table + payload)
* @param p_hdr  its parsed header
* @param out    receives 32 bytes
* @note The Ed25519 signature in p_hdr->signature[0..64) is over this digest,
*       so packing-side and device-side agree iff both hash the same fields.
*       Exposed because the device holds no private key: reading the digest
*       reveals nothing that reading the image does not.
*/
void arkrtos_sign_digest(const uint8 *image, const arkrtos_app_header_t *p_hdr,
                         uint8 out[32]);

#endif /* !ARKRTOS_USE_IMAGE_SIGN_ED25519 */

/**
* @brief Check the signature stored in p_hdr->signature against the image.
* @return 0 = matches, -1 = does not.
* @note HMAC build: 32-byte MAC compared in constant time.
*       Ed25519 build: 64-byte signature over a SHA-256 digest of the same
*       covered bytes; the comparison is the signature check itself, so an
*       early exit leaks nothing beyond "invalid".
*/
int32 arkrtos_sign_verify(const uint8 *image, const arkrtos_app_header_t *p_hdr);

/** Streaming MAC, for the install path that never holds the whole nominal
 *  image in RAM.  Feed the header (as written to Flash), then the nominal
 *  reloc table and payload bytes exactly as they arrive over the wire, then
 *  compare against p_hdr->signature.  Same coverage as arkrtos_sign_image();
 *  one such transfer is in flight at a time, so the state is module-static. */
void  arkrtos_sign_stream_begin(const uint8 *image);
void  arkrtos_sign_stream_update(const uint8 *data, uint32 len);
int32 arkrtos_sign_stream_end(const arkrtos_app_header_t *p_hdr);

/* ---- payload tag：给「整份文件」用的尾部签名（exec / ld 那一档） ----------
 * 与上面那套镜像签名不同，这里没有头部参与：tag 覆盖文件前 len-32 字节的
 * 全部内容，32 字节追加在文件末尾。ELF 与 ET_REL 都按偏移/节表读，尾部多
 * 出来的字节天然不参与装载，所以打包侧只要「签名后再交给 blob 生成」即可，
 * 不改任何既有格式。开关 ARKRTOS_USE_EXEC_SIGN（隐含 IMAGE_SIGN）；默认关
 * 时本段不参与编译，下面五个符号一个字节都不出现。 */

#if (ARKRTOS_USE_EXEC_SIGN == 1)

/** @brief 一次算出 data[0..len) 的 tag；out 收 ARKRTOS_SIGN_LEN 字节 */
void arkrtos_sign_payload(const uint8 *data, uint32 len,
                        uint8 out[ARKRTOS_SIGN_LEN]);

/** @brief 比较 data[0..len) 的 MAC 与 tag（常量时间）
 *  @return 0 = 一致，-1 = 不符 */
int32 arkrtos_sign_payload_check(const uint8 *data, uint32 len,
                               const uint8 tag[ARKRTOS_SIGN_LEN]);

/** @brief 流式三件套（begin / update / end）：装载器不把整份文件读进 RAM 时用
 *  @note 一次只能有一路在算（状态是模块静态的），装载路径本身是串行的。 */
void  arkrtos_sign_payload_begin(void);

/** @brief 喂一块数据给流式 MAC；没 begin 过就调用会被静默丢弃 */
void  arkrtos_sign_payload_update(const uint8 *data, uint32 len);
/** @return 0 = 一致，-1 = 不符（或没有 begin 过） */
int32 arkrtos_sign_payload_end(const uint8 tag[ARKRTOS_SIGN_LEN]);

#endif /* ARKRTOS_USE_EXEC_SIGN */

#endif /* ARKRTOS_USE_IMAGE_SIGN */

#endif /* __ARKRTOS_SIGN_H__ */
