#ifndef ESPLAB_VM_H
#define ESPLAB_VM_H
/* ESPlab task VM — a tiny sandboxed register machine for user tasks.
 * Portable C: compiles on ESP32 (Xtensa/RISC-V) and on the host for tests.
 * Determinism: no floats, no pointers, no host calls; same code + same input
 * = same result on every chip.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* fixed VM limits (sized so 3 slots + program fit in a classic ESP32 core's
 * share of RAM while leaving room for the cluster core) */
#define VM_REGS       16
#define VM_SLOTS      3
#define VM_SLOT_SIZE  256
#define VM_CODE_MAX   256
#define VM_STACK      8
#define GL_VM_INPUT_MAX 2816  /* frame jobs carry a whole frame's RLE (real footage ~2.7KB) */
#define VM_CYCLES_PER_SLICE 5000

/* opcodes — a full byte each; operands packed per-format below */
typedef enum {
    OP_NOP = 0, OP_LOADI, OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_MOD, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR,
    OP_LD, OP_ST, OP_LDV, OP_LDS, OP_CMP, OP_JMP, OP_JZ, OP_JNZ,
    OP_CALL, OP_RET, OP_HALT,
    OP_EMIT,   /* append rA to this task's output buffer */
    OP_FAIL,   /* explicit fault: status=VM fault */
    OP_YIELD,
    /* v0.3 additions (appended so older bytecode keeps its meaning) */
    OP_ADDI,   /* rA = rB + sext(c): small-constant add, no LOADI dance */
    OP_LDVP,   /* rA = input[rB]; rB += 1  (post-increment input read) */
    OP_EMITN,  /* append rA's low byte rC times (clamped to out cap) */
    OP_MAX,    /* rA = max(rB, rC) */
    OP_MIN,    /* rA = min(rB, rC) */
} vm_op_t;

typedef enum {
    VM_OK = 0,
    VM_FAULT_BADOP, VM_FAULT_DIV0, VM_FAULT_MEM, VM_FAULT_STACK,
    VM_FAULT_CODE_TOO_BIG, VM_FAULT_INPUT_TOO_BIG,
} vm_status_t;

typedef struct {
    uint8_t  *code;  uint16_t code_len;
    uint8_t  *input; uint16_t input_len;
    int32_t  lo, hi;             /* this chunk's input slice [lo,hi) */
    int32_t  regs[VM_REGS];
    int32_t  slots[VM_SLOTS][VM_SLOT_SIZE / 4];  /* scratch arrays */
    int16_t  sp;                 /* call/jmp stack (return addrs) */
    int32_t  stack[VM_STACK];
    uint32_t pc;
    uint8_t *out;    uint16_t out_cap, out_len;
    uint8_t  status;
    uint8_t  halted;             /* set by OP_HALT or falling off the end */
    uint16_t cycles_left;        /* timeslice budget, refilled by caller */
} vm_t;

/* reset all VM state for a fresh run; lo/hi = this chunk's input slice */
void vm_init(vm_t *v, uint8_t *code, uint16_t code_len,
             uint8_t *input, uint16_t input_len,
             uint8_t *out, uint16_t out_cap,
             int32_t lo, int32_t hi);

/* run up to cycles_left instructions; returns v->status (VM_OK = ran out
 * of budget or halted cleanly; caller decides by checking pc/yc count) */
uint8_t vm_run(vm_t *v, uint32_t cycles);

/* execution stats for the console */
typedef struct { uint32_t executed, halted, faults, yields; } vm_stats_t;
extern vm_stats_t vm_stats;

#endif /* ESPLAB_VM_H */
