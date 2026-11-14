# eod_decode.s — ESP On Demand frame-slice decoder (EOD RLE -> palette bytes)
#
# One VM task = one horizontal band of one frame. The coordinator slices each
# frame into bands with its normal [lo,hi) chunking: chunk c covers pixel
# rows [c*4, c*4+4) of a 64x48 frame (12 bands, 256 decoded bytes each).
# Every chunk of the job receives the WHOLE frame's RLE in its input (one
# ELP_SHARE broadcast), so this program scans the runs and emits only the
# palette indices belonging to its band. Band outputs concatenated in chunk
# order = the frame.
#
# Task input (tools/eod_codec.h):
#   [0..3] frameOff u32   [4..5] frameLen u16 (RLE bytes, pairs*2)
#   [6..7] framePix u16   [8..9] rowOff u16   [10..11] rowSpan u16
#   [12..13] rowPitch u16 [14..15] frameIdx u16
#   [16..] RLE pairs: byte0 = (run-1)<<1, byte1 = palette index 0..63
#
# RUN-LEVEL EMISSION (v0.3): work happens per RUN, not per pixel. For each
# run [cur, cur+run) the visible slice is [max(cur,lo), min(cur+run,hi)) —
# two clamp ops — and EMITN writes it in one instruction. Cost per pair is
# constant (~13 instructions) no matter how long the run is, so flat areas
# of the frame are nearly free. The old pixel-level loop cost ~10.5
# instructions per PIXEL; this one is ~9x fewer for typical footage.
#
# CURSOR RULE (the subtle part): the pixel cursor r0 is ABSOLUTE (starts
# at 0, not at the band start). A run can straddle the band boundary; the
# clamps handle that exactly. Starting r0 at lo would shift every run
# boundary after the first straddling run and corrupt the band.
#
# Registers: r0 absolute pixel cursor   r1 pair cursor    r2 band end (hi)
#            r3 run length              r4 palette idx    r5 constant 1
#            r6 band start (lo)         r8 constant 0     r9 run start
#            r10 run end                r11 visible length / cmp scratch
#            (r7 stays scratch for assembler materialization; unused here)

  lds   r6, 0            # r6 = lo = first pixel of my band
  lds   r2, 1            # r2 = hi = one past my last pixel
  loadi r0, 0            # absolute cursor starts at the frame's first pixel
  loadi r1, 16           # pairs start at input[16]
  loadi r5, 1            # shift amount: cnt>>1 gives run-1
  loadi r8, 0            # zero constant for clamping
scan:
  cmp   r11, r0, r2      # whole band decoded? (r0 >= r2 ends the scan)
  addi  r11, r11, 1
  jnz   r11, done
  ldvp  r3, r1           # r3 = cnt byte, pair cursor advances
  ldvp  r4, r1           # r4 = palette index, pair cursor advances
  shr   r3, r3, r5       # r3 = run - 1
  addi  r3, r3, 1        # r3 = run length
  max   r9, r6, r0       # r9 = visible start  = max(lo, cur)
  add   r10, r0, r3      # r10 = run end (absolute)
  min   r10, r10, r2     # r10 = min(run end, hi)
  sub   r11, r10, r9     # r11 = visible length (negative if band missed it)
  max   r11, r11, r8     # clamp to 0
  jz    r11, adv         # this run is entirely outside my band
  emitn r4, r11          # emit the palette index r11 times, one instruction
adv:
  add   r0, r0, r3       # cursor advances by the FULL run either way
  cmp   r11, r0, r2      # loop while cursor < hi
  addi  r11, r11, 1
  jnz   r11, done
  jmp   scan
done:
  halt
