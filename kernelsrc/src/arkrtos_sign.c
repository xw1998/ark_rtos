/**
* @file arkrtos_sign.c
* @brief App image signature: SHA-256 + HMAC + the image MAC (kernel side).
* @details See arkrtos_sign.h for what this covers and what it does not.  The
*          HMAC is RFC 2104 over the shared SHA-256 in arkrtos_sha256.c.  Both
*          are compiled only when ARKRTOS_USE_IMAGE_SIGN is on, so a build that
*          does not want signature checking carries none of this code.
* @author xw
*/
#include "arkrtos_sign.h"
#include "arkrtos_config.h"    /* feature gate: ARKRTOS_USE_IMAGE_SIGN + key */

#if (ARKRTOS_USE_IMAGE_SIGN == 1)

/* --------------------------------------------------------------- SHA-256 */

/* The primitive is in arkrtos_sha256.h (header-only, static) so the console
 * login can hash without turning image signing on.  It stays in *this*
 * translation unit on purpose: as a separate .c it cost 32 bytes of text here,
 * because gcc could no longer inline it into the HMAC wrappers below, and the
 * default image stopped matching the pre-change build byte for byte. */

#include "arkrtos_sha256.h"

/* ------------------------------------------------------------------- HMAC */

typedef struct
{
    arkrtos_sha256_t inner;
    uint8          opad[64];
} arkrtos_hmac_t;

static void arkrtos_hmac_init(arkrtos_hmac_t *h, const uint8 *key, uint32 klen)
{
    uint8 kpad[64];
    uint8 i;

    for(i = 0u; i < 64u; i++) { kpad[i] = 0u; }
    for(i = 0u; (i < klen) && (i < 64u); i++) { kpad[i] = key[i]; }

    arkrtos_sha256_init(&h->inner);
    for(i = 0u; i < 64u; i++)
    {
        h->opad[i] = (uint8)(kpad[i] ^ 0x5cu);
        kpad[i]   = (uint8)(kpad[i] ^ 0x36u);
    }
    arkrtos_sha256_update(&h->inner, kpad, 64u);
}

static void arkrtos_hmac_update(arkrtos_hmac_t *h, const uint8 *data, uint32 len)
{
    arkrtos_sha256_update(&h->inner, data, len);
}

static void arkrtos_hmac_final(arkrtos_hmac_t *h, uint8 out[32])
{
    uint8 inner[32];
    arkrtos_sha256_t outer;

    arkrtos_sha256_final(&h->inner, inner);
    arkrtos_sha256_init(&outer);
    arkrtos_sha256_update(&outer, h->opad, 64u);
    arkrtos_sha256_update(&outer, inner, 32u);
    arkrtos_sha256_final(&outer, out);
}

/* -------------------------------------------------------------- image MAC */

/* The key is a compile time list of 32 bytes; see features.h / the board
 * config.  A build with the switch on but no key is refused at compile time
 * there, so reaching this initialiser means a key exists. */
static const uint8 arkrtos_sign_key[ARKRTOS_SIGN_KEY_LEN] = ARKRTOS_IMAGE_SIGN_KEY;

void arkrtos_sign_image(const uint8 *image, const arkrtos_app_header_t *p_hdr,
                      uint8 out[ARKRTOS_SIGN_LEN])
{
    static const uint8 zero4[4]  = { 0u, 0u, 0u, 0u };
    static const uint8 zero64[64] = { 0u };
    arkrtos_hmac_t h;
    uint32 rt_len = p_hdr->reloc_count * ARKRTOS_APP_RELOC_SIZE;

    arkrtos_hmac_init(&h, arkrtos_sign_key, ARKRTOS_SIGN_KEY_LEN);

    /* header, with the four fields that are either self-referential or set
     * after packing replaced by zeros: crc32 (28), signature (32..96),
     * state (100) and runtime_ram_base (252).  Same rule the CRC uses, plus
     * the signature field itself - a signature cannot cover its own value. */
    arkrtos_hmac_update(&h, image, ARKRTOS_APP_OFF_CRC32);
    arkrtos_hmac_update(&h, zero4, 4u);
    arkrtos_hmac_update(&h, zero64, 64u);
    arkrtos_hmac_update(&h, image + 96u, 4u);            /* flags */
    arkrtos_hmac_update(&h, zero4, 4u);
    arkrtos_hmac_update(&h, image + 104u, 148u);         /* 104..252 */
    arkrtos_hmac_update(&h, zero4, 4u);
    /* reloc table + payload */
    arkrtos_hmac_update(&h, image + ARKRTOS_APP_HEADER_SIZE, rt_len + p_hdr->image_size);

    arkrtos_hmac_final(&h, out);
}

/* Streaming variant: the streaming installer holds no nominal copy of the
 * image (it patches each chunk as it lands), so the MAC has to be built up
 * from the bytes as they arrive.  Header handling is identical to
 * arkrtos_sign_image(); only reloc table + payload come in later. */
static arkrtos_hmac_t arkrtos_sign_stream_ctx;

void arkrtos_sign_stream_begin(const uint8 *image)
{
    static const uint8 zero4[4]  = { 0u, 0u, 0u, 0u };
    static const uint8 zero64[64] = { 0u };

    arkrtos_hmac_init(&arkrtos_sign_stream_ctx, arkrtos_sign_key, ARKRTOS_SIGN_KEY_LEN);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, image, ARKRTOS_APP_OFF_CRC32);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, zero4, 4u);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, zero64, 64u);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, image + 96u, 4u);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, zero4, 4u);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, image + 104u, 148u);
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, zero4, 4u);
}

void arkrtos_sign_stream_update(const uint8 *data, uint32 len)
{
    arkrtos_hmac_update(&arkrtos_sign_stream_ctx, data, len);
}

int32 arkrtos_sign_stream_end(const arkrtos_app_header_t *p_hdr)
{
    uint8  mac[ARKRTOS_SIGN_LEN];
    uint32 i;
    uint8  diff = 0u;

    arkrtos_hmac_final(&arkrtos_sign_stream_ctx, mac);
    for(i = 0u; i < ARKRTOS_SIGN_LEN; i++)
    {
        diff |= (uint8)(mac[i] ^ p_hdr->signature[i]);
    }
    return (diff == 0u) ? 0 : -1;
}

int32 arkrtos_sign_verify(const uint8 *image, const arkrtos_app_header_t *p_hdr)
{
    uint8  mac[ARKRTOS_SIGN_LEN];
    uint32 i;
    uint8  diff = 0u;

    arkrtos_sign_image(image, p_hdr, mac);
    for(i = 0u; i < ARKRTOS_SIGN_LEN; i++)
    {
        diff |= (uint8)(mac[i] ^ p_hdr->signature[i]);
    }
    return (diff == 0u) ? 0 : -1;
}

/* ------------------------------------------------------------ payload MAC */

#if (ARKRTOS_USE_EXEC_SIGN == 1)

static arkrtos_hmac_t arkrtos_sign_payload_ctx;
static uint8          arkrtos_sign_payload_open;

void arkrtos_sign_payload(const uint8 *data, uint32 len,
                        uint8 out[ARKRTOS_SIGN_LEN])
{
    arkrtos_hmac_t h;

    arkrtos_hmac_init(&h, arkrtos_sign_key, ARKRTOS_SIGN_KEY_LEN);
    if(len != 0u)
    {
        arkrtos_hmac_update(&h, data, len);
    }
    arkrtos_hmac_final(&h, out);
}

int32 arkrtos_sign_payload_check(const uint8 *data, uint32 len,
                               const uint8 tag[ARKRTOS_SIGN_LEN])
{
    uint8  mac[ARKRTOS_SIGN_LEN];
    uint8  diff = 0u;
    uint32 i;

    arkrtos_sign_payload(data, len, mac);
    for(i = 0u; i < ARKRTOS_SIGN_LEN; i++)
    {
        diff |= (uint8)(mac[i] ^ tag[i]);
    }
    return (diff == 0u) ? 0 : -1;
}

void arkrtos_sign_payload_begin(void)
{
    arkrtos_hmac_init(&arkrtos_sign_payload_ctx, arkrtos_sign_key,
                      ARKRTOS_SIGN_KEY_LEN);
    arkrtos_sign_payload_open = 1u;
}

void arkrtos_sign_payload_update(const uint8 *data, uint32 len)
{
    /* 没 begin 就 update 属于调用方的错，但这里静默吞掉比「算出一个
     * 只覆盖后半段、却照样能对上的 MAC」安全：后者会是一条假绿。 */
    if((arkrtos_sign_payload_open == 0u) || (len == 0u))
    {
        return;
    }
    arkrtos_hmac_update(&arkrtos_sign_payload_ctx, data, len);
}

int32 arkrtos_sign_payload_end(const uint8 tag[ARKRTOS_SIGN_LEN])
{
    uint8  mac[ARKRTOS_SIGN_LEN];
    uint8  diff = 0u;
    uint32 i;

    if(arkrtos_sign_payload_open == 0u)
    {
        return -1;
    }
    arkrtos_sign_payload_open = 0u;

    arkrtos_hmac_final(&arkrtos_sign_payload_ctx, mac);
    for(i = 0u; i < ARKRTOS_SIGN_LEN; i++)
    {
        diff |= (uint8)(mac[i] ^ tag[i]);
    }
    return (diff == 0u) ? 0 : -1;
}

#endif /* ARKRTOS_USE_EXEC_SIGN */

#endif /* ARKRTOS_USE_IMAGE_SIGN */
