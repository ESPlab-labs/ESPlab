/* ESPlab node core — see inc/machine.h.
 *
 * Design rules:
 *  - One node = one computer's "core". The coordinator is just the core that
 *    also keeps the job list and the disk index — and it still does work.
 *  - Chunks are self-contained: a worker needs nothing but the CHUNK payload
 *    to do its job. Boards can join, die, and rejoin freely.
 *  - Failure of one board is failure of the machine: the coordinator re-runs
 *    a dead core's chunk; if the coordinator itself dies the boards re-elect
 *    a new brain.
 *  - Never trust the radio: every inbound frame is length- and field-checked
 *    before it touches state.
 */
#include "machine.h"
#include "vm.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- timing constants (ms) ---- */
#define GL_ANNOUNCE_MS        1000
#define GL_COORD_TIMEOUT_MS   3500
#define GL_ELECTION_WINDOW_MS 600
#define GL_BOOT_QUIET_MS      2000   /* listen before claiming coordinator */
#define GL_PULL_RETRY_MS      250
#define GL_ASG_TIMEOUT_MS     8000
#define GL_JOB_POLL_MS        1000

/* ===================== small helpers ===================== */

static uint64_t now_ms(gl_node_t *g) { return g->bus ? g->bus->now_ms : 0; }

static void log_add(gl_node_t *g, const char *fmt, ...)
{
    char *dst = g->log[g->log_head];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, sizeof(g->log[0]), fmt, ap);
    va_end(ap);
    g->log_head = (uint8_t)((g->log_head + 1) & 7);
    if (g->log_n < 8) g->log_n++;
}

static bool id_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool id_is_zero(const uint8_t a[6])
{
    static const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
    return id_eq(a, z);
}

static uint64_t boot_id_from(const uint8_t id[6], uint64_t now)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 6; i++) {
        h ^= id[i];
        h *= 0x100000001B3ull;
    }
    h ^= now;
    h *= 0xD1B54A32D192ED03ull;
    return h ^ (h >> 29);
}

/* ===================== roster ===================== */

static int roster_find(gl_node_t *g, const uint8_t id[6])
{
    for (int i = 0; i < g->roster_n; i++)
        if (id_eq(g->roster[i].id, id)) return i;
    return -1;
}

static int roster_upsert(gl_node_t *g, const uint8_t id[6])
{
    int i = roster_find(g, id);
    if (i >= 0) return i;
    if (g->roster_n >= GL_MAX_NODES) return -1;
    i = g->roster_n++;
    memset(&g->roster[i], 0, sizeof(elp_node_t));
    memcpy(g->roster[i].id, id, 6);
    return i;
}

static void roster_touch(gl_node_t *g, const uint8_t id[6])
{
    int i = roster_find(g, id);
    if (i >= 0) g->roster[i].last_seen_ms = now_ms(g);
}

static uint32_t roster_alive_n(gl_node_t *g)
{
    uint32_t n = 0;
    uint64_t now = now_ms(g);
    for (int i = 0; i < g->roster_n; i++)
        if (!id_eq(g->roster[i].id, g->self_id) &&
            g->roster[i].last_seen_ms != 0 &&
            now - g->roster[i].last_seen_ms <= GL_NODE_TIMEOUT_MS)
            n++;
    return n;
}

/* ===================== frame send (with fragmentation) ===================== */

/* A small message goes out as one frame. A big one (a VM chunk can be ~500
 * bytes, the radio carries ~212) is split into fragments sharing a tag:
 *   frag header: 0xFF 0xFE tag(2) seq(1) count(1)
 * Every transport gets fragmentation for free because it lives here. */
static void send_frame(gl_node_t *g, const uint8_t dst[6], uint8_t type,
                       const uint8_t *body, size_t len);

/* public wrapper (console submit-forwarding uses it) */
void gl_send_frame(gl_node_t *g, const uint8_t dst[6], uint8_t type,
                   const uint8_t *body, size_t len)
{
    send_frame(g, dst, type, body, len);
}

static void send_frame(gl_node_t *g, const uint8_t dst[6], uint8_t type,
                       const uint8_t *body, size_t len)
{
    uint8_t buf[ELP_MAX_PAYLOAD];
    size_t cap = ELP_MAX_PAYLOAD - GL_FRAME_HEADER;
    size_t fcap = ELP_MAX_PAYLOAD - GL_FRAG_HEADER;  /* fragments carry 15B hdr */
    g->stat_tx++;

    if (len <= cap) {
        buf[0] = ELP_VERSION;
        buf[1] = type;
        memcpy(buf + 2, g->self_id, 6);
        buf[8] = (uint8_t)(len & 0xFF);
        buf[9] = (uint8_t)((len >> 8) & 0xFF);
        if (len) memcpy(buf + GL_FRAME_HEADER, body, len);
        if (dst) g->bus->send(g->bus, dst, buf, len + GL_FRAME_HEADER);
        else     g->bus->broadcast(g->bus, buf, len + GL_FRAME_HEADER);
        return;
    }
    if (len > GL_MAX_MSG - GL_FRAME_HEADER) { g->stat_drops++; return; }

    uint16_t tag = ++g->frag_tag;
    uint16_t count = (uint16_t)((len + fcap - 1) / fcap);
    if (count > 255) { g->stat_drops++; return; }
    for (uint16_t i = 0; i < count; i++) {
        uint16_t off = (uint16_t)(i * fcap);
        uint16_t n = (uint16_t)((len - off) < fcap ? (len - off) : fcap);
        buf[0] = 0xFF; buf[1] = 0xFE;
        buf[2] = (uint8_t)(tag & 0xFF);
        buf[3] = (uint8_t)((tag >> 8) & 0xFF);
        buf[4] = (uint8_t)i;
        buf[5] = (uint8_t)count;
        memcpy(buf + 6, g->self_id, 6);
        buf[12] = (uint8_t)type;
        buf[13] = (uint8_t)(n & 0xFF);
        buf[14] = (uint8_t)((n >> 8) & 0xFF);
        memcpy(buf + 15, body + off, n);
        size_t flen = 15 + n;
        if (flen > ELP_MAX_PAYLOAD) { g->stat_drops++; return; }
        if (dst) g->bus->send(g->bus, dst, buf, flen);
        else     g->bus->broadcast(g->bus, buf, flen);
    }
}

/* reassemble inbound fragments; returns true when a full message was fed.
 * Order-independent: fragments land at their offsets; the message completes
 * when every piece has arrived (radios do not promise ordering). */
static bool frag_recv(gl_node_t *g, const uint8_t *payload, size_t len)
{
    if (len < GL_FRAG_HEADER) { g->stat_drops++; return false; }
    uint16_t tag = (uint16_t)(payload[2] | ((uint16_t)payload[3] << 8));
    uint8_t seq = payload[4], count = payload[5];
    if (count == 0 || count > 16) { g->stat_drops++; return false; }

    if (!g->rx_active || g->rx_tag != tag) {
        g->rx_active = true;
        g->rx_tag = tag;
        g->rx_total = count;
        g->rx_got = 0;
        g->rx_len = 0;
        g->rx_type = payload[12];
        memcpy(g->rx_src, payload + 6, 6);
        memset(g->rx_bits, 0, sizeof(g->rx_bits));
    }
    if (g->rx_total != count) { g->rx_active = 0; g->stat_drops++; return false; }
    if (seq >= g->rx_total || g->rx_bits[seq]) { g->stat_drops++; return false; }

    uint16_t n = (uint16_t)(payload[13] | ((uint16_t)payload[14] << 8));
    if (n > GL_FRAG_CAP ||
        (size_t)seq * GL_FRAG_CAP + n > GL_MAX_MSG - GL_FRAME_HEADER) {
        g->rx_active = 0; g->stat_drops++; return false;
    }
    memcpy(g->rx_buf + (size_t)seq * GL_FRAG_CAP, payload + GL_FRAG_HEADER, n);
    if (seq == g->rx_total - 1) g->rx_tail_len = n;
    g->rx_bits[seq] = 1;
    g->rx_got++;
    if (g->rx_got < g->rx_total) return false;   /* need more pieces */

    /* complete: total length = full pieces + the tail piece */
    g->rx_len = (uint16_t)((g->rx_total - 1) * GL_FRAG_CAP + g->rx_tail_len);

    /* build the normal frame header the dispatcher expects
     * (fragments carry only the body; the frame wraps it here) */
    g->rx_buf_r[0] = ELP_VERSION;
    g->rx_buf_r[1] = g->rx_type;
    memcpy(g->rx_buf_r + 2, g->rx_src, 6);
    g->rx_buf_r[8] = (uint8_t)(g->rx_len & 0xFF);
    g->rx_buf_r[9] = (uint8_t)((g->rx_len >> 8) & 0xFF);
    memcpy(g->rx_buf_r + GL_FRAME_HEADER, g->rx_buf, g->rx_len);
    g->rx_active = 0;
    return true;
}

/* ===================== election ===================== */

static bool beats_me(const gl_node_t *g, int32_t score, uint64_t boot_id,
                     const uint8_t id[6])
{
    if (score != g->score) return score > g->score;
    if (boot_id != g->boot_id) return boot_id > g->boot_id;
    return memcmp(id, g->self_id, 6) > 0;
}

static bool beats_best(gl_node_t *g, int32_t score, uint64_t boot_id,
                       const uint8_t id[6])
{
    if (!g->election_best_set) return true;
    if (score != g->election_best_score) return score > g->election_best_score;
    if (boot_id != g->election_best_boot) return boot_id > g->election_best_boot;
    return memcmp(id, g->election_best_id, 6) > 0;
}

static void election_start(gl_node_t *g, const char *why)
{
    uint8_t body[16];
    g->election_gen++;
    g->election_busy = true;
    g->election_lost = false;
    g->election_best_set = false;
    g->election_until_ms = now_ms(g) + GL_ELECTION_WINDOW_MS;
    memset(g->coord_id, 0, 6);       /* forget the old brain, all 6 bytes */
    ELP_PUT_U32(body + 0, (uint32_t)g->election_gen);
    ELP_PUT_U32(body + 4, (uint32_t)g->score);
    ELP_PUT_U64(body + 8, g->boot_id);
    log_add(g, "election gen=%lu (%s)", (unsigned long)g->election_gen, why);
    send_frame(g, NULL, ELP_ELECTION, body, 16);
}

/* Ask this node to call an election now. The board with the best score
 * (then boot id, then id) wins, so this is how a late-joining board takes
 * the machine on merit — the dashboard uses it so its console can see the
 * coordinator's job state. A no-op if we already run the machine. */
void gl_force_election(gl_node_t *g)
{
    if (g->role == GL_ROLE_COORD) return;
    election_start(g, "consolidation requested");
}

static void election_finish(gl_node_t *g)
{
    uint8_t body[18];
    g->election_busy = false;
    g->role = GL_ROLE_COORD;
    memcpy(g->coord_id, g->self_id, 6);
    g->coord_since_ms = now_ms(g);
    log_add(g, "I am the coordinator");
    memcpy(body, g->self_id, 6);
    ELP_PUT_U32(body + 6, (uint32_t)g->score);
    ELP_PUT_U64(body + 10, g->boot_id);
    send_frame(g, NULL, ELP_COORD, body, 18);
    g->next_announce_ms = now_ms(g) + GL_ANNOUNCE_MS;
}

/* accept src as our coordinator (score/boot carried when body_len >= 18) */
static void coord_accept(gl_node_t *g, const uint8_t *src,
                         const uint8_t *body, size_t body_len)
{
    int32_t their_score = 0;
    uint64_t their_boot = 0;
    if (body_len >= 18) {
        uint32_t tmp32;
        ELP_GET_U32(body + 6, tmp32); their_score = (int32_t)tmp32;
        ELP_GET_U64(body + 10, their_boot);
        if (body_len >= 26) {
            ELP_GET_U32(body + 18, g->seen_jobs_running);
            ELP_GET_U32(body + 22, g->seen_jobs_bump);
        }
    }
    int i = roster_upsert(g, src);
    if (i >= 0) g->roster[i].last_seen_ms = now_ms(g);

    if (g->role == GL_ROLE_COORD) {
        /* two brains: the better one keeps the job; the other defers */
        if (beats_me(g, their_score, their_boot, src)) {
            g->role = GL_ROLE_WORKER;
            memcpy(g->coord_id, src, 6);
            g->coord_since_ms = now_ms(g);
            log_add(g, "yielding coordinator role");
        } else {
            /* re-assert once; their next tick fixes them or us */
            uint8_t my[18];
            memcpy(my, g->self_id, 6);
            ELP_PUT_U32(my + 6, (uint32_t)g->score);
            ELP_PUT_U64(my + 10, g->boot_id);
            send_frame(g, NULL, ELP_COORD, my, 18);
        }
        return;
    }
    if (g->role == GL_ROLE_WORKER && id_eq(g->coord_id, src)) {
        g->coord_since_ms = now_ms(g);           /* refresh watchdog only */
        return;
    }
    /* LONE, or worker whose brain changed, or candidate with lower score
     * than an existing coord: an already-running cluster wins over a fresh
     * board's ambition (their claim predates us) */
    if (g->role == GL_ROLE_LONE ||
        !g->election_busy ||
        body_len >= 18) {
        g->election_busy = false;
        g->role = GL_ROLE_WORKER;
        memcpy(g->coord_id, src, 6);
        g->coord_since_ms = now_ms(g);
        log_add(g, "coordinator is %02x%02x%02x%02x%02x%02x",
                src[0], src[1], src[2], src[3], src[4], src[5]);
    }
}

/* ===================== jobs (supervisor side) ===================== */

static void job_setup_chunks(gl_job_t *j)
{
    uint32_t lo = (uint32_t)j->params[1];
    uint32_t hi = (uint32_t)j->params[2];
    uint32_t span = (hi > lo) ? (hi - lo) : 1;
    for (uint32_t c = 0; c < j->nchunks; c++) {
        uint32_t a = lo + (uint32_t)((uint64_t)span * c / j->nchunks);
        uint32_t b = lo + (uint32_t)((uint64_t)span * (c + 1) / j->nchunks);
        if (c == j->nchunks - 1) b = hi;
        j->chunks[c].lo = a;
        j->chunks[c].hi = b;
        j->chunks[c].state = 0;
        j->chunks[c].attempts = 0;
        j->chunks[c].assigned_ms = 0;
        memset(j->chunks[c].worker, 0, 6);
    }
}

static gl_job_t *job_alloc(gl_node_t *g, gl_taskkind_t kind, const char *name)
{
    if (g->jobs_n >= GL_MAX_JOBS) {
        /* table full: evict the oldest finished job; refuse if all running */
        int victim = -1;
        for (uint8_t i = 0; i < g->jobs_n; i++)
            if (g->jobs[i].state != GL_JOB_RUNNING) { victim = i; break; }
        if (victim < 0) return NULL;
        for (uint8_t i = (uint8_t)victim; i + 1 < g->jobs_n; i++)
            g->jobs[i] = g->jobs[i + 1];
        g->jobs_n--;
    }
    gl_job_t *j = &g->jobs[g->jobs_n++];
    memset(j, 0, sizeof(*j));
    j->id = ++g->next_jobid;
    j->kind = kind;
    snprintf(j->name, sizeof(j->name), "%s", name ? name : "job");
    j->state = GL_JOB_RUNNING;
    j->started_ms = now_ms(g);
    return j;
}

static uint16_t chunk_payload(const gl_job_t *j, uint32_t c,
                              uint8_t *out, uint16_t cap)
{
    uint16_t p = 0;
    if (cap < 64) return 0;
    memcpy(out + p, j->name, GL_NAME_MAX); p += GL_NAME_MAX;
    ELP_PUT_U64(out + p, j->id); p += 8;
    ELP_PUT_U32(out + p, c); p += 4;
    out[p++] = (uint8_t)j->kind;
    ELP_PUT_U32(out + p, j->chunks[c].lo); p += 4;
    ELP_PUT_U32(out + p, j->chunks[c].hi); p += 4;
    for (int i = 0; i < 4; i++) { ELP_PUT_U32(out + p, (uint32_t)j->params[i]); p += 4; }
    if (j->kind == GL_TASK_VM || j->kind == GL_TASK_FRAME) {
        ELP_PUT_U16(out + p, j->vm_code_len); p += 2;
        ELP_PUT_U16(out + p, j->vm_input_len); p += 2;
        if ((uint32_t)p + j->vm_code_len + j->vm_input_len > cap) return 0;
        memcpy(out + p, j->vm_code, j->vm_code_len); p = (uint16_t)(p + j->vm_code_len);
        memcpy(out + p, j->vm_input, j->vm_input_len); p = (uint16_t)(p + j->vm_input_len);
    }
    return p;
}

static int job_pending_chunk(gl_job_t *j)
{
    for (uint32_t c = 0; c < j->nchunks; c++)
        if (j->chunks[c].state == 0) return (int)c;
    return -1;
}

static void job_check_done(gl_node_t *g, gl_job_t *j)
{
    bool any_open = false, any_failed = false;
    for (uint32_t c = 0; c < j->nchunks; c++) {
        if (j->chunks[c].state == 0 || j->chunks[c].state == 1) any_open = true;
        if (j->chunks[c].state == 3) any_failed = true;
    }
    if (any_open) return;
    j->state = any_failed ? GL_JOB_FAILED : GL_JOB_DONE;
    j->finished_ms = now_ms(g);
    log_add(g, "job %s %s: %lu/%lu chunks ok, agg=%lu",
            j->name, (j->state == GL_JOB_DONE) ? "DONE" : "FAILED",
            (unsigned long)j->done_count, (unsigned long)j->nchunks,
            (unsigned long)j->aggregate);
}

static void coord_give_chunk(gl_node_t *g, const uint8_t worker[6])
{
    static uint8_t pay[GL_MAX_MSG];
    for (uint8_t ji = 0; ji < g->jobs_n; ji++) {
        gl_job_t *j = &g->jobs[ji];
        if (j->state != GL_JOB_RUNNING) continue;
        int c = job_pending_chunk(j);
        if (c < 0) continue;
        uint16_t len = chunk_payload(j, (uint32_t)c, pay, sizeof(pay));
        if (!len) continue;
        j->chunks[c].state = 1;
        j->chunks[c].attempts++;
        j->chunks[c].assigned_ms = now_ms(g);
        memcpy(j->chunks[c].worker, worker, 6);
        send_frame(g, worker, ELP_CHUNK, pay, len);
        return;
    }
    /* nothing pending: tell them so their pull retry backs off naturally */
    send_frame(g, worker, ELP_RESULT_ACK, NULL, 0);
}

static void coord_result(gl_node_t *g, const uint8_t *body, size_t len)
{
    const uint8_t *p = body;
    if (len < 6 + 8 + 4 + 1 + 2) { g->stat_drops++; return; }
    const uint8_t *from = p; p += 6;
    uint64_t job_id; ELP_GET_U64(p, job_id); p += 8;
    uint32_t chunk;  ELP_GET_U32(p, chunk); p += 4;
    uint8_t status = *p++;
    uint16_t out_len; ELP_GET_U16(p, out_len); p += 2;
    if (out_len > JOB_OUT_MAX || len < (size_t)(p - body) + out_len) {
        g->stat_drops++; return;
    }
    int ji = gl_job_find(g, job_id);
    if (ji < 0) { g->stat_drops++; return; }
    gl_job_t *j = &g->jobs[ji];
    if (chunk >= j->nchunks) { g->stat_drops++; return; }
    gl_chunkrec_t *r = &j->chunks[chunk];
    if (r->state == 2 || r->state == 3) { g->stat_drops++; return; } /* stale dup */
    r->state = (status == 0) ? 2 : 3;
    memcpy(r->worker, from, 6);
    if (status == 0) {
        j->done_count++;
        /* keep the chunk's raw output (VM jobs deliver bytes, not numbers) */
        if (out_len > GL_CHUNK_OUT_MAX) out_len = GL_CHUNK_OUT_MAX;
        if (out_len) { memcpy(r->out, p, out_len); r->out_len = out_len; }
        if (out_len >= 4) {
            uint32_t val; ELP_GET_U32(p, val);
            if (j->kind == GL_TASK_SEARCH) {
                j->aggregate += val;
                log_add(g, "result %s#%lu: ok count=%lu", j->name,
                        (unsigned long)chunk, (unsigned long)val);
            } else if (j->kind == GL_TASK_VM) {
                j->aggregate += val;
                log_add(g, "result %s#%lu: ok val=%lu", j->name,
                        (unsigned long)chunk, (unsigned long)val);
            }
        } else if (out_len > 0) {
            char txt[24];
            uint16_t n = out_len < 23 ? out_len : 23;
            memcpy(txt, p, n);
            txt[n] = 0;
            log_add(g, "result %s#%lu: ok '%s'", j->name, (unsigned long)chunk, txt);
        }
    } else {
        j->fail_count++;
        log_add(g, "result %s#%lu: FAILED status=%lu", j->name,
                (unsigned long)chunk, (unsigned long)status);
    }
    uint8_t ack[13];
    ELP_PUT_U64(ack, job_id);
    ELP_PUT_U32(ack + 8, chunk);
    ack[12] = 0;
    send_frame(g, from, ELP_RESULT_ACK, ack, 13);
    job_check_done(g, j);
}

static void coord_job_poll(gl_node_t *g)
{
    uint64_t now = now_ms(g);
    for (uint8_t ji = 0; ji < g->jobs_n; ji++) {
        gl_job_t *j = &g->jobs[ji];
        if (j->state != GL_JOB_RUNNING) continue;
        for (uint32_t c = 0; c < j->nchunks; c++) {
            gl_chunkrec_t *r = &j->chunks[c];
            /* dead-core rescheduling: an assignment that has sat too long
             * (its worker went silent or the result was lost) goes back in
             * the pool, up to three attempts, then the chunk (and job) fails */
            if (r->state == 1 && r->assigned_ms &&
                now - r->assigned_ms > GL_ASG_TIMEOUT_MS) {
                if (r->attempts >= 5) {
                    r->state = 3;
                    j->fail_count++;
                    log_add(g, "chunk %s#%lu failed after %lu attempts",
                            j->name, (unsigned long)c,
                            (unsigned long)j->chunks[c].attempts);
                    job_check_done(g, j);
                } else {
                    r->state = 0;
                    r->assigned_ms = 0;
                    log_add(g, "chunk %s#%u back in pool (worker silent)",
                            j->name, c);
                }
            }
        }
    }
}

/* the coordinator is also a worker: run one pending chunk inline.
 * asg_local marks the slot as MINE (not a radio dispatch), so the worker's
 * on_chunk guard and the result deliverer both know what this is. */
static void coord_self_work(gl_node_t *g)
{
    static uint8_t pay[GL_MAX_MSG];
    if (g->asg_active) return;
    for (uint8_t ji = 0; ji < g->jobs_n; ji++) {
        gl_job_t *j = &g->jobs[ji];
        if (j->state != GL_JOB_RUNNING) continue;
        int c = job_pending_chunk(j);
        if (c < 0) continue;
        uint16_t len = chunk_payload(j, (uint32_t)c, pay, sizeof(pay));
        if (!len) continue;
        j->chunks[c].state = 1;
        j->chunks[c].attempts++;
        j->chunks[c].assigned_ms = now_ms(g);
        memcpy(j->chunks[c].worker, g->self_id, 6);
        g->asg_local = true;
        worker_execute_and_deliver(g, pay, len);
        return;
    }
}

/* ===================== worker side ===================== */

/* execute a chunk and deliver the result (radio, or straight to ourselves
 * when this node is its own coordinator — the loopback shortcut) */
void worker_execute_and_deliver(gl_node_t *g, const uint8_t *body, size_t len)
{
    gl_assignment_t *a = &g->asg;
    uint8_t out[JOB_OUT_MAX];
    uint16_t out_len = 0;
    uint8_t status = 0;
    uint64_t job_id = 0;
    uint32_t chunkno = 0;

    memset(a, 0, sizeof(*a));
    if (len < 16 + 8 + 4 + 1 + 4 + 4 + 16) { g->stat_drops++; g->asg_active = false; return; }
    uint16_t p = 16;                       /* skip job name */
    ELP_GET_U64(body + p, job_id); p += 8;
    ELP_GET_U32(body + p, chunkno); p += 4;
    uint8_t kind = body[p++];
    ELP_GET_U32(body + p, a->lo); p += 4;
    ELP_GET_U32(body + p, a->hi); p += 4;
    for (int i = 0; i < 4; i++) {
        uint32_t tmp32;
        ELP_GET_U32(body + p, tmp32);
        a->params[i] = (int32_t)tmp32;
        p += 4;
    }

    if (kind == GL_TASK_SEARCH) {
        /* count values in [lo,hi) divisible by pred */
        int32_t pred = a->params[0];
        if (pred <= 0) {
            status = 3;
        } else {
            uint32_t count = 0;
            for (uint32_t v = a->lo; v < a->hi; v++)
                if ((v % (uint32_t)pred) == 0) count++;
            ELP_PUT_U32(out, count);
            out_len = 4;
        }
    } else if (kind == GL_TASK_HELLO) {
        /* result = this core's card, as text */
        out_len = (uint16_t)snprintf((char *)out, JOB_OUT_MAX,
            "core %02x%02x chip=%lu cores=%lu ram=%luk flash=%lumb",
            g->self_id[4], g->self_id[5], (unsigned long)g->chipspec,
            (unsigned long)g->ncores, (unsigned long)g->ram_kb,
            (unsigned long)g->flash_mb);
        if (out_len >= JOB_OUT_MAX) out_len = JOB_OUT_MAX - 1;
    } else if (kind == GL_TASK_VM || kind == GL_TASK_FRAME) {
        ELP_GET_U16(body + p, a->code_len); p += 2;
        ELP_GET_U16(body + p, a->input_len); p += 2;
        if (a->code_len > VM_CODE_MAX || a->input_len > GL_VM_INPUT_MAX ||
            len < (size_t)p + a->code_len + a->input_len) {
            status = 3;
        } else {
            memcpy(a->code, body + p, a->code_len); p = (uint16_t)(p + a->code_len);
            memcpy(a->input, body + p, a->input_len);
            vm_t vm;
            vm_init(&vm, a->code, a->code_len, a->input, a->input_len,
                    out, JOB_OUT_MAX, (int32_t)a->lo, (int32_t)a->hi);
            for (int slice = 0; slice < 40 && vm.status == VM_OK; slice++) {
                vm_run(&vm, VM_CYCLES_PER_SLICE);
                if (vm.halted) break;      /* finished cleanly: stop slicing */
            }
            /* sandbox bound: a task that never halted within its slice
             * budget is killed here (bounded, and no false positives on
             * long-running loops aligned to the timeslice) */
            if (!vm.halted && vm.status == VM_OK) vm.status = VM_FAULT_STACK;
            status = (vm.status == VM_OK) ? 0 : 2;
            out_len = vm.out_len;
        }
    } else {
        status = 3;
    }

    uint8_t rbuf[6 + 8 + 4 + 1 + 2 + JOB_OUT_MAX];
    uint16_t rp = 0;
    memcpy(rbuf + rp, g->self_id, 6); rp += 6;
    ELP_PUT_U64(rbuf + rp, job_id); rp += 8;
    ELP_PUT_U32(rbuf + rp, chunkno); rp += 4;
    rbuf[rp++] = status;
    ELP_PUT_U16(rbuf + rp, out_len); rp += 2;
    memcpy(rbuf + rp, out, out_len); rp = (uint16_t)(rp + out_len);

    if (g->asg_local) {
        /* self-work: deliver straight to our own coordinator role */
        g->asg_local = false;
        g->asg_active = false;
        coord_result(g, rbuf, rp);
        return;
    }
    if (id_eq(g->coord_id, g->self_id))
        coord_result(g, rbuf, rp);          /* loopback (should not happen) */
    else if (!id_is_zero(g->coord_id))
        send_frame(g, g->coord_id, ELP_RESULT, rbuf, rp);
    g->asg_active = false;
}

/* ===================== kv storage ===================== */

static gl_kvrec_t *kv_find(gl_node_t *g, const char *key)
{
    for (uint8_t i = 0; i < g->kv_n; i++)
        if (strncmp(g->kv[i].key, key, GL_KV_KEY_MAX) == 0) return &g->kv[i];
    return NULL;
}

int gl_kv_put(gl_node_t *g, const char *key, const uint8_t *val, uint16_t len)
{
    if (!key || !*key || strlen(key) >= GL_KV_KEY_MAX) return -1;
    if (len > GL_KV_VAL_MAX) return -1;
    gl_kvrec_t *r = kv_find(g, key);
    if (!r) {
        if (g->kv_n >= GL_MAX_KV) return -1;
        r = &g->kv[g->kv_n++];
        memset(r, 0, sizeof(*r));
        snprintf(r->key, sizeof(r->key), "%s", key);
    }
    memcpy(r->val, val, len);
    r->val_len = len;
    r->used = true;
    return 0;
}

int gl_kv_get(gl_node_t *g, const char *key, uint8_t *out, uint16_t *out_len)
{
    gl_kvrec_t *r = kv_find(g, key);
    if (!r) return -1;
    if (*out_len < r->val_len) return -1;
    memcpy(out, r->val, r->val_len);
    *out_len = r->val_len;
    return 0;
}

int gl_kv_del(gl_node_t *g, const char *key)
{
    for (uint8_t i = 0; i < g->kv_n; i++) {
        if (strncmp(g->kv[i].key, key, GL_KV_KEY_MAX) == 0) {
            g->kv[i] = g->kv[g->kv_n - 1];
            g->kv_n--;
            return 0;
        }
    }
    return -1;
}

/* count running jobs on the machine (the brain announces this) */
static uint32_t machine_jobs_running(gl_node_t *g)
{
    uint32_t n = 0;
    for (uint8_t i = 0; i < g->jobs_n; i++)
        if (g->jobs[i].state == GL_JOB_RUNNING) n++;
    return n;
}

/* ===================== submit forwarding ===================== */
/* A worker's console forwards SUBMIT to the brain; the brain runs it. */
static void coord_submit(gl_node_t *g, const uint8_t *body, size_t len)
{
    if (len < 17) { g->stat_drops++; return; }
    uint8_t kind = body[0];
    uint32_t a, b, c, d;
    ELP_GET_U32(body + 1, a);
    ELP_GET_U32(body + 5, b);
    ELP_GET_U32(body + 9, c);
    ELP_GET_U32(body + 13, d);
    (void)d;
    if (kind == GL_TASK_HELLO) {
        uint64_t id;
        if (gl_submit_hello(g, &id) == 0)
            log_add(g, "remote submit: hello #%llu", (unsigned long long)id);
    } else if ((kind == GL_TASK_VM || kind == GL_TASK_FRAME) && len >= 9) {
        /* kind(1) nchunks(4) codeLen(2) inputLen(2) code input */
        uint32_t nch; uint16_t cl2, il2;
        ELP_GET_U32(body + 1, nch);
        ELP_GET_U16(body + 5, cl2);
        ELP_GET_U16(body + 7, il2);
        if ((size_t)(9 + cl2 + il2) > len || cl2 == 0 || cl2 > VM_CODE_MAX ||
            il2 > GL_VM_INPUT_MAX || nch == 0) { g->stat_drops++; return; }
        uint64_t id;
        int rc = (kind == GL_TASK_FRAME)
            ? gl_submit_frame(g, body + 9, cl2, body + 9 + cl2, il2, nch, &id)
            : gl_submit_vm(g, body + 9, cl2, body + 9 + cl2, il2, nch, &id);
        if (rc == 0)
            log_add(g, "remote submit: %s %luB code, %luB input, %lu chunks #%llu",
                    (kind == GL_TASK_FRAME) ? "frame" : "vm",
                    (unsigned long)cl2, (unsigned long)il2, (unsigned long)nch,
                    (unsigned long long)id);
    } else if (kind == GL_TASK_SEARCH) {
        uint64_t id;
        if (gl_submit_search(g, b, c, (int32_t)a, &id) == 0)
            log_add(g, "remote submit: search %u-%u pred=%d #%llu",
                    b, c, (int32_t)a, (unsigned long long)id);
    }
}

/* ===================== inbound dispatch ===================== */

static void on_hello(gl_node_t *g, const uint8_t *src, const uint8_t *body, size_t len)
{
    if (len < 22) { g->stat_drops++; return; }
    int i = roster_upsert(g, src);
    if (i < 0) return;
    elp_node_t *n = &g->roster[i];
    n->chipspec = body[0];
    n->ncores = body[1];
    uint16_t ram, flash;
    ELP_GET_U16(body + 2, ram);
    ELP_GET_U16(body + 4, flash);
    ELP_GET_U32(body + 6, n->capacities);
    {
        uint32_t tmp32;
        ELP_GET_U32(body + 10, tmp32);
        n->score = (int32_t)tmp32;
    }
    ELP_GET_U64(body + 14, n->boot_id);
    n->ram_kb_x16 = (uint8_t)(ram / 16);
    n->ram_kb_rem = (uint8_t)(ram % 16);
    n->flash_mb_x16 = (uint8_t)(flash / 16);
    n->flash_mb_rem = (uint8_t)(flash % 16);
    n->last_seen_ms = now_ms(g);
    /* greet them so a joining board learns the brain within one round trip */
    if (g->role == GL_ROLE_COORD) {
        uint8_t my[18];
        memcpy(my, g->self_id, 6);
        ELP_PUT_U32(my + 6, (uint32_t)g->score);
        ELP_PUT_U64(my + 10, g->boot_id);
        send_frame(g, src, ELP_COORD, my, 18);
    }
}

static void on_election(gl_node_t *g, const uint8_t *src, const uint8_t *body, size_t len)
{
    if (len < 16) { g->stat_drops++; return; }
    int32_t their_score;
    uint64_t their_boot;
    {
        uint32_t tmp32;
        ELP_GET_U32(body + 4, tmp32);
        their_score = (int32_t)tmp32;
    }
    ELP_GET_U64(body + 8, their_boot);
    roster_touch(g, src);
    if (!beats_me(g, their_score, their_boot, src)) return;
    if (g->role == GL_ROLE_COORD) {
        g->role = GL_ROLE_WORKER;
        log_add(g, "stepping down for a better candidate");
    }
    if (g->election_busy && beats_best(g, their_score, their_boot, src)) {
        g->election_best_set = true;
        g->election_best_score = their_score;
        g->election_best_boot = their_boot;
        memcpy(g->election_best_id, src, 6);
        g->election_lost = true;
    }
}

static void on_chunk_req(gl_node_t *g, const uint8_t *src)
{
    roster_touch(g, src);
    if (g->role != GL_ROLE_COORD) return;
    coord_give_chunk(g, src);
}

static void on_chunk(gl_node_t *g, const uint8_t *src, const uint8_t *body, size_t len)
{
    roster_touch(g, src);
    if (g->asg_active) { g->stat_drops++; return; }  /* busy; coord will reassign */
    if (len < 53) { g->stat_drops++; return; }
    g->asg_active = true;
    g->asg_deadline_ms = now_ms(g) + GL_ASG_TIMEOUT_MS;
    worker_execute_and_deliver(g, body, len);
}

void gl_recv(gl_node_t *g, const uint8_t *payload, size_t len)
{
    g->stat_rx++;
    if (len < GL_FRAME_HEADER || len > GL_MAX_MSG) { g->stat_drops++; return; }
    if (payload[0] == 0xFF && payload[1] == 0xFE) {
        if (!frag_recv(g, payload, len)) return;
        payload = g->rx_buf_r;
        len = g->rx_len + GL_FRAME_HEADER;
        if (len < GL_FRAME_HEADER) { g->stat_drops++; return; }
    }
    if (payload[0] != ELP_VERSION) { g->stat_drops++; return; }
    uint8_t type = payload[1];
    const uint8_t *src = payload + 2;
    uint16_t blen = (uint16_t)(payload[8] | ((uint16_t)payload[9] << 8));
    if (blen != len - GL_FRAME_HEADER) { g->stat_drops++; return; }
    if (id_eq(src, g->self_id)) return;      /* our own broadcast echo */
    const uint8_t *body = payload + GL_FRAME_HEADER;

    switch (type) {
    case ELP_HELLO:      on_hello(g, src, body, blen); break;
    case ELP_ANNOUNCE:   coord_accept(g, src, body, blen); break;
    case ELP_ELECTION:   on_election(g, src, body, blen); break;
    case ELP_COORD:      coord_accept(g, src, body, blen); break;
    case ELP_CHUNK_REQ:  on_chunk_req(g, src); break;
    case ELP_SUBMIT:     if (g->role == GL_ROLE_COORD) coord_submit(g, body, blen); break;
    case ELP_CHUNK:      on_chunk(g, src, body, blen); break;
    case ELP_RESULT:     coord_result(g, body, blen); break;
    case ELP_RESULT_ACK: roster_touch(g, src); break;
    default: g->stat_drops++; break;
    }
}

/* ===================== tick ===================== */

void gl_tick(gl_node_t *g)
{
    uint64_t now = now_ms(g);

    /* roster aging */
    for (int i = 0; i < g->roster_n; i++) {
        elp_node_t *n = &g->roster[i];
        if (id_eq(n->id, g->self_id)) continue;
        if (n->last_seen_ms && now - n->last_seen_ms > GL_NODE_TIMEOUT_MS) {
            log_add(g, "node %02x%02x silent", n->id[4], n->id[5]);
            n->last_seen_ms = 0;
        }
    }

    /* boot: listen quietly, re-helloing while alone, then claim the machine
     * if nobody spoke (first hello can be lost on a real radio) */
    if (g->role == GL_ROLE_LONE) {
        if (now >= g->next_announce_ms) {
            uint8_t body[22];
            body[0] = g->chipspec;
            body[1] = g->ncores;
            ELP_PUT_U16(body + 2, (uint16_t)(g->ram_kb & 0xFFFF));
            ELP_PUT_U16(body + 4, (uint16_t)(g->flash_mb & 0xFFFF));
            ELP_PUT_U32(body + 6, g->capacities);
            ELP_PUT_U32(body + 10, (uint32_t)g->score);
            ELP_PUT_U64(body + 14, g->boot_id);
            send_frame(g, NULL, ELP_HELLO, body, 22);
            g->next_announce_ms = now + 2000;
        }
        if (!g->election_busy && now >= g->election_pending_until) {
            election_start(g, "no cluster heard");
        }
    }

    /* election window close */
    if (g->election_busy && now >= g->election_until_ms) {
        if (g->election_lost) {
            g->election_busy = false;
            g->role = GL_ROLE_WORKER;
            memcpy(g->coord_id, g->election_best_id, 6);
            g->coord_since_ms = now;
            log_add(g, "lost election; following %02x%02x",
                    g->coord_id[4], g->coord_id[5]);
        } else {
            election_finish(g);
        }
    }

    if (g->role == GL_ROLE_COORD) {
        if (now >= g->next_announce_ms) {
            uint8_t body[26];
            memcpy(body, g->self_id, 6);
            ELP_PUT_U32(body + 6, (uint32_t)g->score);
            ELP_PUT_U64(body + 10, g->boot_id);
            ELP_PUT_U32(body + 18, machine_jobs_running(g));
            ELP_PUT_U32(body + 22, ++g->jobs_bump);
            send_frame(g, NULL, ELP_ANNOUNCE, body, 26);
            g->next_announce_ms = now + GL_ANNOUNCE_MS;
        }
        if (now >= g->next_tick_ms) {
            coord_job_poll(g);
            coord_self_work(g);      /* the brain does work too */
            g->next_tick_ms = now + GL_JOB_POLL_MS;
        }
    } else {
        /* worker: re-hello periodically so a lost first HELLO can't leave
         * us card-less in the coordinator's roster (heals `nodes` output) */
        if (now >= g->next_hello_ms) {
            uint8_t body[22];
            body[0] = g->chipspec;
            body[1] = g->ncores;
            ELP_PUT_U16(body + 2, (uint16_t)(g->ram_kb & 0xFFFF));
            ELP_PUT_U16(body + 4, (uint16_t)(g->flash_mb & 0xFFFF));
            ELP_PUT_U32(body + 6, g->capacities);
            ELP_PUT_U32(body + 10, (uint32_t)g->score);
            ELP_PUT_U64(body + 14, g->boot_id);
            send_frame(g, NULL, ELP_HELLO, body, 22);
            g->next_hello_ms = now + 5000;
        }
        /* worker: watchdog the brain (never re-fire inside an election) */
        if (g->role == GL_ROLE_WORKER && !g->election_busy &&
            !id_is_zero(g->coord_id) &&
            now - g->coord_since_ms > GL_COORD_TIMEOUT_MS) {
            election_start(g, "coordinator silent");
            g->next_pull_ms = now + GL_ELECTION_WINDOW_MS;
        }
        /* worker: pull work when idle */
        if (g->role == GL_ROLE_WORKER && !g->asg_active &&
            !id_is_zero(g->coord_id) && now >= g->next_pull_ms) {
            send_frame(g, g->coord_id, ELP_CHUNK_REQ, NULL, 0);
            g->next_pull_ms = now + GL_PULL_RETRY_MS;
        }
        /* dispatch deadline: the coordinator will reassign; just forget it */
        if (g->asg_active && now >= g->asg_deadline_ms) {
            log_add(g, "dispatch timed out; re-requesting");
            g->asg_active = false;
        }
    }
}

/* ===================== lifecycle ===================== */

void gl_init(gl_node_t *g, gl_transport_t *bus,
                 const uint8_t self_id[6],
                 uint8_t chipspec, uint8_t ncores,
                 uint32_t ram_kb, uint32_t flash_mb,
                 uint32_t capacities, int32_t score)
{
    memset(g, 0, sizeof(*g));
    g->bus = bus;
    memcpy(g->self_id, self_id, 6);
    g->chipspec = chipspec;
    g->ncores = ncores;
    g->ram_kb = ram_kb;
    g->flash_mb = flash_mb;
    g->capacities = capacities;
    g->score = score ? score : (int32_t)(ncores * 100 + ram_kb / 100 + flash_mb * 2);
    g->boot_id = boot_id_from(self_id, bus ? bus->now_ms : 0);
    g->role = GL_ROLE_LONE;
    g->election_pending_until = (bus ? bus->now_ms : 0) + GL_BOOT_QUIET_MS;
    log_add(g, "ESPlab node up: %02x%02x%02x%02x%02x%02x",
            self_id[0], self_id[1], self_id[2], self_id[3], self_id[4], self_id[5]);
    {
        uint8_t body[22];
        body[0] = chipspec;
        body[1] = ncores;
        ELP_PUT_U16(body + 2, (uint16_t)(ram_kb & 0xFFFF));
        ELP_PUT_U16(body + 4, (uint16_t)(flash_mb & 0xFFFF));
        ELP_PUT_U32(body + 6, capacities);
        ELP_PUT_U32(body + 10, (uint32_t)g->score);
        ELP_PUT_U64(body + 14, g->boot_id);
        send_frame(g, NULL, ELP_HELLO, body, 22);
    }
}

/* ===================== job submission ===================== */

int gl_submit_hello(gl_node_t *g, uint64_t *job_id)
{
    if (g->role != GL_ROLE_COORD) return -1;
    gl_job_t *j = job_alloc(g, GL_TASK_HELLO, "hello");
    if (!j) return -1;
    j->nchunks = roster_alive_n(g) + 1;      /* every core reports, incl. us */
    if (j->nchunks > GL_MAX_CHUNKS) j->nchunks = GL_MAX_CHUNKS;
    j->params[0] = 1;
    j->params[1] = 0;
    j->params[2] = j->nchunks;
    job_setup_chunks(j);
    if (job_id) *job_id = j->id;
    log_add(g, "job hello #%llu: %u chunks", (unsigned long long)j->id, j->nchunks);
    return 0;
}

int gl_submit_search(gl_node_t *g, uint32_t lo, uint32_t hi,
                     int32_t pred, uint64_t *job_id)
{
    if (g->role != GL_ROLE_COORD) return -1;
    if (hi <= lo) return -1;
    gl_job_t *j = job_alloc(g, GL_TASK_SEARCH, "search");
    if (!j) return -1;
    uint32_t cores = roster_alive_n(g) + 1;
    uint32_t want = cores * 4;
    if (want > GL_MAX_CHUNKS) want = GL_MAX_CHUNKS;
    uint32_t span = hi - lo;
    if (want > span) want = span;
    j->nchunks = want ? want : 1;
    j->params[0] = pred;
    j->params[1] = (int32_t)lo;
    j->params[2] = (int32_t)hi;
    job_setup_chunks(j);
    if (job_id) *job_id = j->id;
    log_add(g, "job search #%llu: %u-%u pred=%d, %u chunks",
            (unsigned long long)j->id, lo, hi, pred, j->nchunks);
    return 0;
}

int gl_submit_vm(gl_node_t *g, const uint8_t *code, uint16_t code_len,
                 const uint8_t *input, uint16_t input_len, uint32_t nchunks,
                 uint64_t *job_id)
{
    if (g->role != GL_ROLE_COORD) return -1;
    if (code_len > VM_CODE_MAX || input_len > GL_VM_INPUT_MAX) return -1;
    if (nchunks == 0 || nchunks > GL_MAX_CHUNKS) return -1;
    gl_job_t *j = job_alloc(g, GL_TASK_VM, "vm");
    if (!j) return -1;
    memcpy(j->vm_code, code, code_len);
    j->vm_code_len = code_len;
    memcpy(j->vm_input, input, input_len);
    j->vm_input_len = input_len;
    j->nchunks = nchunks;
    /* each chunk c processes input bytes [c*step, (c+1)*step) via LDV */
    j->params[1] = 0;
    j->params[2] = input_len ? input_len : 1;
    job_setup_chunks(j);
    if (job_id) *job_id = j->id;
    log_add(g, "job vm #%llu: %u chunks, %u code bytes",
            (unsigned long long)j->id, nchunks, code_len);
    return 0;
}

int gl_submit_frame(gl_node_t *g, const uint8_t *code, uint16_t code_len,
                    const uint8_t *input, uint16_t input_len, uint32_t nchunks,
                    uint64_t *job_id)
{
    if (g->role != GL_ROLE_COORD) return -1;
    if (code_len > VM_CODE_MAX || input_len > GL_VM_INPUT_MAX) return -1;
    if (nchunks == 0 || nchunks > GL_MAX_CHUNKS) return -1;
    gl_job_t *j = job_alloc(g, GL_TASK_FRAME, "frame");
    if (!j) return -1;
    memcpy(j->vm_code, code, code_len);
    j->vm_code_len = code_len;
    memcpy(j->vm_input, input, input_len);
    j->vm_input_len = input_len;
    j->nchunks = nchunks;
    /* chunk c covers pixel rows [c*rowsPerBand, ...) via the normal
     * [lo,hi) slicing: lo=0, hi=total pixels. params[1..2] drive it. */
    j->params[1] = 0;
    j->params[2] = input_len ? 3072 : 1;   /* 64x48; framePix rides in input */
    job_setup_chunks(j);
    if (job_id) *job_id = j->id;
    log_add(g, "job frame #%llu: %u chunks, %uB frame rle",
            (unsigned long long)j->id, nchunks, input_len);
    return 0;
}

int gl_job_find(gl_node_t *g, uint64_t job_id)
{
    for (uint8_t i = 0; i < g->jobs_n; i++)
        if (g->jobs[i].id == job_id) return i;
    return -1;
}

const char *gl_role_name(gl_role_t r)
{
    switch (r) {
    case GL_ROLE_COORD: return "COORD";
    case GL_ROLE_WORKER: return "worker";
    default: return "lone";
    }
}
