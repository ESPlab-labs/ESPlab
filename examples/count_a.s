# count the 'a' bytes in this chunk's input slice, emit the count
# as a 4-byte little-endian value (the machine sums them across chunks)
  lds   r0, 0            # r0 = slice lo (where my input slice starts)
  lds   r1, 1            # r1 = slice hi (where it ends)
  loadi r2, 'a'
  loadi r3, 0
loop:
  cmp  r4, r0, r1        # r4 = (r0 < slice_hi)?
  jz   r4, done
  ldv  r5, r0            # r5 = input[r0]
  cmp  r6, r5, r2
  jnz  r6, skip
  add  r3, r3, 1
skip:
  add  r0, r0, 1
  jmp  loop
done:
  emit r3                # count byte 0 (LSB)
  shr  r4, r3, 8
  emit r4                # byte 1
  shr  r4, r3, 16
  emit r4                # byte 2
  shr  r4, r3, 24
  emit r4                # byte 3 (MSB)
  halt
