/* ESPlab console — the machine's front panel.
 * Runs identically on a board's serial line and on a virtual board's stdin.
 * The node you are plugged into IS the computer; these commands address the
 * machine, never "board #3" directly. */
#include "glados.h"
#include "vm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- tiny argv splitter (mutates the line) ---- */
static int split(char *line, char *argv[], int maxv)
{
    int n = 0;
    char *p = line;
    while (*p && n < maxv) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static bool id_eq_self(gl_node_t *g, const uint8_t id[6])
{
    return memcmp(id, g->self_id, 6) == 0;
}

uint32_t roster_alive_pub(gl_node_t *g)
{
    uint32_t n = 0;
    uint64_t now = g->bus ? g->bus->now_ms : 0;
    for (int i = 0; i < g->roster_n; i++)
        if (memcmp(g->roster[i].id, g->self_id, 6) != 0 &&
            g->roster[i].last_seen_ms &&
            now - g->roster[i].last_seen_ms <= GL_NODE_TIMEOUT_MS)
            n++;
    return n;
}

static bool id_is_zero(const uint8_t a[6])
{
    static const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
    return memcmp(a, z, 6) == 0;
}

/* forward a SUBMIT frame to the coordinator (console.c side; the core's
 * send_frame is private, so console.c wraps the transport directly) */
static void send_frame_pub(gl_node_t *g, const uint8_t dst[6], uint8_t type,
                           const uint8_t *body, size_t len)
{
    uint8_t buf[ELP_MAX_PAYLOAD];
    if (len > ELP_MAX_PAYLOAD - GL_FRAME_HEADER || !g->bus) return;
    buf[0] = ELP_VERSION;
    buf[1] = type;
    memcpy(buf + 2, g->self_id, 6);
    buf[8] = (uint8_t)(len & 0xFF);
    buf[9] = (uint8_t)((len >> 8) & 0xFF);
    if (len) memcpy(buf + GL_FRAME_HEADER, body, len);
    g->bus->send(g->bus, dst, buf, len + GL_FRAME_HEADER);
}

static void print_nodes(gl_node_t *g, gl_print_fn out, void *ctx)
{
    char line[128];
    uint64_t now = g->bus ? g->bus->now_ms : 0;
    out(ctx, "id                chip   cores  ram(k) flash(mb) state");
    for (int i = 0; i < g->roster_n; i++) {
        elp_node_t *n = &g->roster[i];
        bool self = id_eq_self(g, n->id);
        bool alive = self || (n->last_seen_ms &&
                              now - n->last_seen_ms <= GL_NODE_TIMEOUT_MS);
        static const char *chips[] = {"?", "classic", "s2", "s3", "c3", "host"};
        const char *chip = (n->chipspec < 6) ? chips[n->chipspec] : "?";
        uint32_t ram = self ? g->ram_kb
                            : (uint32_t)n->ram_kb_x16 * 16 + n->ram_kb_rem;
        uint32_t fl = self ? g->flash_mb
                           : (uint32_t)n->flash_mb_x16 * 16 + n->flash_mb_rem;
        snprintf(line, sizeof(line), "%02x%02x%02x%02x%02x%02x  %-7s  %-5u  %-6lu  %-5lu  %s%s",
                 n->id[0], n->id[1], n->id[2], n->id[3], n->id[4], n->id[5],
                 chip, (unsigned)(self ? g->ncores : n->ncores),
                 (unsigned long)ram, (unsigned long)fl, alive ? "up" : "DOWN",
                 self ? " (me)" : "");
        out(ctx, line);
    }
}

static void print_jobs(gl_node_t *g, gl_print_fn out, void *ctx)
{
    char line[128];
    if (g->role == GL_ROLE_COORD) {
        if (g->jobs_n == 0) { out(ctx, "no jobs"); return; }
        for (int i = 0; i < g->jobs_n; i++) {
            gl_job_t *j = &g->jobs[i];
            static const char *st[] = {"running", "DONE", "FAILED"};
            static const char *kd[] = {"?", "hello", "search", "vm", "frame"};
            snprintf(line, sizeof(line), "#%llu %-8s %-7s %s  %lu/%lu chunks agg=%lu",
                     (unsigned long long)j->id, j->name, kd[j->kind & 7],
                     st[j->state & 3], (unsigned long)j->done_count,
                     (unsigned long)j->nchunks, (unsigned long)j->aggregate);
            out(ctx, line);
        }
        return;
    }
    /* worker: show the machine's job list as announced by the brain */
    snprintf(line, sizeof(line), "machine: %lu job(s) running",
             (unsigned long)g->seen_jobs_running);
    out(ctx, line);
}

void gl_console(gl_node_t *g, const char *input, gl_print_fn out, void *ctx)
{
    /* `submit vm` carries code+input as hex: up to ~1200 chars. The console
     * buffer must hold a full command, not just a human's fingers. */
    static char buf[6000];   /* frame submits (esp on demand) carry ~2.6KB hex */
    static char *argv[10];
    snprintf(buf, sizeof(buf), "%s", input ? input : "");
    int n = split(buf, argv, 10);
    char line[160];
    if (n == 0) return;

    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
        out(ctx, "ESPlab commands:");
        out(ctx, "  status              machine overview");
        out(ctx, "  nodes               boards in this machine");
        out(ctx, "  jobs                job list");
        out(ctx, "  job <id>            chunk detail for one job");
        out(ctx, "  submit hello        every core reports in");
        out(ctx, "  submit search <lo> <hi> <pred>");
        out(ctx, "                      count multiples of pred in [lo,hi)");
        out(ctx, "  put <key> <text>    store on the machine's disk");
        out(ctx, "  get <key>           read from the machine's disk");
        out(ctx, "  ls                  list the machine's disk");
        out(ctx, "  log                 recent machine log");
        out(ctx, "  id                  this console's node id");
        return;
    }
    if (!strcmp(argv[0], "id")) {
        snprintf(line, sizeof(line), "%02x%02x%02x%02x%02x%02x",
                 g->self_id[0], g->self_id[1], g->self_id[2],
                 g->self_id[3], g->self_id[4], g->self_id[5]);
        out(ctx, line);
        return;
    }
    if (!strcmp(argv[0], "status")) {
        snprintf(line, sizeof(line),
                 "role=%s  coord=%02x%02x%02x%02x%02x%02x  boards_up=%lu  rx=%lu tx=%lu drops=%lu",
                 gl_role_name(g->role),
                 g->coord_id[0], g->coord_id[1], g->coord_id[2],
                 g->coord_id[3], g->coord_id[4], g->coord_id[5],
                 (unsigned long)(roster_alive_pub(g) + 1),
                 (unsigned long)g->stat_rx, (unsigned long)g->stat_tx,
                 (unsigned long)g->stat_drops);
        out(ctx, line);
        print_jobs(g, out, ctx);
        return;
    }
    if (!strcmp(argv[0], "nodes")) { print_nodes(g, out, ctx); return; }
    if (!strcmp(argv[0], "jobs")) { print_jobs(g, out, ctx); return; }
    if (!strcmp(argv[0], "log")) {
        for (uint8_t i = 0; i < g->log_n; i++) {
            uint8_t idx = (uint8_t)((g->log_head - g->log_n + i + 8) & 7);
            out(ctx, g->log[idx]);
        }
        return;
    }
    if (!strcmp(argv[0], "job") && n >= 2) {
        uint64_t id = strtoull(argv[1], NULL, 0);
        int ji = gl_job_find(g, id);
        if (ji < 0) {
            out(ctx, (g->role == GL_ROLE_COORD)
                     ? "no such job" : "job details live on the coordinator board");
            return;
        }
        gl_job_t *j = &g->jobs[ji];
        {
            uint64_t dur = (j->finished_ms ? j->finished_ms : g->bus->now_ms)
                           - j->started_ms;
            snprintf(line, sizeof(line), "  took %lu ms", (unsigned long)dur);
            out(ctx, line);
        }
        for (uint32_t c = 0; c < j->nchunks && c < 16; c++) {
            static const char *cs[] = {"pending", "assigned", "done", "failed"};
            snprintf(line, sizeof(line), "  chunk %-2lu [%lu,%lu)  %s",
                     (unsigned long)c, (unsigned long)j->chunks[c].lo,
                     (unsigned long)j->chunks[c].hi,
                     cs[j->chunks[c].state & 3]);
            out(ctx, line);
        }
        if (j->nchunks > 16) out(ctx, "  ...");
        if (j->kind == GL_TASK_VM) {
            uint32_t total = 0;
            for (uint32_t c = 0; c < j->nchunks; c++) total += j->chunks[c].out_len;
            snprintf(line, sizeof(line), "  output: %lu bytes (binary, kept per chunk)",
                     (unsigned long)total);
            out(ctx, line);
        }
        return;
    }
    if (!strcmp(argv[0], "jobdump") && n >= 2) {
        /* ESP On Demand: raw per-chunk output, hex lines, in chunk order.
         * The web service concatenates; the browser renders pixels. */
        uint64_t id = strtoull(argv[1], NULL, 0);
        int ji = gl_job_find(g, id);
        if (ji < 0) { out(ctx, "no such job"); return; }
        gl_job_t *j = &g->jobs[ji];
        for (uint32_t c = 0; c < j->nchunks; c++) {
            if (j->chunks[c].state != 2 || j->chunks[c].out_len == 0) {
                snprintf(line, sizeof(line), "chunk %lu incomplete", (unsigned long)c);
                out(ctx, line);
                continue;
            }
            for (uint16_t i = 0; i < j->chunks[c].out_len; i += 32) {
                char hex[2 * 32 + 1];
                size_t o = 0;
                uint16_t end = (uint16_t)(i + 32);
                if (end > j->chunks[c].out_len) end = j->chunks[c].out_len;
                for (uint16_t k = i; k < end; k++) {
                    hex[o++] = "0123456789abcdef"[j->chunks[c].out[k] >> 4];
                    hex[o++] = "0123456789abcdef"[j->chunks[c].out[k] & 15];
                }
                hex[o] = 0;
                snprintf(line, sizeof(line), "chunk %lu %s", (unsigned long)c, hex);
                out(ctx, line);
            }
        }
        return;
    }
    if (!strcmp(argv[0], "submit")) {
        if (g->role != GL_ROLE_COORD) {
            /* one console is as good as any other: forward to the brain */
            if (id_is_zero(g->coord_id)) { out(ctx, "no coordinator yet"); return; }
            if (n >= 5 && !strcmp(argv[1], "search")) {
                uint8_t body[17];
                body[0] = (uint8_t)GL_TASK_SEARCH;
                ELP_PUT_U32(body + 1, (uint32_t)strtol(argv[4], NULL, 0));  /* pred */
                ELP_PUT_U32(body + 5, (uint32_t)strtoul(argv[2], NULL, 0)); /* lo */
                ELP_PUT_U32(body + 9, (uint32_t)strtoul(argv[3], NULL, 0)); /* hi */
                ELP_PUT_U32(body + 13, 0);
                send_frame_pub(g, g->coord_id, ELP_SUBMIT, body, 17);
                out(ctx, "submitted to the coordinator");
            } else if (n >= 2 && !strcmp(argv[1], "hello")) {
                uint8_t body[17];
                body[0] = (uint8_t)GL_TASK_HELLO;
                memset(body + 1, 0, 16);
                send_frame_pub(g, g->coord_id, ELP_SUBMIT, body, 17);
                out(ctx, "submitted to the coordinator");
            } else if (n >= 5 && !strcmp(argv[1], "vm")) {
                /* forward a full vm submit: kind(1) nchunks(4) codeLen(2)
                 * inputLen(2) code input — the coordinator parses it */
                size_t cl = strlen(argv[2]) / 2, il = strlen(argv[3]) / 2;
                uint32_t nch = (uint32_t)strtoul(argv[4], NULL, 0);
                if (cl == 0 || cl > VM_CODE_MAX || il > GL_VM_INPUT_MAX || nch == 0) {
                    out(ctx, "bad vm submit");
                    return;
                }
                static uint8_t body[400];
                size_t p = 0;
                body[p++] = (uint8_t)GL_TASK_VM;
                ELP_PUT_U32(body + p, nch); p += 4;
                ELP_PUT_U16(body + p, (uint16_t)cl); p += 2;
                ELP_PUT_U16(body + p, (uint16_t)il); p += 2;
                for (size_t i = 0; i < cl; i++) {
                    char hb[3] = {argv[2][i*2], argv[2][i*2+1], 0};
                    body[p++] = (uint8_t)strtoul(hb, NULL, 16);
                }
                for (size_t i = 0; i < il; i++) {
                    char hb[3] = {argv[3][i*2], argv[3][i*2+1], 0};
                    body[p++] = (uint8_t)strtoul(hb, NULL, 16);
                }
                gl_send_frame(g, g->coord_id, ELP_SUBMIT, body, p);
                out(ctx, "submitted to the coordinator");
            } else if (n >= 5 && !strcmp(argv[1], "frame")) {
                /* forward a frame submit: kind(1) nchunks(4) codeLen(2)
                 * inputLen(2) code input — GL_TASK_FRAME on the wire */
                size_t cl = strlen(argv[2]) / 2, il = strlen(argv[3]) / 2;
                uint32_t nch = (uint32_t)strtoul(argv[4], NULL, 0);
                if (cl == 0 || cl > VM_CODE_MAX || il > GL_VM_INPUT_MAX || nch == 0) {
                    out(ctx, "bad frame submit");
                    return;
                }
                static uint8_t fbody[1100];
                size_t fp = 0;
                fbody[fp++] = (uint8_t)GL_TASK_FRAME;
                ELP_PUT_U32(fbody + fp, nch); fp += 4;
                ELP_PUT_U16(fbody + fp, (uint16_t)cl); fp += 2;
                ELP_PUT_U16(fbody + fp, (uint16_t)il); fp += 2;
                for (size_t i = 0; i < cl; i++) {
                    char hb[3] = {argv[2][i*2], argv[2][i*2+1], 0};
                    fbody[fp++] = (uint8_t)strtoul(hb, NULL, 16);
                }
                for (size_t i = 0; i < il; i++) {
                    char hb[3] = {argv[3][i*2], argv[3][i*2+1], 0};
                    fbody[fp++] = (uint8_t)strtoul(hb, NULL, 16);
                }
                gl_send_frame(g, g->coord_id, ELP_SUBMIT, fbody, fp);
                out(ctx, "submitted to the coordinator");
            } else {
                out(ctx, "workers forward: submit hello | submit search <lo> <hi> <pred>");
            }
            return;
        }
        /* coordinator-side submits */
        if (n >= 2 && !strcmp(argv[1], "hello")) {
            uint64_t id;
            if (gl_submit_hello(g, &id) == 0) {
                snprintf(line, sizeof(line), "submitted hello job #%llu",
                         (unsigned long long)id);
                out(ctx, line);
            } else out(ctx, "submit failed");
            return;
        }
        if (n >= 5 && !strcmp(argv[1], "search")) {
            uint32_t lo = (uint32_t)strtoul(argv[2], NULL, 0);
            uint32_t hi = (uint32_t)strtoul(argv[3], NULL, 0);
            int32_t pred = (int32_t)strtol(argv[4], NULL, 0);
            uint64_t id;
            if (gl_submit_search(g, lo, hi, pred, &id) == 0) {
                snprintf(line, sizeof(line), "submitted search job #%llu (%lu-%lu, mult of %ld)",
                         (unsigned long long)id, (unsigned long)lo,
                         (unsigned long)hi, (long)pred);
                out(ctx, line);
            } else out(ctx, "submit failed (range empty?)");
            return;
        }
        if (n >= 5 && !strcmp(argv[1], "vm")) {
            /* submit vm <hexcode> <hexinput> <nchunks> — the task pipeline */
            size_t cl = strlen(argv[2]) / 2, il = strlen(argv[3]) / 2;
            uint32_t nchunks = (uint32_t)strtoul(argv[4], NULL, 0);
            static uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX];
            if (cl == 0 || cl > sizeof(code) || il > sizeof(input) || nchunks == 0) {
                out(ctx, "bad vm submit (code/input size or chunk count)");
                return;
            }
            for (size_t i = 0; i < cl; i++) {
                char b[3] = {argv[2][i * 2], argv[2][i * 2 + 1], 0};
                code[i] = (uint8_t)strtoul(b, NULL, 16);
            }
            for (size_t i = 0; i < il; i++) {
                char b[3] = {argv[3][i * 2], argv[3][i * 2 + 1], 0};
                input[i] = (uint8_t)strtoul(b, NULL, 16);
            }
            uint64_t id;
            if (gl_submit_vm(g, code, (uint16_t)cl, input, (uint16_t)il,
                             nchunks, &id) == 0) {
                snprintf(line, sizeof(line), "submitted vm job #%llu (%uB code, %uB input, %u chunks)",
                         (unsigned long long)id, (unsigned)cl, (unsigned)il,
                         (unsigned)nchunks);
                out(ctx, line);
            } else out(ctx, "submit failed (no coordinator? code/input limits?)");
            return;
        }
        if (n >= 5 && !strcmp(argv[1], "frame")) {
            /* submit frame <hexcode> <hexframe> <nchunks> — ESP On Demand */
            size_t cl = strlen(argv[2]) / 2, il = strlen(argv[3]) / 2;
            uint32_t nchunks = (uint32_t)strtoul(argv[4], NULL, 0);
            static uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX];
            if (cl == 0 || cl > sizeof(code) || il > sizeof(input) || nchunks == 0) {
                out(ctx, "bad frame submit (code/frame size or chunk count)");
                return;
            }
            for (size_t i = 0; i < cl; i++) {
                char b[3] = {argv[2][i * 2], argv[2][i * 2 + 1], 0};
                code[i] = (uint8_t)strtoul(b, NULL, 16);
            }
            for (size_t i = 0; i < il; i++) {
                char b[3] = {argv[3][i * 2], argv[3][i * 2 + 1], 0};
                input[i] = (uint8_t)strtoul(b, NULL, 16);
            }
            uint64_t id;
            if (gl_submit_frame(g, code, (uint16_t)cl, input, (uint16_t)il,
                                nchunks, &id) == 0) {
                snprintf(line, sizeof(line), "submitted frame job #%llu (%uB code, %uB rle, %u chunks)",
                         (unsigned long long)id, (unsigned)cl, (unsigned)il,
                         (unsigned)nchunks);
                out(ctx, line);
            } else out(ctx, "submit failed (no coordinator? code/frame limits?)");
            return;
        }
        out(ctx, "usage: submit hello | submit search <lo> <hi> <pred> | submit vm <hexcode> <hexinput> <nchunks> | submit frame <hexcode> <hexframe> <nchunks>");
        return;
    }
    if (!strcmp(argv[0], "put") && n >= 3) {
        /* join argv[2..] with spaces as the value */
        char val[GLADOS_KV_VAL_MAX];
        size_t o = 0;
        for (int i = 2; i < n; i++) {
            size_t l = strlen(argv[i]);
            if (o + l + 2 >= sizeof(val)) break;
            if (i > 2) val[o++] = ' ';
            memcpy(val + o, argv[i], l);
            o += l;
        }
        val[o] = 0;
        if (gl_kv_put(g, argv[1], (uint8_t *)val, (uint16_t)o) == 0) {
            out(ctx, "stored");
        } else out(ctx, "put failed (too big? disk full?)");
        return;
    }
    if (!strcmp(argv[0], "get") && n >= 2) {
        uint8_t val[GLADOS_KV_VAL_MAX];
        uint16_t vl = sizeof(val);
        if (gl_kv_get(g, argv[1], val, &vl) == 0) {
            snprintf(line, sizeof(line), "%s = %.*s", argv[1], (int)vl, (char *)val);
            out(ctx, line);
        } else out(ctx, "not found");
        return;
    }
    if (!strcmp(argv[0], "ls")) {
        if (g->kv_n == 0) { out(ctx, "(empty)"); return; }
        for (uint8_t i = 0; i < g->kv_n; i++) {
            snprintf(line, sizeof(line), "%s (%lu bytes)", g->kv[i].key,
                     (unsigned long)g->kv[i].val_len);
            out(ctx, line);
        }
        return;
    }
    out(ctx, "unknown command; try 'help'");
}
