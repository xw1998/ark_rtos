/*
 * ark_bus_frame.c - the wire codec: CRC-16/CCITT, encode, decode, and the
 * byte-at-a-time assembler used by interrupt-driven links.
 */
#include "ark_bus_priv.h"

uint16_t ark_bus_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t  bit;

    if (!data) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t ark_bus_frame_size(uint16_t payload_len)
{
    return (uint16_t)(ARK_BUS_HDR_LEN + payload_len + ARK_BUS_CRC_LEN);
}

int ark_bus_frame_encode(const ark_bus_frame_t *f, uint8_t *out, uint16_t cap)
{
    uint16_t total;
    uint16_t crc;

    if (!f || !out) {
        return ARK_BUS_E_INVAL;
    }
    if (f->payload_len > ARK_BUS_WIRE_PAYLOAD_MAX) {
        return ARK_BUS_E_INVAL;
    }
    if (f->payload_len != 0 && f->payload == NULL) {
        return ARK_BUS_E_INVAL;
    }

    total = ark_bus_frame_size(f->payload_len);
    if (cap < total) {
        return ARK_BUS_E_NOMEM;
    }

    out[0] = 0xAAu;
    out[1] = 0x55u;
    out[2] = f->version;
    out[3] = f->msg_type;
    out[4] = f->flags;
    ark_wr16(out + 5, f->topic_id);
    ark_wr16(out + 7, f->seq);
    ark_wr16(out + 9, f->payload_len);
    ark_wr32(out + 11, f->src_id);
    ark_wr32(out + 15, f->dst_id);

    if (f->payload_len != 0) {
        ark_memcpy(out + ARK_BUS_HDR_LEN, f->payload, f->payload_len);
    }

    crc = ark_bus_crc16(out, (uint16_t)(ARK_BUS_HDR_LEN + f->payload_len));
    ark_wr16(out + ARK_BUS_HDR_LEN + f->payload_len, crc);

    return (int)total;
}

int ark_bus_frame_decode(const uint8_t *in, uint16_t len, ark_bus_frame_t *f)
{
    uint16_t plen;
    uint16_t total;
    uint16_t crc_rx;
    uint16_t crc_calc;

    if (!in || !f) {
        return ARK_BUS_E_INVAL;
    }
    if (len < (uint16_t)(ARK_BUS_HDR_LEN + ARK_BUS_CRC_LEN)) {
        return ARK_BUS_E_FORMAT;
    }
    if (in[0] != 0xAAu || in[1] != 0x55u) {
        return ARK_BUS_E_FORMAT;
    }
    if (in[2] != ARK_BUS_VERSION) {
        return ARK_BUS_E_FORMAT;
    }

    plen = ark_rd16(in + 9);
    if (plen > ARK_BUS_WIRE_PAYLOAD_MAX) {
        return ARK_BUS_E_FORMAT;
    }

    total = ark_bus_frame_size(plen);
    if (len < total) {
        return ARK_BUS_E_FORMAT;
    }

    crc_rx   = ark_rd16(in + ARK_BUS_HDR_LEN + plen);
    crc_calc = ark_bus_crc16(in, (uint16_t)(ARK_BUS_HDR_LEN + plen));
    if (crc_rx != crc_calc) {
        return ARK_BUS_E_CRC;
    }

    f->version     = in[2];
    f->msg_type    = in[3];
    f->flags       = in[4];
    f->topic_id    = ark_rd16(in + 5);
    f->seq         = ark_rd16(in + 7);
    f->payload_len = plen;
    f->src_id      = ark_rd32(in + 11);
    f->dst_id      = ark_rd32(in + 15);
    f->payload     = in + ARK_BUS_HDR_LEN;

    return ARK_BUS_OK;
}

/* ------------------------------------------------------------------ */
/* Streaming assembler                                                 */
/* ------------------------------------------------------------------ */

void ark_bus_frame_reader_init(ark_bus_frame_reader_t *r)
{
    if (!r) {
        return;
    }
    r->len = 0;
    r->total = 0;
}

void ark_bus_frame_reader_reset(ark_bus_frame_reader_t *r)
{
    if (!r) {
        return;
    }
    r->len = 0;
    r->total = 0;
}

int ark_bus_frame_reader_push(ark_bus_frame_reader_t *r, uint8_t byte)
{
    if (!r) {
        return -1;
    }

    /* Hunting for the two-byte magic. */
    if (r->len == 0) {
        if (byte != 0xAAu) {
            return -1;
        }
        r->buf[0] = byte;
        r->len = 1;
        r->total = 0;
        return 0;
    }
    if (r->len == 1 && r->total == 0) {
        if (byte == 0x55u) {
            r->buf[1] = byte;
            r->len = 2;
            return 0;
        }
        /* Not the sync word.  The current byte might itself start a new
         * candidate (0xAA 0xAA), otherwise drop everything so far. */
        if (byte == 0xAAu) {
            r->buf[0] = byte;
            r->len = 1;
        } else {
            r->len = 0;
        }
        return -1;
    }

    r->buf[r->len++] = byte;

    /* As soon as the payload-length field is in, we know the whole size. */
    if (r->total == 0 && r->len >= 11) {
        uint16_t plen = ark_rd16(r->buf + 9);
        if (plen > ARK_BUS_MSG_MAX_SIZE) {
            r->len = 0;
            r->total = 0;
            return -1;
        }
        r->total = ark_bus_frame_size(plen);
    }

    if (r->total != 0 && r->len >= r->total) {
        return 1;
    }
    if (r->len >= (uint16_t)sizeof(r->buf)) {
        r->len = 0;
        r->total = 0;
        return -1;
    }
    return 0;
}
