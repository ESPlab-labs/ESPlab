#ifndef ESPLAB_GLADOS_H
#define ESPLAB_GLADOS_H
/* ESPlab node core — portable C.
 *
 * One glados_t = one ESPlab node (a real board, or a virtual board: a Linux
 * process). The same core runs everywhere; only the transport differs.
 *
 * Threading model: the core is single-threaded. The host calls
 *   glados_tick()   every ~50ms (timers, retransmits, supervision)
 *   glados_recv()   once per received datagram
 *   glados_poll_out()  to drain datagrams and hand them to the transport
 * On FreeRTOS the firmware calls these from one task; on Linux the vboard
 * main loop calls them. No locks anywhere in this core.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "protocol.h"
#include "vm.h"   /* VM_CODE_MAX, GL_VM_INPUT_MAX */

#define GLADOS_MAX_NODES   16   /* roster capacity (boards + self) */
#define GLADOS_MAX_JOBS    8    /* simultaneous jobs on the machine */
#define GLADOS_MAX_CHUNKS  64   /* chunks per job */
#define GLADOS_MAX_KV      128  /* kv records per storage node */
#define GLADOS_KV_KEY_MAX  24
#define GLADOS_KV_VAL_MAX  160
#define GLADOS_NAME_MAX    16
#define GL_MAX_MSG         640  /* max reassembled message (VM chunks fit) */
#define GL_NODE_TIMEOUT_MS 6000 /* peer silent this long = down */
#define GL_FRAME_HEADER    10   /* ver(1) type(1) src(6) len(2) */
#define GL_FRAG_HEADER     15   /* magic(2) tag(2) seq(1) count(1) src(6) type(1) len(2) */
#define GL_FRAG_CAP        (ELP_MAX_PAYLOAD - GL_FRAG_HEADER) /* body bytes per fragment */

/* ---- node / cluster role ---- */
typedef enum {
    GL_ROLE_LONE = 0,   /* no peers heard yet; behave as coordinator */
    GL_ROLE_COORD,
    GL_ROLE_WORKER,
} gl_role_t;

typedef enum {
    GL_NODE_SELF = 0,
    GL_NODE_PEER,
    GL_NODE_DEAD,       /* timed out; kept for roster history */
} gl_node_state_t;

/* ---- job model ---- */
typedef enum {
    GL_TASK_HELLO = 1,      /* each core reports in; result = its card */
    GL_TASK_SEARCH = 2,     /* chunk = [lo,hi): count values passing pred */
    GL_TASK_VM = 3,         /* chunk = VM program + input slice */
} gl_taskkind_t;

typedef enum {
    GL_JOB_RUNNING = 0,
    GL_JOB_DONE,
    GL_JOB_FAILED,
} gl_jobstate_t;

/* per-chunk record kept by the coordinator (and by workers: last assignment) */
typedef struct {
    uint32_t lo, hi;        /* chunk space (search) or input offset (vm) */
    uint8_t  state;         /* 0=pending 1=assigned 2=done 3=failed */
    uint8_t  attempts;
    uint8_t  worker[6];     /* who has it (assigned/done) */
    uint64_t assigned_ms;
} gl_chunkrec_t;

typedef struct {
    uint64_t    id;
    char        name[GLADOS_NAME_MAX];
    gl_taskkind_t kind;
    gl_jobstate_t state;
    uint32_t    nchunks;
    uint32_t    done_count, fail_count;
    uint32_t    aggregate;  /* sum of chunk results (search count, etc.) */
    int32_t     params[4];  /* task-specific: pred value, range, ... */
    uint8_t     vm_code[VM_CODE_MAX];
    uint16_t    vm_code_len;
    uint8_t     vm_input[GL_VM_INPUT_MAX];
    uint16_t    vm_input_len;
    uint32_t    input_total;
    gl_chunkrec_t chunks[GLADOS_MAX_CHUNKS];
    uint64_t    next_poll_ms;   /* when the supervisor next checks liveness */
} gl_job_t;

/* ---- worker-side execution ---- */
typedef struct {
    uint64_t job_id;
    uint32_t chunk;
    uint32_t lo, hi;
    int32_t  params[4];
    uint16_t code_len, input_len;
    uint8_t  code[VM_CODE_MAX];
    uint8_t  input[GL_VM_INPUT_MAX];
} gl_assignment_t;

/* ---- kv storage ---- */
typedef struct {
    char     key[GLADOS_KV_KEY_MAX];
    uint16_t val_len;
    uint8_t  val[GLADOS_KV_VAL_MAX];
    bool     used;
} gl_kvrec_t;

/* ---- transport interface (implemented per platform) ---- */
typedef struct gl_transport gl_transport_t;
struct gl_transport {
    /* send payload to peer id (6 bytes); return 0 on ok, negative on error.
     * best-effort by design: the core layers handle retries. */
    int (*send)(gl_transport_t *t, const uint8_t peer[6],
                const uint8_t *payload, size_t len);
    /* broadcast to the group address */
    int (*broadcast)(gl_transport_t *t, const uint8_t *payload, size_t len);
    uint64_t now_ms;   /* transport clock (monotonic), refreshed per tick */
    void *ctx;
};

/* ---- the node ---- */
typedef struct gl_node {
    gl_transport_t *bus;

    uint8_t  self_id[6];
    uint8_t  chipspec;
    uint8_t  ncores;
    uint32_t ram_kb;
    uint32_t flash_mb;
    uint32_t capacities;
    int32_t  score;
    uint64_t boot_id;

    gl_role_t      role;
    uint8_t        coord_id[6];   /* current/last known coordinator */
    uint64_t       coord_since_ms;
    uint8_t        election_gen;  /* election round counter */
    bool           election_busy;
    uint64_t       election_until_ms;

    elp_node_t roster[GLADOS_MAX_NODES];
    uint8_t    roster_n;
    uint32_t   roster_seq;

    /* supervisor state (meaningful when role == COORD or LONE) */
    gl_job_t  jobs[GLADOS_MAX_JOBS];
    uint8_t   jobs_n;
    uint64_t  next_jobid;
    uint32_t  jobs_bump;        /* changes whenever the job list changes */

    /* worker state */
    gl_assignment_t asg;        /* current assignment, if any */
    bool     asg_active;
    bool     asg_local;         /* slot holds self-work (coordinator's own) */
    uint64_t asg_deadline_ms;   /* supervisor reassigns after this passes */

    /* local storage share */
    gl_kvrec_t kv[GLADOS_MAX_KV];
    uint8_t    kv_n;

    /* worker view of the machine's job list (announced by the brain) */
    uint32_t seen_jobs_running;
    uint32_t seen_jobs_bump;

    /* announce/timers */
    uint64_t next_announce_ms;
    uint64_t next_tick_ms;
    uint64_t next_pull_ms;
    uint64_t next_hello_ms;
    uint64_t election_pending_until;

    /* election tracking */
    bool     election_lost;
    bool     election_best_set;
    int32_t  election_best_score;
    uint64_t election_best_boot;
    uint8_t  election_best_id[6];

    /* fragmentation/reassembly (small messages pass straight through) */
    uint16_t frag_tag;                    /* rolling tag for outgoing */
    uint8_t  rx_active;                   /* reassembly in progress */
    uint16_t rx_tag, rx_total, rx_got;
    uint16_t rx_len;
    uint8_t  rx_type;                     /* message type carried by fragments */
    uint8_t  rx_src[6];
    uint8_t  rx_bits[16];                 /* which fragments have arrived */
    uint16_t rx_tail_len;                 /* size of the last fragment */
    uint8_t  rx_buf[GL_MAX_MSG];          /* reassembled body */
    uint8_t  rx_buf_r[GL_FRAME_HEADER + GL_MAX_MSG]; /* framed copy */

    /* small log ring for the console */
    char     log[8][96];
    uint8_t  log_head, log_n;

    /* stats */
    uint32_t stat_rx, stat_tx, stat_drops, stat_reassigns;
} gl_node_t;

/* ---- lifecycle ---- */
void glados_init(gl_node_t *g, gl_transport_t *bus,
                 const uint8_t self_id[6],
                 uint8_t chipspec, uint8_t ncores,
                 uint32_t ram_kb, uint32_t flash_mb,
                 uint32_t capacities, int32_t score);
void glados_tick(gl_node_t *g);          /* call every ~50 ms */
void glados_recv(gl_node_t *g, const uint8_t *payload, size_t len);

/* ---- console front-end (v1: the local console is always wired straight
 * into the node; with one node it IS the machine, exactly as designed) ---- */
typedef void (*gl_print_fn)(void *ctx, const char *line);
void gl_console(gl_node_t *g, const char *line, gl_print_fn out, void *ctx);

/* ---- kv API (local + cluster-aware) ---- */
int  gl_kv_put(gl_node_t *g, const char *key, const uint8_t *val, uint16_t len);
int  gl_kv_get(gl_node_t *g, const char *key, uint8_t *out, uint16_t *out_len);
int  gl_kv_del(gl_node_t *g, const char *key);

/* ---- job submission (programmatic; console wraps this) ---- */
int  gl_submit_hello(gl_node_t *g, uint64_t *job_id);
int  gl_submit_search(gl_node_t *g, uint32_t lo, uint32_t hi,
                      int32_t pred, uint64_t *job_id);

/* introspection for tests and consoles */
int  gl_submit_vm(gl_node_t *g, const uint8_t *code, uint16_t code_len,
                  const uint8_t *input, uint16_t input_len, uint32_t nchunks,
                  uint64_t *job_id);

int gl_job_find(gl_node_t *g, uint64_t job_id);
const char *gl_role_name(gl_role_t r);

/* VM chunk execution (exposed for the coordinator's self-work path) */
void worker_execute_and_deliver(gl_node_t *g, const uint8_t *body, size_t len);

#endif /* ESPLAB_GLADOS_H */
