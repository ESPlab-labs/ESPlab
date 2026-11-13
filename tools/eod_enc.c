/* eod_enc — ESP On Demand encoder (PC side).
 * Raw RGB frames -> EOD container (rgb222 palette, one byte per pixel).
 *   usage: eod_enc <frames.bin WxH> <out.eod> [fps]
 * frames.bin: W*H*3 bytes per frame, no header. fps written into header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "eod_codec.h"

static int pal64_lut(int r, int g, int b)
{
    /* nearest rgb222 cell by rounding */
    int r2 = (r * 3 + 127) / 255;
    int g2 = (g * 3 + 127) / 255;
    int b2 = (b * 3 + 127) / 255;
    return (r2 << 4) | (g2 << 2) | b2;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: eod_enc frames.bin WxH out.eod [fps]\n"); return 1; }
    const char *size = argv[2];
    int W = atoi(size), H = atoi(strchr(size, 'x') + 1);
    int fps = (argc > 4) ? atoi(argv[4]) : 10;
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror(argv[1]); return 1; }
    FILE *fo = fopen(argv[3], "wb");
    if (!fo) { perror(argv[3]); return 1; }

    fseek(fi, 0, SEEK_END);
    long rawsz = ftell(fi);
    fseek(fi, 0, SEEK_SET);
    size_t fsz = (size_t)W * H * 3;
    long nframes = rawsz / (long)fsz;
    if (nframes < 1) { fprintf(stderr, "no frames\n"); return 1; }

    uint8_t hdr[8];
    hdr[0] = 'E'; hdr[1] = 'O'; hdr[2] = 'D'; hdr[3] = '1';
    hdr[4] = (uint8_t)fps; hdr[5] = 0;
    hdr[6] = nframes & 0xff; hdr[7] = (nframes >> 8) & 0xff;
    fwrite(hdr, 1, 8, fo);

    uint8_t *frame = malloc(fsz);
    uint8_t *idxs = malloc((size_t)W * H);
    uint8_t *rle = malloc((size_t)W * H * 2 + 16);
    if (!frame || !idxs || !rle) { fprintf(stderr, "oom\n"); return 1; }

    for (long f = 0; f < nframes; f++) {
        if (fread(frame, 1, fsz, fi) != fsz) { fprintf(stderr, "short read\n"); return 1; }
        /* quantize each pixel to rgb222 index */
        for (size_t p = 0; p < (size_t)W * H; p++)
            idxs[p] = (uint8_t)pal64_lut(frame[p*3], frame[p*3+1], frame[p*3+2]);
        /* horizontal RLE over the index bytes */
        size_t o = 0, npix = (size_t)W * H;
        size_t p = 0;
        while (p < npix) {
            uint8_t idx = idxs[p];
            size_t run = 1;
            while (p + run < npix && idxs[p + run] == idx && run < 256) run++;
            rle[o++] = (uint8_t)((run - 1) << 1);
            rle[o++] = idx;
            p += run;
        }
        /* frame record: u16 pair count then pairs */
        uint16_t npairs = (uint16_t)(o / 2);
        uint8_t fh[2];
        fh[0] = npairs & 0xff; fh[1] = npairs >> 8;
        fwrite(fh, 1, 2, fo);
        fwrite(rle, 1, o, fo);
    }
    fclose(fi);
    fclose(fo);
    printf("%s: %ld frames %dx%d @%dfps\n", argv[3], nframes, W, H, fps);
    return 0;
}
