/*
 * ark_bus_protocol.h - the wire frame: layout, message types, CRC and codec.
 *
 * One frame looks like this, and every multi-byte field is big-endian
 * ("network order") on the wire regardless of the host CPU:
 *
 *   offset  size  field
 *   ------  ----  -------------------------------------------
 *     0      2    magic            0xAA 0x55
 *     2      1    version          ARK_BUS_VERSION
 *     3      1    msg_type         ARK_BUS_MSG_*
 *     4      1    flags            ARK_BUS_FLAG_*
 *     5      2    topic_id         registered topic id (0 = none)
 *     7      2    seq              sender sequence number
 *     9      2    payload_len      number of payload bytes
 *    11      4    src_id           sender device id
 *    15      4    dst_id           target device id (0 = broadcast)
 *    19      N    payload
 *  19+N      2    crc16            over bytes [0, 19+N)
 *
 * The payload-length field sits at offset 9 precisely so a byte-at-a-time
 * receiver can learn the frame size as soon as the 11th byte arrives; the
 * reader below relies on this and does not need to wait for the whole header.
 */
#ifndef ARK_BUS_PROTOCOL_H
#define ARK_BUS_PROTOCOL_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"

/* Frame sync word, transmitted big-endian as bytes 0xAA, 0x55. */
#define ARK_BUS_FRAME_MAGIC     0xAA55u

/* Protocol version carried in every frame. */
#define ARK_BUS_VERSION         1u

/* Header length and CRC length, kept in sync with ark_bus_config.h. */
#define ARK_BUS_HDR_LEN         ARK_BUS_HDR_LEN_
#define ARK_BUS_CRC_LEN         ARK_BUS_CRC_LEN_

/* Special device / topic ids. */
#define ARK_BUS_ID_BROADCAST    0x00000000u
#define ARK_BUS_TOPIC_NONE      0

/* Message types. */
#define ARK_BUS_MSG_PUBLISH         0x01u  /* topic broadcast            */
#define ARK_BUS_MSG_DIRECT          0x02u  /* topic, addressed to dst_id */
#define ARK_BUS_MSG_SUBSCRIBE       0x03u  /* remote subscribe request   */
#define ARK_BUS_MSG_UNSUBSCRIBE     0x04u  /* remote unsubscribe request */
#define ARK_BUS_MSG_DISCOVER        0x10u  /* "who is out there?"        */
#define ARK_BUS_MSG_DISCOVER_REPLY  0x11u  /* answer to a DISCOVER       */
#define ARK_BUS_MSG_HEARTBEAT       0x12u  /* liveness beacon            */
#define ARK_BUS_MSG_ACK             0x20u  /* QoS1 acknowledgement       */
#define ARK_BUS_MSG_RPC_REQ         0x30u  /* request, expects an RPC_REP */
#define ARK_BUS_MSG_RPC_REP         0x31u  /* reply to an RPC_REQ        */
#define ARK_BUS_MSG_STREAM          0x40u  /* one chunk of a bulk object */
#define ARK_BUS_MSG_STREAM_ACK      0x41u  /* cumulative ack of a chunk  */

/*
 * RPC method ids.  A method id is a u16 carried at the front of the payload;
 * it deliberately does NOT get its own 1-byte frame type so the wire frame
 * stays as small as it is.  0x0000..0x7FFF is reserved for the system methods
 * below, 0x8000..0xFFFF is free for user-registered methods.
 */
#define ARK_BUS_M_PING          0x0001u  /* liveness / round-trip probe */
#define ARK_BUS_M_DESCRIBE      0x0002u  /* structured self-description */
#define ARK_BUS_M_TASK_RUN      0x0010u  /* run a named task            */
#define ARK_BUS_M_TASK_CANCEL   0x0011u  /* cancel a running task       */
#define ARK_BUS_M_USER_BASE     0x8000u  /* first user-registered id    */

/*
 * Capability / role bitmask, carried in ark_bus_start() and mirrored in the
 * device table so a scheduler can pick a worker without asking.  Zero means a
 * plain pub/sub node that advertises nothing.  Bits 16..31 are user-defined.
 */
#define ARK_CAP_MCU_EDGE        0x00000001u  /* constrained edge node    */
#define ARK_CAP_COMPUTE         0x00000002u  /* can run compute jobs     */
#define ARK_CAP_STORAGE         0x00000004u  /* can hold/relay data      */
#define ARK_CAP_LLM_BACKEND     0x00000008u  /* serves an inference core */
#define ARK_CAP_SENSOR          0x00000010u  /* samples the world        */
#define ARK_CAP_ACTUATOR        0x00000020u  /* drives the world         */
#define ARK_CAP_USER_BASE       0x00010000u  /* first user-defined bit   */

/*
 * RPC payload layout.  The request id is carried explicitly in the payload
 * rather than borrowed from the frame seq: seq is shared with QoS1 ACK
 * matching, and reusing it for both directions of a request/reply pair can
 * collide when two peers each have an outstanding frame with the same seq.
 *
 *   RPC_REQ payload:  req_id u16 | method u16 | params ...
 *   RPC_REP payload:  req_id u16 | method u16 | status i8 | result ...
 *
 * status is 0 on success or a negative ARK_BUS_E_* code; result is only
 * meaningful when status == 0.  All multi-byte fields are big-endian like
 * every other field on the wire.
 */
#define ARK_BUS_RPC_REQ_HDR     4u  /* req_id + method              */
#define ARK_BUS_RPC_REP_HDR     5u  /* req_id + method + status     */

/*
 * Data-plane payload layouts (ark_bus_stream.h).  A bulk object is cut into
 * chunks that each fit in one frame; a chunk names its own absolute offset, so
 * a duplicate is idempotent and a gap is recoverable without a window bitmap:
 *
 *   STREAM payload:      xid u16 | off u32 | total u32 | n u16 | data[n]
 *   STREAM_ACK payload:  xid u16 | next u32 | status i8
 *
 * `off` is the byte offset of this chunk inside the whole object and `total`
 * is the whole object's length, so the receiver can accept a chunk before it
 * has seen the first one.  `next` in the ACK is cumulative: every byte below it
 * has been stored.  status is 0, or a negative ARK_BUS_E_* code when the
 * receiver refused the object (E_NOMEM: bigger than its receive buffer,
 * E_FULL: no free slot).
 *
 * The design follows the block-wise transfer pattern that the IETF
 * standardised for constrained nodes (RFC 7959, "Block-Wise Transfers in
 * CoAP"): a block number/offset plus a more-bit and an explicit block size,
 * chosen because basic messages only suit small payloads while occasional
 * larger ones (firmware images) still have to cross the same narrow link.  It
 * borrows the offset-plus-cumulative-ack idea, not CoAP's REST request/response
 * framing: this layer is a one-way push between two peers.
 */
#define ARK_BUS_STREAM_HDR      12u  /* xid + off + total + n        */
#define ARK_BUS_STREAM_ACK_LEN   7u  /* xid + next + status          */

/* Usable bytes per chunk, i.e. the object step size for the default frame. */
#define ARK_BUS_STREAM_CHUNK_MAX \
    (ARK_BUS_MSG_MAX_SIZE - ARK_BUS_STREAM_HDR)

#if ARK_BUS_STREAM_ENABLED && ARK_BUS_MSG_MAX_SIZE <= ARK_BUS_STREAM_HDR
#error "ARK_BUS_MSG_MAX_SIZE leaves no room for a stream chunk header"
#endif

/* Frame flags. */
#define ARK_BUS_FLAG_QOS1       0x01u  /* sender expects an ACK           */
#define ARK_BUS_FLAG_RETRY      0x02u  /* this frame is a retransmission  */
#define ARK_BUS_FLAG_COMPRESSED 0x04u  /* payload is compressed           */

/* Parsed view of one frame.  `payload` points into the caller's buffer. */
typedef struct {
    uint8_t        version;
    uint8_t        msg_type;
    uint8_t        flags;
    uint16_t       topic_id;
    uint16_t       seq;
    uint32_t       src_id;
    uint32_t       dst_id;
    uint16_t       payload_len;
    const uint8_t *payload;
} ark_bus_frame_t;

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over `len` bytes. */
uint16_t ark_bus_crc16(const uint8_t *data, uint16_t len);

/* Total on-wire size of a frame carrying `payload_len` payload bytes. */
uint16_t ark_bus_frame_size(uint16_t payload_len);

/*
 * Encode `f` into `out`.  Returns the number of bytes written, or a negative
 * ARK_BUS_E_* code when the frame does not fit in `cap`.
 */
int ark_bus_frame_encode(const ark_bus_frame_t *f, uint8_t *out, uint16_t cap);

/*
 * Decode a complete frame in `in`.  Returns ARK_BUS_OK, or a negative
 * ARK_BUS_E_* code: E_FORMAT on a bad magic/version/oversize, E_CRC on a CRC
 * mismatch, E_INVAL on a length the header cannot justify.
 */
int ark_bus_frame_decode(const uint8_t *in, uint16_t len, ark_bus_frame_t *f);

/*
 * Byte-at-a-time frame assembler for interrupt-driven links (UART).
 *
 * It resynchronises on a bad magic byte and, crucially, drops an oversized or
 * never-completing frame instead of letting a half frame swallow the next good
 * one: any byte that arrives after the buffer is full resets the assembler.
 */
typedef struct {
    uint8_t  buf[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t len;
    uint16_t total;  /* expected total size once known, else 0 */
} ark_bus_frame_reader_t;

void ark_bus_frame_reader_init(ark_bus_frame_reader_t *r);

/*
 * Feed one byte.  Returns 1 when a complete frame now sits in r->buf (r->len
 * equals the total size), 0 when more bytes are needed, and -1 when a byte was
 * discarded while resynchronising.  After a 1 the caller consumes the frame and
 * calls ark_bus_frame_reader_reset().
 */
int ark_bus_frame_reader_push(ark_bus_frame_reader_t *r, uint8_t byte);

/* Discard any partial frame and start looking for the next magic byte. */
void ark_bus_frame_reader_reset(ark_bus_frame_reader_t *r);

#endif /* ARK_BUS_PROTOCOL_H */
