#ifndef ESPLAB_PROTOCOL_H
#define ESPLAB_PROTOCOL_H
/* ESPlab wire protocol — shared by every node, every transport.
 * Little-endian on the wire. Fixed-width types so a classic ESP32 and a
 * Raspberry Pi running the same core agree on every byte.
 *
 * NOTE (encryption): ESP-NOW link encryption (shared PMK + per-peer LTK) is
 * applied by the transport on hardware. This layer frames and serializes;
 * the bus layer owns the key material and the "drop strangers" policy. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ELP_VERSION 1

/* ---- message types ---- */
typedef enum {
    ELP_HELLO = 1,       /* "I exist" — first contact on join/startup */
    ELP_ANNOUNCE = 2,    /* periodic from coordinator: I am the coordinator */
    ELP_ELECTION = 3,    /* candidate score during coordinator election */
    ELP_COORD = 4,       /* "I won, I am coordinator" */
    ELP_SUBMIT = 5,      /* submit a job (console/net front-end -> coord) */
    ELP_CHUNK_REQ = 6,   /* worker -> coordinator: give me the next chunk */
    ELP_CHUNK = 7,       /* coordinator -> worker: run this chunk */
    ELP_RESULT = 8,      /* worker -> coordinator: chunk result */
    ELP_RESULT_ACK = 9,  /* coordinator -> worker: result recorded */
    ELP_CHUNK_LOST = 10, /* coordinator -> any: chunk lost, will reassign */
    ELP_KV_PUT = 11,     /* coordinator -> owner: store this record */
    ELP_KV_GET = 12,     /* reader -> owner: read this record */
    ELP_KV_VAL = 13,     /* owner -> reader: record value (0 = not found) */
    ELP_KV_DEL = 14,
    ELP_KV_ACK = 15,
    ELP_UPDATE = 16,     /* supervisor -> node: change this setting */
    ELP_UPDATE_ACK = 17,
    ELP_BYE = 18,        /* graceful leave */
} elp_type_t;

/* ---- node descriptor (capability card) ---- */
typedef struct {
    uint8_t  id[6];
    uint8_t  chipspec;
    uint8_t  ncores;
    uint8_t  ram_kb_x16;    /* usable RAM: KB / 16 */
    uint8_t  ram_kb_rem;    /* remainder: KB % 16 */
    uint8_t  flash_mb_x16;  /* flash: MB / 16 */
    uint8_t  flash_mb_rem;  /* remainder: MB % 16 */
    uint32_t capacities;    /* native capability bitmask */
    int32_t  score;         /* election score: higher wins */
    uint64_t boot_id;       /* random per boot; breaks score ties */
    uint64_t last_seen_ms;  /* transport-local clock: last heard */
    uint32_t roster_seq;    /* last roster seq this node has applied */
    bool     is_self;
} elp_node_t;

/* ELP_CHIP_* constants (kept as defines: C89-friendly) */
#define ELP_CHIP_CLASSIC 1
#define ELP_CHIP_S2      2
#define ELP_CHIP_S3      3
#define ELP_CHIP_C3      4
#define ELP_CHIP_HOST    5  /* a virtual board: linux process */

/* capability bits */
#define ELP_CAP_NATIVE_HASH (1u << 0)
#define ELP_CAP_NATIVE_AES  (1u << 1)
#define ELP_CAP_VM          (1u << 2)
#define ELP_CAP_STORE       (1u << 3)

/* ---- limits and constants ---- */
#define JOB_OUT_MAX 200

/* ---- payload structs ---- */
typedef struct {
    uint8_t  winner[6];
    int32_t  score;
    uint64_t boot_id;
} elp_coord_t;

typedef struct {
    uint8_t  from[6];
    uint64_t job_id;
    uint32_t chunk;
    uint32_t rtt_hint_ms;
    uint32_t payload_len;
} elp_chunk_t;

typedef struct {
    uint8_t  from[6];
    uint64_t job_id;
    uint32_t chunk;
    uint8_t  status;          /* 0=ok 1=overflow 2=VM fault 3=host fault */
    uint16_t out_len;
    uint8_t  out[JOB_OUT_MAX];
} elp_result_t;

/* ---- more limits ---- */
/* ESP-NOW radio frames carry 250 bytes max; 26 bytes of link header leaves
 * 224 bytes of message body per packet. Bigger payloads travel chunked. */
#define ELP_MAX_FRAME   250
#define ELP_LINK_HEADER 26
#define ELP_MAX_PAYLOAD (ELP_MAX_FRAME - ELP_LINK_HEADER)

/* ---- byte packing helpers (little-endian) ---- */
#define ELP_PUT_U16(b, v) do { \
    (b)[0] = (uint8_t)((v) & 0xFF);         (b)[1] = (uint8_t)(((v) >> 8) & 0xFF); } while (0)
#define ELP_PUT_U32(b, v) do { \
    (b)[0] = (uint8_t)((v) & 0xFF);         (b)[1] = (uint8_t)(((v) >> 8) & 0xFF); \
    (b)[2] = (uint8_t)(((v) >> 16) & 0xFF); (b)[3] = (uint8_t)(((v) >> 24) & 0xFF); } while (0)
#define ELP_PUT_U64(b, v) do { \
    (b)[0] = (uint8_t)((v) & 0xFF);         (b)[1] = (uint8_t)(((v) >> 8) & 0xFF); \
    (b)[2] = (uint8_t)(((v) >> 16) & 0xFF); (b)[3] = (uint8_t)(((v) >> 24) & 0xFF); \
    (b)[4] = (uint8_t)(((v) >> 32) & 0xFF); (b)[5] = (uint8_t)(((v) >> 40) & 0xFF); \
    (b)[6] = (uint8_t)(((v) >> 48) & 0xFF); (b)[7] = (uint8_t)(((v) >> 56) & 0xFF); } while (0)

#define ELP_GET_U16(b, v) do { \
    (v) = (uint16_t)((b)[0]) | ((uint16_t)((b)[1]) << 8); } while (0)
#define ELP_GET_U32(b, v) do { \
    (v) = (uint32_t)((b)[0]) | ((uint32_t)((b)[1]) << 8) | \
          ((uint32_t)((b)[2]) << 16) | ((uint32_t)((b)[3]) << 24); } while (0)
#define ELP_GET_U64(b, v) do { \
    (v) = (uint64_t)((b)[0]) | ((uint64_t)((b)[1]) << 8) | \
          ((uint64_t)((b)[2]) << 16) | ((uint64_t)((b)[3]) << 24) | \
          ((uint64_t)((b)[4]) << 32) | ((uint64_t)((b)[5]) << 40) | \
          ((uint64_t)((b)[6]) << 48) | ((uint64_t)((b)[7]) << 56); } while (0)

#endif /* ESPLAB_PROTOCOL_H */
