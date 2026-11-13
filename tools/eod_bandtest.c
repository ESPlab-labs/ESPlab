/* eod_bandtest.c — run the eod decoder VM directly for each band, print status */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../core/inc/vm.h"
#include "../../core/inc/protocol.h"   /* JOB_OUT_MAX */

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: eod_bandtest <task.bin> <frame-input.bin>\n"); return 1; }
    static uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX], out[JOB_OUT_MAX];
    FILE *f = fopen(argv[1], "rb");
    uint16_t clen = (uint16_t)fread(code, 1, sizeof(code), f); fclose(f);
    f = fopen(argv[2], "rb");
    uint16_t ilen = (uint16_t)fread(input, 1, sizeof(input), f); fclose(f);
    printf("code %uB input %uB\n", clen, ilen);
    for (int band = 0; band < 12; band++) {
        vm_t vm;
        int lo = band * 256, hi = lo + 256;
        memset(out, 0, sizeof(out));
        vm_init(&vm, code, clen, input, ilen, out, JOB_OUT_MAX, lo, hi);
        for (int slice = 0; slice < 40 && vm.status == VM_OK; slice++) {
            vm_run(&vm, VM_CYCLES_PER_SLICE);
            if (vm.halted) break;
        }
        if (!vm.halted && vm.status == VM_OK) vm.status = VM_FAULT_STACK;
        printf("band %2d lo=%4d hi=%4d status=%d halted=%d out_len=%u out[0]=%02x out[255]=%02x\n",
               band, lo, hi, vm.status, vm.halted, vm.out_len,
               vm.out_len ? out[0] : 0, vm.out_len > 255 ? out[255] : 0);
    }
    return 0;
}
