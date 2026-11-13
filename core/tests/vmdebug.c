/* debug: run the test's count_a VM program standalone and show everything */
#include "../inc/vm.h"
#include "../inc/protocol.h"
#include <stdio.h>
#include <string.h>

static uint16_t build_count_a(uint8_t *code)
{
    uint16_t n = 0;
    code[n++] = OP_LDS;  code[n++] = 0; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_LDS;  code[n++] = 1; code[n++] = 1; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 2; code[n++] = 'a'; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 3; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 7; code[n++] = 1; code[n++] = 0;
    code[n++] = OP_CMP;  code[n++] = 4; code[n++] = 0; code[n++] = 1;
    code[n++] = OP_JZ;   code[n++] = 4; code[n++] = 7; code[n++] = 0;
    code[n++] = OP_LDV;  code[n++] = 5; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_CMP;  code[n++] = 6; code[n++] = 5; code[n++] = 2;
    code[n++] = OP_JNZ;  code[n++] = 6; code[n++] = 2; code[n++] = 0;
    code[n++] = OP_ADD;  code[n++] = 3; code[n++] = 3; code[n++] = 7;
    code[n++] = OP_ADD;  code[n++] = 0; code[n++] = 0; code[n++] = 7;
    code[n++] = OP_JMP;  code[n++] = 0; code[n++] = (uint8_t)(0-7); code[n++] = 0xFF;
    code[n++] = OP_LOADI;code[n++] = 8; code[n++] = 0xFF; code[n++] = 0;
    code[n++] = OP_AND;  code[n++] = 4; code[n++] = 3; code[n++] = 8;
    code[n++] = OP_EMIT; code[n++] = 4; code[n++] = 0; code[n++] = 0;
    code[n++] = OP_LOADI;code[n++] = 9; code[n++] = 8; code[n++] = 0;
    code[n++] = OP_SHR;  code[n++] = 5; code[n++] = 3; code[n++] = 9;
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

int main(void)
{
    uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX], out[JOB_OUT_MAX];
    uint16_t clen = build_count_a(code);
    uint16_t ilen = 64;
    for (int i = 0; i < ilen; i++) input[i] = (i % 3 == 0) ? 'a' : 'b';

    vm_t vm;
    vm_init(&vm, code, clen, input, ilen, out, sizeof(out), 21, 32);
    printf("code=%u bytes input[21..32)=", clen);
    for (int i = 21; i < 32; i++) putchar(input[i]);
    printf("\n");
    for (int slice = 0; slice < 8 && vm.status == VM_OK; slice++) {
        uint32_t pc0 = vm.pc;
        uint32_t y0 = vm_stats.yields;
        vm_run(&vm, VM_CYCLES_PER_SLICE);
        printf("slice %d: pc %u -> %u status=%u out_len=%u halted=%u\n",
               slice, pc0, vm.pc, vm.status, vm.out_len, vm.halted);
        if (vm.halted) break;
        if (vm.status == VM_OK && slice >= 1 && vm.pc == pc0 && vm_stats.yields == y0) {
            vm.status = VM_FAULT_STACK;
            printf("runaway guard fired\n");
        }
    }
    printf("final status=%u out_len=%u bytes:", vm.status, vm.out_len);
    for (int i = 0; i < vm.out_len; i++) printf(" %02x", out[i]);
    printf("\nexpected count in slice: ");
    int exp = 0;
    for (int i = 21; i < 32; i++) if (input[i] == 'a') exp++;
    printf("%d\n", exp);
    return 0;
}
