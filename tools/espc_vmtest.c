/* espc_vmtest — run one VM task on the REAL core (core/src/vm.c) and print
 * a machine-readable line for tools/test_espc.py.
 *
 * usage: espc_vmtest <task.bin> <input.bin> <lo> <hi>
 * prints: status=<n> executed=<n> out=<hex bytes>
 *
 * This is the single-task harness the espc regression tests drive: it needs
 * no cluster, so a compiler miscompile shows up as a wrong byte here, not
 * as a mystery in an end-to-end run.
 */
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t code[VM_CODE_MAX], input[GL_VM_INPUT_MAX], out[512];

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: espc_vmtest <task.bin> <input.bin> [lo] [hi]\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t clen = fread(code, 1, sizeof(code), f);
    fclose(f);

    f = fopen(argv[2], "rb");
    size_t ilen = 0;
    if (f) { ilen = fread(input, 1, sizeof(input), f); fclose(f); }

    long lo = (argc > 3) ? strtol(argv[3], NULL, 10) : 0;
    long hi = (argc > 4) ? strtol(argv[4], NULL, 10) : (long)ilen;

    vm_t vm;
    vm_init(&vm, code, (uint16_t)clen, input, (uint16_t)ilen,
            out, sizeof(out), (int32_t)lo, (int32_t)hi);
    for (int slice = 0; slice < 20000 && vm.status == VM_OK; slice++) {
        vm_run(&vm, VM_CYCLES_PER_SLICE);
        if (vm.halted) break;
    }

    printf("status=%d executed=%u out=", vm.status,
           (unsigned)vm_stats.executed);
    for (unsigned i = 0; i < vm.out_len; i++)
        printf("%02x", out[i]);
    printf("\n");
    return 0;
}
