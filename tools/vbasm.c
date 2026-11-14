/* vbasm — the ESPlab task assembler.
 * Assembly -> VM bytecode, matching the VM's real 4-byte encoding:
 *   [op][a][b][c]; LOADI packs a 16-bit value into b|c<<8;
 *   branches take an offset in INSTRUCTIONS from the current pc:
 *   target = pc + 4*off (so off=1 skips the next instruction).
 * Labels resolve automatically — no hand-counted jumps, ever again.
 *
 * Syntax (one instruction per line, ';' or '#' comments):
 *   loadi r2, 'a'      16-bit constant into register
 *   add   r3, r3, r1   dest, srcA, srcB  (sub/mul/div/mod/and/or/xor/shl/shr)
 *   cmp   r4, r0, r1   r4 = (r0 < r1) ? 1 : 0
 *   jz/jnz r4, done    branch if register is zero / nonzero
 *   jmp  done          unconditional branch
 *   ldv   r5, r0       r5 = input[r0]     (input is read-only)
 *   lds   r1, 0        r1 = slice lo  (b==0) or slice hi (b==1)
 *   emit  r3           append r3 (low byte) to the chunk output
 *   halt               finish this chunk
 */
#include "../core/inc/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#define MAX_LINES 1024
#define MAX_LABELS 128
#define MAX_FIXUPS 256

typedef struct { int instr; const char *label; int line_no; } fixup_t;

static char *src[MAX_LINES]; static int n_lines;
static struct { char name[32]; int instr; } labels[MAX_LABELS]; static int n_labels;
static fixup_t fixups[MAX_FIXUPS]; static int n_fixups;
static uint8_t out[VM_CODE_MAX]; static int out_len;

static void die(const char *msg, int line_no)
{
    if (line_no > 0) fprintf(stderr, "vbasm: line %d: %s\n", line_no, msg);
    else fprintf(stderr, "vbasm: %s\n", msg);
    exit(1);
}

static int find_label(const char *name)
{
    for (int i = 0; i < n_labels; i++)
        if (!strcmp(labels[i].name, name)) return labels[i].instr;
    return -1;
}

static int parse_reg(const char *tok, int line_no)
{
    if (tok[0] != 'r') die("expected register (r0..r7)", line_no);
    int r = atoi(tok + 1);
    if (r < 0 || r >= VM_REGS) die("register out of range", line_no);
    return r;
}

static int parse_imm16(const char *tok, int line_no)
{
    if (tok[0] == '\'') {                       /* character literal */
        if (tok[2] != '\'') die("bad char literal", line_no);
        return (int)(unsigned char)tok[1];
    }
    char *end;
    long v = strtol(tok, &end, 0);
    if (*end) die("bad immediate", line_no);
    if (v < 0 || v > 65535) die("immediate must be 0..65535 (LOADI is unsigned 16-bit)", line_no);
    return (int)v;
}

/* split "op a, b, c" into tokens (commas/whitespace both separate) */
static int tokenize(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static int parse_alu_operand(const char *tok, int line_no)
{
    if (tok[0] == 'r') return parse_reg(tok, line_no);
    return -parse_imm16(tok, line_no) - 1;      /* negative encodes an imm */
}

static void emit_instr(uint8_t op, uint8_t a, uint8_t b, uint8_t c)
{
    if (out_len + 4 > VM_CODE_MAX) die("program too big", 0);
    out[out_len++] = op;
    out[out_len++] = a;
    out[out_len++] = b;
    out[out_len++] = c;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: vbasm <input.s> [-o out.bin]\n");
        return 1;
    }
    const char *out_path = "task.bin";
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];

    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    char buf[512];
    while (fgets(buf, sizeof(buf), f) && n_lines < MAX_LINES) {
        buf[strcspn(buf, "\r\n")] = 0;
        src[n_lines++] = strdup(buf);
    }
    fclose(f);

    int instr = 0;
    for (int ln = 0; ln < n_lines; ln++) {
        char *tok[8];
        char *cm = strchr(src[ln], ';'); if (cm) *cm = 0;
        cm = strchr(src[ln], '#');      if (cm) *cm = 0;
        int n = tokenize(src[ln], tok, 8);
        if (n == 0) continue;
        /* label? ("name:") */
        size_t tl = strlen(tok[0]);
        if (tl > 1 && tok[0][tl - 1] == ':') {
            tok[0][tl - 1] = 0;
            if (find_label(tok[0]) >= 0) die("duplicate label", ln + 1);
            if (n_labels >= MAX_LABELS) die("too many labels", ln + 1);
            snprintf(labels[n_labels].name, 32, "%s", tok[0]);
            labels[n_labels].instr = instr;
            n_labels++;
            for (int k = 0; k < 7; k++) tok[k] = tok[k + 1];
            n--;
            if (n == 0) continue;
        }
        const char *op = tok[0];
        uint8_t opcode = 255, a = 0, b = 0, c = 0;
        int want = -1;                 /* expected operand count */
        int is_branch3 = 0;            /* jz/jnz: reg + label */
        int is_branch1 = 0;            /* jmp: label only */
        int wants_imm = 0;             /* loadi: reg + imm16 */

        if      (!strcmp(op, "loadi")) { opcode = OP_LOADI; want = 2; wants_imm = 1; }
        else if (!strcmp(op, "add"))   { opcode = OP_ADD; want = 3; }
        else if (!strcmp(op, "sub"))   { opcode = OP_SUB; want = 3; }
        else if (!strcmp(op, "mul"))   { opcode = OP_MUL; want = 3; }
        else if (!strcmp(op, "div"))   { opcode = OP_DIV; want = 3; }
        else if (!strcmp(op, "mod"))   { opcode = OP_MOD; want = 3; }
        else if (!strcmp(op, "and"))   { opcode = OP_AND; want = 3; }
        else if (!strcmp(op, "or"))    { opcode = OP_OR;  want = 3; }
        else if (!strcmp(op, "xor"))   { opcode = OP_XOR; want = 3; }
        else if (!strcmp(op, "shl"))   { opcode = OP_SHL; want = 3; }
        else if (!strcmp(op, "shr"))   { opcode = OP_SHR; want = 3; }
        else if (!strcmp(op, "cmp"))   { opcode = OP_CMP; want = 3; }
        else if (!strcmp(op, "ldv"))   { opcode = OP_LDV; want = 2; }
        else if (!strcmp(op, "lds"))   { opcode = OP_LDS; want = 2; }
        else if (!strcmp(op, "emit"))  { opcode = OP_EMIT; want = 1; }
        else if (!strcmp(op, "addi"))  { opcode = OP_ADDI; want = 3; }
        else if (!strcmp(op, "ldvp"))  { opcode = OP_LDVP; want = 2; }
        else if (!strcmp(op, "emitn")) { opcode = OP_EMITN; want = 2; }
        else if (!strcmp(op, "max"))   { opcode = OP_MAX;  want = 3; }
        else if (!strcmp(op, "min"))   { opcode = OP_MIN;  want = 3; }
        else if (!strcmp(op, "ldx"))  { opcode = OP_LDX; want = 3; }
        else if (!strcmp(op, "stx"))  { opcode = OP_STX; want = 3; }
        else if (!strcmp(op, "halt"))  { opcode = OP_HALT; want = 0; }
        else if (!strcmp(op, "nop"))   { opcode = OP_NOP;  want = 0; }
        else if (!strcmp(op, "yield")) { opcode = OP_YIELD; want = 0; }
        else if (!strcmp(op, "fail"))  { opcode = OP_FAIL; want = 0; }
        else if (!strcmp(op, "jmp"))   { opcode = OP_JMP;  want = 1; is_branch1 = 1; }
        else if (!strcmp(op, "jz"))    { opcode = OP_JZ;   want = 2; is_branch3 = 1; }
        else if (!strcmp(op, "jnz"))   { opcode = OP_JNZ;  want = 2; is_branch3 = 1; }
        else die("unknown instruction", ln + 1);

        if (n - 1 != want) die("wrong operand count", ln + 1);

        int is_lds = !strcmp(op, "lds");
        if (is_branch1 || is_branch3) {
            const char *label = tok[n - 1];
            if (is_branch3) a = (uint8_t)parse_reg(tok[1], ln + 1);
            if (label[0] >= '0' && label[0] <= '9') {
                int v = atoi(label);              /* raw instruction offset */
                b = (uint8_t)(v & 0xFF);
                c = (uint8_t)((v >> 8) & 0xFF);
            } else {
                int target = find_label(label);
                if (target >= 0) {
                    int rel = target - instr;     /* in instructions */
                    b = (uint8_t)(rel & 0xFF);
                    c = (uint8_t)((rel >> 8) & 0xFF);
                } else {
                    if (n_fixups >= MAX_FIXUPS) die("too many forward refs", ln + 1);
                    fixups[n_fixups].instr = instr;
                    fixups[n_fixups].label = strdup(label);
                    fixups[n_fixups].line_no = ln + 1;
                    n_fixups++;
                }
            }
        } else if (is_lds) {
            a = (uint8_t)parse_reg(tok[1], ln + 1);
            int v = parse_imm16(tok[2], ln + 1);      /* 0 = lo, 1 = hi */
            if (v > 1) die("lds selector must be 0 (lo) or 1 (hi)", ln + 1);
            b = (uint8_t)v;
        } else if (!strcmp(op, "addi")) {
            /* addi rA, rB, imm  — signed immediate rides directly in c */
            a = (uint8_t)parse_reg(tok[1], ln + 1);
            b = (uint8_t)parse_reg(tok[2], ln + 1);
            char *end; long v = strtol(tok[3], &end, 0);
            if (*end || v < -128 || v > 255)
                die("addi immediate must be -128..255", ln + 1);
            c = (uint8_t)(v & 0xFF);
        } else if (!strcmp(op, "emitn")) {
            /* emitn rA, rC  — value in a, count register in c (b unused) */
            a = (uint8_t)parse_reg(tok[1], ln + 1);
            c = (uint8_t)parse_reg(tok[2], ln + 1);
        } else if (!strcmp(op, "ldx") || !strcmp(op, "stx")) {
            /* ldx rA, rB, slot / stx rA, rB, slot — indexed scratch access:
             * rA = slot[slot][rB] resp. slot[slot][rB] = rA; slot in c */
            a = (uint8_t)parse_reg(tok[1], ln + 1);
            b = (uint8_t)parse_reg(tok[2], ln + 1);
            char *end; long s = strtol(tok[3], &end, 0);
            if (*end || s < 0 || s >= VM_SLOTS)
                die("slot index must be 0..2", ln + 1);
            c = (uint8_t)s;
        } else if (wants_imm) {
            a = (uint8_t)parse_reg(tok[1], ln + 1);
            int v = parse_imm16(tok[2], ln + 1);
            b = (uint8_t)(v & 0xFF);
            c = (uint8_t)((v >> 8) & 0xFF);
        } else if (want == 1 && !strcmp(op, "emit")) {
            a = (uint8_t)parse_reg(tok[1], ln + 1);
        } else if (want == 0) {
            /* nop/halt/yield/fail: no operands to parse */
        } else {
            int va = parse_alu_operand(tok[1], ln + 1);
            int vb2 = parse_alu_operand(tok[2], ln + 1);
            int vc = (n >= 4) ? parse_alu_operand(tok[3], ln + 1) : 0;
            if (vc < 0) {
                /* ALU immediate: the ISA takes registers only, so materialize
                 * the constant into r7 (VM_REGS-1) — r7 is scratch by
                 * convention. `add r3, r3, 1` becomes r7=1; add r3,r3,r7. */
                int k = -vc - 1;
                a = (uint8_t)va; b = (uint8_t)vb2;
                emit_instr(OP_LOADI, (uint8_t)(VM_REGS - 1),
                           (uint8_t)(k & 0xFF), (uint8_t)((k >> 8) & 0xFF));
                instr++;
                c = (uint8_t)(VM_REGS - 1);
            } else {
                a = (uint8_t)va; b = (uint8_t)vb2; c = (uint8_t)vc;
            }
        }
        emit_instr(opcode, a, b, c);
        instr++;
    }

    for (int i = 0; i < n_fixups; i++) {
        int target = find_label(fixups[i].label);
        if (target < 0) {
            fprintf(stderr, "vbasm: line %d: unknown label '%s'\n",
                    fixups[i].line_no, fixups[i].label);
            return 1;
        }
        int rel = target - fixups[i].instr;       /* in instructions */
        if (rel < -32768 || rel > 32767) die("branch out of range", fixups[i].line_no);
        int p = fixups[i].instr * 4 + 2;
        out[p] = (uint8_t)(rel & 0xFF);
        out[p + 1] = (uint8_t)((rel >> 8) & 0xFF);
    }

    f = fopen(out_path, "wb");
    if (!f) { perror(out_path); return 1; }
    fwrite(out, 1, (size_t)out_len, f);
    fclose(f);
    printf("%s: %d instructions, %d bytes\n", out_path, instr, out_len);
    return 0;
}
