/* ESPlab task VM implementation — see inc/vm.h.
 * One rule above all: never trust the bytecode. Every operand is bounds
 * checked before use; a bad instruction faults the task, never the machine. */
#include "vm.h"
#include <string.h>

vm_stats_t vm_stats;

static uint8_t rd8(vm_t *v, uint32_t i) {
    return (i < v->code_len) ? v->code[i] : 0;
}

void vm_init(vm_t *v, uint8_t *code, uint16_t code_len,
             uint8_t *input, uint16_t input_len,
             uint8_t *out, uint16_t out_cap,
             int32_t lo, int32_t hi)
{
    memset(v, 0, sizeof(*v));
    v->code = code; v->code_len = code_len;
    v->input = input; v->input_len = input_len;
    v->out = out; v->out_cap = out_cap;
    v->lo = lo; v->hi = hi;
    v->status = VM_OK;
}

static void vm_fault(vm_t *v, uint8_t st) {
    v->status = st;
    vm_stats.faults++;
}

static int32_t slot_read(vm_t *v, uint8_t slot, uint8_t idx4) {
    uint32_t idx = (uint32_t)idx4 * 4u;   /* idx4 = word index in 0..63 */
    if (idx >= VM_SLOT_SIZE) { vm_fault(v, VM_FAULT_MEM); return 0; }
    return v->slots[slot & 3][idx / 4];
}

static void slot_write(vm_t *v, uint8_t slot, uint8_t idx4, int32_t val) {
    uint32_t idx = (uint32_t)idx4 * 4u;
    if (idx >= VM_SLOT_SIZE) { vm_fault(v, VM_FAULT_MEM); return; }
    v->slots[slot & 3][idx / 4] = val;
}

/* LDV: read a byte of the task input. Input is read-only to the task. */
static int32_t vm_ldv(vm_t *v, uint32_t idx) {
    if (idx >= v->input_len) { vm_fault(v, VM_FAULT_MEM); return 0; }
    return v->input[idx];
}

uint8_t vm_run(vm_t *v, uint32_t cycles)
{
    v->cycles_left = (cycles > 0xFFFFu) ? 0xFFFFu : (uint16_t)cycles;

    while (v->cycles_left) {
        if (v->pc >= v->code_len) {           /* fell off the end: halt */
            v->halted = 1;
            vm_stats.halted++;
            return v->status;
        }
        uint8_t op = rd8(v, v->pc);
        uint8_t a = rd8(v, v->pc + 1);
        uint8_t b = rd8(v, v->pc + 2);
        uint8_t c = rd8(v, v->pc + 3);
        uint32_t next = v->pc + 4;

        if (v->status != VM_OK) return v->status;

        switch (op) {
        case OP_NOP:
            break;
        case OP_LOADI:
            if (a >= VM_REGS) { vm_fault(v, VM_FAULT_BADOP); break; }
            v->regs[a] = (int32_t)((uint32_t)b | ((uint32_t)c << 8));
            break;
        case OP_MOV:
            if (a >= VM_REGS || b >= VM_REGS) { vm_fault(v, VM_FAULT_BADOP); break; }
            v->regs[a] = v->regs[b];
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_AND: case OP_OR: case OP_XOR:
            if (a >= VM_REGS || b >= VM_REGS || c >= VM_REGS) {
                vm_fault(v, VM_FAULT_BADOP); break;
            }
            if ((op == OP_DIV || op == OP_MOD) && v->regs[c] == 0) {
                vm_fault(v, VM_FAULT_DIV0); break;
            }
            switch (op) {
            case OP_ADD: v->regs[a] = v->regs[b] + v->regs[c]; break;
            case OP_SUB: v->regs[a] = v->regs[b] - v->regs[c]; break;
            case OP_MUL: v->regs[a] = v->regs[b] * v->regs[c]; break;
            case OP_DIV: v->regs[a] = v->regs[b] / v->regs[c]; break;
            case OP_MOD: v->regs[a] = v->regs[b] % v->regs[c]; break;
            case OP_AND: v->regs[a] = v->regs[b] & v->regs[c]; break;
            case OP_OR:  v->regs[a] = v->regs[b] | v->regs[c]; break;
            case OP_XOR: v->regs[a] = v->regs[b] ^ v->regs[c]; break;
            }
            break;
        case OP_SHL: case OP_SHR:
            if (a >= VM_REGS || b >= VM_REGS || c >= VM_REGS) {
                vm_fault(v, VM_FAULT_BADOP); break;
            }
            {
                int32_t s = v->regs[c] & 31;
                if (op == OP_SHL) v->regs[a] = (int32_t)((uint32_t)v->regs[b] << s);
                else              v->regs[a] = v->regs[b] >> s;
            }
            break;
        case OP_LD:
            if (a >= VM_REGS || b > 3) { vm_fault(v, VM_FAULT_BADOP); break; }
            v->regs[a] = slot_read(v, b, c);
            break;
        case OP_ST: {
            if (a >= VM_REGS || b > 3) { vm_fault(v, VM_FAULT_BADOP); break; }
            slot_write(v, b, c, v->regs[a]);
            break;
        }
        case OP_LDV:
            if (a >= VM_REGS) { vm_fault(v, VM_FAULT_BADOP); break; }
            /* b = source reg holding the index */
            if (b >= VM_REGS) { vm_fault(v, VM_FAULT_BADOP); break; }
            v->regs[a] = vm_ldv(v, (uint32_t)v->regs[b]);
            break;
        case OP_LDS:
            /* the one "where am I" syscall: rA = chunk slice lo (b==0) or hi */
            if (a >= VM_REGS || b > 1) { vm_fault(v, VM_FAULT_BADOP); break; }
            v->regs[a] = (b == 0) ? v->lo : v->hi;
            break;
        case OP_CMP:
            if (a >= VM_REGS || b >= VM_REGS || c >= VM_REGS) {
                vm_fault(v, VM_FAULT_BADOP); break;
            }
            v->regs[a] = (v->regs[b] < v->regs[c]) ? -1 :
                         (v->regs[b] > v->regs[c]) ?  1 : 0;
            break;
        case OP_JMP: case OP_JZ: case OP_JNZ: {
            /* operand: 2-byte signed offset in bytes 2..3 (b|c) */
            int32_t off = (int32_t)((uint32_t)b | ((uint32_t)c << 8));
            if (off & 0x8000) off -= 0x10000;
            int32_t take = (op == OP_JMP) ? 1 :
                           (v->regs[a & 15] == 0) ? (op == OP_JZ) :
                                                    (op == OP_JNZ);
            if (take) {
                int32_t target = (int32_t)v->pc + 4 * off;
                if (target < 0 || (uint32_t)target >= v->code_len) {
                    vm_fault(v, VM_FAULT_MEM); break;
                }
                next = (uint32_t)target;
            }
            break;
        }
        case OP_CALL: case OP_RET:
            /* subroutines: CALL pushes the return pc on the VM stack */
            if (op == OP_CALL) {
                int32_t off = (int32_t)((uint32_t)b | ((uint32_t)c << 8));
                if (off & 0x8000) off -= 0x10000;
                if (v->sp >= VM_STACK) { vm_fault(v, VM_FAULT_STACK); break; }
                v->stack[v->sp++] = (int32_t)next;
                int32_t target = (int32_t)v->pc + 4 * off;
                if (target < 0 || (uint32_t)target >= v->code_len) {
                    vm_fault(v, VM_FAULT_MEM); break;
                }
                next = (uint32_t)target;
            } else {
                if (v->sp <= 0) { vm_fault(v, VM_FAULT_STACK); break; }
                next = (uint32_t)v->stack[--v->sp];
            }
            break;
        case OP_HALT:
            v->halted = 1;
            vm_stats.halted++;
            return v->status;
        case OP_EMIT:
            if (a >= VM_REGS) { vm_fault(v, VM_FAULT_BADOP); break; }
            if (v->out && v->out_len < v->out_cap)
                v->out[v->out_len++] = (uint8_t)v->regs[a] & 0xFF;
            break;
        case OP_FAIL:
            vm_fault(v, VM_FAULT_BADOP);
            return v->status;
        case OP_YIELD:
            vm_stats.yields++;
            return v->status;   /* caller refills cycles and re-enters */
        default:
            vm_fault(v, VM_FAULT_BADOP);
            return v->status;
        }

        if (v->status != VM_OK) return v->status;
        v->pc = next;
        v->cycles_left--;
        vm_stats.executed++;
    }
    return v->status;   /* budget exhausted: caller decides (yield) */
}
