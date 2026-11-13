#ifndef EOD_CODEC_H
#define EOD_CODEC_H
/* ESP On Demand — tiny video codec shared by encoder (PC) and decoder (VM).
 *
 * One palette: 64 colors = rgb222 (r=c>>4, g=(c>>2)&3, b=c&3, scaled x85).
 * Encoded stream is ONE BYTE PER PIXEL (palette index); the browser does
 * color expansion. Chunks are 4-row bands: 64x4 = 256 decoded bytes/chunk.
 *
 * Container (little-endian):
 *   "EOD1" u8 fps u8 rsvd u16 nframes
 *   per frame: u16 pair_count, then pairs
 *   pair: byte0 = (run-1)<<1, byte1 = palette index (0..63)
 *
 * VM task input (192-byte cap, see eod_slice.py):
 *   [0..3] frameOff u32   [4..5] frameLen u16 (pairs*2)
 *   [6..7] framePix u16   [8..9] rowOff u16   [10..11] rowSpan u16
 *   [12..13] rowPitch u16 [14..15] frameIdx u16
 *   [16..] RLE pairs for this frame (up to 176 bytes)
 */
#define EOD_SLICE_BYTES 256   /* rowSpan*rowPitch = decoded bytes per chunk */
#define EOD_FRAME_HDR 16
#endif /* EOD_CODEC_H */
