# eod_decode.s — ESP On Demand frame-slice decoder (EOD RLE -> palette bytes)
#
# One VM task = one horizontal band of one frame. The machine chops each
# frame into bands with its normal [lo,hi) chunking: chunk c covers pixel
# rows [c*4, c*4+4) of a 64x48 frame (12 bands, 256 decoded bytes each).
# Every chunk of the job receives the WHOLE frame's RLE in its input, so
# this program scans all pairs and emits only the palette indices belonging
# to its band. Band outputs are concatenated in chunk order = the frame.
#
# Task input (tools/eod_codec.h):
#   [0..3] frameOff u32   [4..5] frameLen u16 (RLE bytes, pairs*2)
#   [6..7] framePix u16   [8..9] rowOff u16   [10..11] rowSpan u16
#   [12..13] rowPitch u16 [14..15] frameIdx u16
#   [16..] RLE pairs: byte0 = (run-1)<<1, byte1 = palette index 0..63
#
# The band comes from lds (lo/hi are PIXEL indices here), so the same
# program serves any band of any frame — the machine does the chopping.
#
# CURSOR RULE (the subtle part): the pixel cursor r0 is ABSOLUTE (starts
# at 0, not at the band start). A run can straddle the band boundary, so
# we walk every pair from the frame's beginning and emit only pixels in
# [lo,hi). Starting r0 at lo would shift every run boundary after the
# first straddling run and corrupt the band.
#
# Registers: r0 absolute pixel cursor  r1 pair cursor  r2 band end (hi)
#            r3 run remaining  r4 current idx  r5 cmp/scratch
#            r6 band start (lo)
#            r7 = assembler scratch (vbasm materializes immediates there;
#            never keep a live value in r7).

  lds   r6, 0            # r6 = lo = first pixel of my band
  lds   r2, 1            # r2 = hi = one past my last pixel
  loadi r0, 0            # absolute cursor starts at the frame's first pixel
  loadi r1, 16           # pairs start at input[16]
scan:
  cmp   r5, r0, r2       # whole band decoded?
  add   r5, r5, 1
  jnz   r5, done
  ldv   r5, r1           # r5 = cnt byte
  add   r1, r1, 1
  ldv   r4, r1           # r4 = palette index
  add   r1, r1, 1        # r1 = next pair (cnt +1, idx +1 = +2 total)
  shr   r3, r5, 1
  add   r3, r3, 1        # r3 = run length
emit_loop:
  cmp   r5, r0, r2       # pixel cursor past my band?
  add   r5, r5, 1
  jnz   r5, next_pair    # band complete mid-run: skip rest of run
  cmp   r5, r0, r6       # pixel cursor before my band?
  add   r5, r5, 1
  jz    r5, skip_pix     # pre-band: walk the run without emitting
  emit  r4
skip_pix:
  add   r0, r0, 1
  sub   r3, r3, 1
  jnz   r3, emit_loop
next_pair:
  jmp   scan
done:
  halt
