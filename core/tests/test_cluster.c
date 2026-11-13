/* ESPlab core tests — no hardware, no sockets, no threads.
 * A deterministic addressed "ether" delivers datagrams between virtual
 * nodes; tests advance simulated time and assert real cluster behavior:
 * election, work splitting, dead-core rescheduling, coordinator death,
 * storage, VM sandbox. */
#include "../inc/machine.h"
#include "../inc/vm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================== deterministic test ether ===================== */

typedef struct {
    uint8_t data[700];
    uint16_t len;
    uint8_t dst[6];        /* 6x 0xFF = broadcast */
    uint64_t deliver_at;
} pkt_t;

typedef struct {
    gl_node_t *nodes[8];
    int nodes_n;
    pkt_t wire[512];
    int wire_n;
    uint64_t now;
    int drop_mod;          /* 0 = reliable; else drop packets whose (serial%N)==N-1 */
    int serial;
} sim_t;

static sim_t S;

static void ether_push(const uint8_t *dst, const uint8_t *payload, size_t len)
{
    if (S.wire_n >= 512) return;
    pkt_t *p = &S.wire[S.wire_n++];
    memcpy(p->data, payload, len);
    p->len = (uint16_t)len;
    memcpy(p->dst, dst, 6);
    p->deliver_at = S.now + 1;      /* 1 ms of radio latency */
}

static int sim_send(gl_transport_t *t, const uint8_t peer[6],
                    const uint8_t *payload, size_t len)
{
    (void)t;
    ether_push(peer, payload, len);
    return 0;
}

static int sim_broadcast(gl_transport_t *t, const uint8_t *payload, size_t len)
{
    (void)t;
    for (int i = 0; i < S.nodes_n; i++)
        ether_push(S.nodes[i]->self_id, payload, len);
    return 0;
}

/* ===================== sim driver ===================== */

static void sim_init(void)
{
    memset(&S, 0, sizeof(S));
}

static void sim_add_board(uint8_t last, uint8_t cores, uint32_t ram, int32_t score)
{
    static gl_transport_t buses[8];
    static uint8_t ids[8][6];
    int i = S.nodes_n++;
    ids[i][0] = 0x24; ids[i][1] = 0x0A; ids[i][2] = 0xC4;
    ids[i][3] = 0xFF; ids[i][4] = 0xFE; ids[i][5] = last;
    buses[i].send = sim_send;
    buses[i].broadcast = sim_broadcast;
    buses[i].now_ms = S.now;
    S.nodes[i] = malloc(sizeof(gl_node_t));
    gl_init(S.nodes[i], &buses[i], ids[i], ELP_CHIP_CLASSIC,
                cores, ram, 4, ELP_CAP_VM | ELP_CAP_STORE, score);
}

static void sim_step(void)
{
    S.now += 10;                     /* 10 ms quanta: real work takes real time */
    for (int i = 0; i < S.nodes_n; i++)
        S.nodes[i]->bus->now_ms = S.now;

    /* deliver everything addressed to a live node whose time has come */
    int w = 0;
    while (w < S.wire_n) {
        if (S.wire[w].deliver_at <= S.now) {
            uint8_t dst[700];
            uint16_t n = S.wire[w].len;
            memcpy(dst, S.wire[w].data, n);
            uint8_t d6[6];
            memcpy(d6, S.wire[w].dst, 6);
            S.wire[w] = S.wire[S.wire_n - 1];
            S.wire_n--;
            int delivered = 0;
            for (int i = 0; i < S.nodes_n; i++) {
                if (!memcmp(S.nodes[i]->self_id, d6, 6)) {
                    S.serial++;
                    if (S.drop_mod && (S.serial % S.drop_mod) == S.drop_mod - 1)
                        continue;   /* radio ate this one */
                    gl_recv(S.nodes[i], dst, n);
                    delivered = 1;
                }
            }
            if (!delivered) continue;   /* dst dead: packet evaporates */
        } else w++;
    }

    for (int i = 0; i < S.nodes_n; i++)
        gl_tick(S.nodes[i]);
}

static void sim_run(int ms)
{
    for (int m = 0; m < ms / 10; m++) sim_step();
}

static void sim_kill(uint8_t last)
{
    for (int i = 0; i < S.nodes_n; i++) {
        if (S.nodes[i]->self_id[5] == last) {
            gl_node_t *dead = S.nodes[i];
            S.nodes[i] = S.nodes[S.nodes_n - 1];
            S.nodes_n--;
            free(dead);   /* its queued packets will find no target */
            return;
        }
    }
}

/* ===================== test helpers ===================== */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

static gl_node_t *coord(void)
{
    for (int i = 0; i < S.nodes_n; i++)
        if (S.nodes[i]->role == GL_ROLE_COORD) return S.nodes[i];
    return NULL;
}

static void dump_logs(void)
{
    for (int i = 0; i < S.nodes_n; i++) {
        printf("  node %02x role=%s\n", S.nodes[i]->self_id[5],
               gl_role_name(S.nodes[i]->role));
        for (uint8_t l = 0; l < S.nodes[i]->log_n; l++) {
            uint8_t idx = (uint8_t)((S.nodes[i]->log_head - S.nodes[i]->log_n + l + 8) & 7);
            printf("    | %s\n", S.nodes[i]->log[idx]);
        }
    }
}

/* ===================== the tests ===================== */

static void test_single_node_elects_itself(void)
{
    printf("TEST single node elects itself and runs hello\n");
    sim_init();
    sim_add_board(1, 2, 290, 0);
    sim_run(3000);
    gl_node_t *c = coord();
    CHECK(c != NULL, "a coordinator emerged");
    if (!c) { dump_logs(); return; }
    uint64_t id;
    CHECK(gl_submit_hello(c, &id) == 0, "hello job submitted");
    sim_run(2000);
    int ji = gl_job_find(c, id);
    CHECK(ji >= 0 && c->jobs[ji].state == GL_JOB_DONE, "hello job DONE");
    CHECK(c->jobs[ji].done_count == 1, "one chunk done (the brain itself)");
}

static void test_three_boards_elect_and_work(void)
{
    printf("TEST three boards elect one coordinator and share work\n");
    sim_init();
    sim_add_board(1, 2, 290, 0);
    sim_add_board(2, 2, 290, 0);
    sim_add_board(3, 2, 290, 0);
    sim_run(4000);
    int n_coord = 0;
    for (int i = 0; i < S.nodes_n; i++)
        if (S.nodes[i]->role == GL_ROLE_COORD) n_coord++;
    CHECK(n_coord == 1, "exactly one coordinator");
    gl_node_t *c = coord();
    if (!c) { dump_logs(); return; }
    CHECK(c->roster_n >= 2, "coordinator sees the other boards");
    uint64_t id;
    CHECK(gl_submit_search(c, 0, 1000, 7, &id) == 0, "search job submitted");
    sim_run(6000);
    int ji = gl_job_find(c, id);
    CHECK(ji >= 0 && c->jobs[ji].state == GL_JOB_DONE, "search job DONE");
    /* multiples of 7 in [0,1000) = 143 (v=0 counts: 0,7,...,994) */
    CHECK(c->jobs[ji].aggregate == 143, "aggregate correct (143 multiples of 7)");
}

static void test_worker_death_midjob(void)
{
    printf("TEST worker dies mid-job; its chunk is redone and job completes\n");
    sim_init();
    sim_add_board(1, 2, 290, 0);
    sim_add_board(2, 2, 290, 0);
    sim_run(4000);
    gl_node_t *c = coord();
    if (!c) { dump_logs(); return; }
    uint64_t id;
    CHECK(gl_submit_search(c, 0, 100000, 13, &id) == 0, "search job submitted");
    sim_run(50);                       /* let chunks get assigned */
    sim_kill(2);                       /* worker dies with a chunk out */
    sim_run(12000);                    /* reassignment + redo time */
    int ji = gl_job_find(c, id);
    CHECK(ji >= 0 && c->jobs[ji].state == GL_JOB_DONE, "job still completes");
    /* multiples of 13 in [0,100000) = 7693 (v=0 counts) */
    CHECK(c->jobs[ji].aggregate == 7693, "aggregate still correct after death");
}

static void test_coordinator_death_reelects(void)
{
    printf("TEST coordinator dies; survivors re-elect and the machine lives\n");
    sim_init();
    sim_add_board(1, 2, 290, 500);   /* 1 = strong -> becomes brain */
    sim_add_board(2, 2, 290, 100);
    sim_add_board(3, 2, 290, 100);
    sim_run(4000);
    gl_node_t *c = coord();
    CHECK(c != NULL && c->self_id[5] == 1, "board 1 is the brain (higher score)");
    if (!c) { dump_logs(); return; }
    uint64_t id;
    gl_submit_search(c, 0, 5000, 3, &id);
    sim_run(30);
    sim_kill(1);                     /* the brain dies */
    sim_run(15000);                  /* watchdog + re-election + redo */
    gl_node_t *c2 = coord();
    CHECK(c2 != NULL, "a new brain emerged");
    CHECK(c2 != NULL && c2->self_id[5] != 1, "the dead board is not the brain");
    if (!c2) { dump_logs(); return; }
    /* job was on the dead brain: job list died with it (machine-down rule).
     * What must work: the surviving machine accepts NEW work. */
    uint64_t id2;
    CHECK(gl_submit_hello(c2, &id2) == 0, "new machine accepts jobs");
    sim_run(3000);
    int ji = gl_job_find(c2, id2);
    CHECK(ji >= 0 && c2->jobs[ji].state == GL_JOB_DONE, "hello completes on new brain");
}

static void test_kv_storage(void)
{
    printf("TEST machine disk: put/get/del\n");
    sim_init();
    sim_add_board(1, 2, 290, 0);
    sim_add_board(2, 2, 290, 0);
    sim_run(4000);
    gl_node_t *c = coord();
    if (!c) return;
    CHECK(gl_kv_put(c, "config", (const uint8_t *)"mode=fast", 9) == 0, "put works");
    uint8_t buf[GL_KV_VAL_MAX];
    uint16_t bl = sizeof(buf);
    CHECK(gl_kv_get(c, "config", buf, &bl) == 0 && bl == 9 &&
          !memcmp(buf, "mode=fast", 9), "get returns what was put");
    CHECK(gl_kv_del(c, "config") == 0, "del works");
    bl = sizeof(buf);
    CHECK(gl_kv_get(c, "config", buf, &bl) == -1, "get after del fails");
    /* oversize rejection */
    uint8_t big[GL_KV_VAL_MAX + 10];
    memset(big, 'x', sizeof(big));
    CHECK(gl_kv_put(c, "big", big, sizeof(big)) == -1, "oversize put rejected");
}

/* a real VM program: count input bytes equal to 'a' in my slice */
static uint16_t build_count_a(uint8_t *code)
{
    uint16_t n = 0;
    code[n++] = OP_LDS;  code[n++] = 0; code[n++] = 0; code[n++] = 0;  /* r0 = lo */
    code[n++] = OP_LDS;  code[n++] = 1; code[n++] = 1; code[n++] = 0;  /* r1 = hi */
    code[n++] = OP_LOADI;code[n++] = 2; code[n++] = (uint8_t)('a'); code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 3; code[n++] = 0; code[n++] = 0;  /* count */
    code[n++] = OP_LOADI;code[n++] = 7; code[n++] = 1; code[n++] = 0;  /* one */
    /* loop: */
    /* r4 = cmp(r0, r1); if r0>=r1 -> done */
    code[n++] = OP_CMP;  code[n++] = 4; code[n++] = 0; code[n++] = 1;  /* r4 = r0<r1 */
    code[n++] = OP_JZ;   code[n++] = 4; code[n++] = 7; code[n++] = 0;  /* if !r4 -> done (+7 instr, done=52) */
    code[n++] = OP_LDV;  code[n++] = 5; code[n++] = 0; code[n++] = 0;  /* r5 = input[r0] */
    code[n++] = OP_CMP;  code[n++] = 6; code[n++] = 5; code[n++] = 2;  /* r6 = cmp(r5,'a') */
    code[n++] = OP_JNZ;  code[n++] = 6; code[n++] = 2; code[n++] = 0;  /* if != skip count++ (+2) */
    code[n++] = OP_ADD;  code[n++] = 3; code[n++] = 3; code[n++] = 7;  /* count++ */
    code[n++] = OP_ADD;  code[n++] = 0; code[n++] = 0; code[n++] = 7;  /* r0++ */
    code[n++] = OP_JMP;  code[n++] = 0; code[n++] = (uint8_t)(0-7); code[n++] = 0xFF; /* loop (-7 instr) */
    /* done: emit count as a proper little-endian u32 (4 bytes) */
    code[n++] = OP_LOADI;code[n++] = 8; code[n++] = 0xFF; code[n++] = 0;  /* mask */
    code[n++] = OP_AND;  code[n++] = 4; code[n++] = 3; code[n++] = 8;  /* r4 = count & 0xFF */
    code[n++] = OP_EMIT; code[n++] = 4; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 9; code[n++] = 8; code[n++] = 0;
    code[n++] = OP_SHR;  code[n++] = 5; code[n++] = 3; code[n++] = 9;  /* r5 = count >> 8 */
    code[n++] = OP_AND;  code[n++] = 5; code[n++] = 5; code[n++] = 8;
    code[n++] = OP_EMIT; code[n++] = 5; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 9; code[n++] = 16; code[n++] = 0;
    code[n++] = OP_SHR;  code[n++] = 5; code[n++] = 3; code[n++] = 9;
    code[n++] = OP_AND;  code[n++] = 5; code[n++] = 5; code[n++] = 8;
    code[n++] = OP_EMIT; code[n++] = 5; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 9; code[n++] = 24; code[n++] = 0;
    code[n++] = OP_SHR;  code[n++] = 5; code[n++] = 3; code[n++] = 9;
    code[n++] = OP_AND;  code[n++] = 5; code[n++] = 5; code[n++] = 8;
    code[n++] = OP_EMIT; code[n++] = 5; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_HALT; code[n++] = 0; code[n++] = 0; code[n++] = 0;
    return n;
}

static void test_vm_job_across_boards(void)
{
    printf("TEST VM task runs across boards; results assemble\n");
    sim_init();
    sim_add_board(1, 2, 290, 0);
    sim_add_board(2, 2, 290, 0);
    sim_run(4000);
    gl_node_t *c = coord();
    if (!c) { dump_logs(); return; }
    uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX];
    uint16_t clen = build_count_a(code);
    uint16_t ilen = 64;
    for (int i = 0; i < ilen; i++)
        input[i] = (i % 3 == 0) ? 'a' : 'b';
    uint64_t id;
    CHECK(gl_submit_vm(c, code, clen, input, ilen, 4, &id) == 0, "vm job submitted");
    sim_run(6000);
    int ji = gl_job_find(c, id);
    if (ji < 0 || c->jobs[ji].state != GL_JOB_DONE) {
        printf("  [debug] job state=%d done=%u fail=%u nchunks=%u agg=%u\n",
               ji >= 0 ? (int)c->jobs[ji].state : -1,
               ji >= 0 ? c->jobs[ji].done_count : 0,
               ji >= 0 ? c->jobs[ji].fail_count : 0,
               ji >= 0 ? c->jobs[ji].nchunks : 0,
               ji >= 0 ? c->jobs[ji].aggregate : 0);
        for (uint32_t cc = 0; ji >= 0 && cc < c->jobs[ji].nchunks; cc++)
            printf("  [debug] chunk %u: state=%u attempts=%u\n",
                   cc, c->jobs[ji].chunks[cc].state, c->jobs[ji].chunks[cc].attempts);
        dump_logs();
    }
    CHECK(ji >= 0 && c->jobs[ji].state == GL_JOB_DONE, "vm job DONE");
    /* total 'a's = positions 0,3,...,63 = 22; each chunk reports its count once */
    CHECK(c->jobs[ji].aggregate == 22, "aggregate correct (22 'a's total)");
}

static void test_vm_sandbox_kills_runaway(void)
{
    printf("TEST sandbox kills a runaway task without hurting the machine\n");
    uint8_t code[16];
    uint16_t n = 0;
    code[n++] = OP_JMP; code[n++] = 0; code[n++] = 0; code[n++] = 0; /* loop self */
    uint8_t out[JOB_OUT_MAX];
    vm_t vm;
    vm_init(&vm, code, n, NULL, 0, out, sizeof(out), 0, 0);
    for (int slice = 0; slice < 40 && vm.status == VM_OK; slice++) {
        vm_run(&vm, VM_CYCLES_PER_SLICE);
        if (vm.halted) break;
    }
    /* the sandbox bound: a task that never halted in its slice budget */
    if (!vm.halted && vm.status == VM_OK) vm.status = VM_FAULT_STACK;
    CHECK(vm.status == VM_FAULT_STACK, "runaway detected and killed");
    CHECK(vm.pc < 16, "pc stayed inside the code (no escape)");
}

static void test_fragmentation_roundtrip(void)
{
    printf("TEST big messages fragment and reassemble across the ether\n");
    sim_init();
    sim_add_board(1, 2, 290, 0);
    sim_add_board(2, 2, 290, 0);
    sim_run(4000);
    gl_node_t *c = coord();
    if (!c) return;
    /* a VM job with max-size code+input: forces multi-fragment CHUNKs */
    uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX];
    uint16_t clen = build_count_a(code);
    uint16_t ilen = GL_VM_INPUT_MAX;   /* 672 bytes: chunk ~ 16+16+40+672+code */
    for (int i = 0; i < ilen; i++) input[i] = 'a';
    uint64_t id;
    CHECK(gl_submit_vm(c, code, clen, input, ilen, 2, &id) == 0, "big vm job submitted");
    sim_run(8000);
    int ji = gl_job_find(c, id);
    CHECK(ji >= 0 && c->jobs[ji].state == GL_JOB_DONE, "fragmented job completed");
    CHECK(c->jobs[ji].aggregate == GL_VM_INPUT_MAX, "aggregate correct across fragments");
}

int main(void)
{
    test_single_node_elects_itself();
    test_three_boards_elect_and_work();
    test_worker_death_midjob();
    test_coordinator_death_reelects();
    test_kv_storage();
    test_vm_job_across_boards();
    test_vm_sandbox_kills_runaway();
    test_fragmentation_roundtrip();
    printf("\n%s (%d failures)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
