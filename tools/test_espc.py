#!/usr/bin/env python3
"""espc regression tests — compile .epl programs and run them on the REAL
VM core (core/src/vm.c) via tools/espc_vmtest, comparing output bytes.

Why this exists: two miscompiles shipped in espc because nothing checked
compiled output against known answers —
  * `for i in 0..9` ran 9 iterations (range is inclusive; docs say 0..99)
  * `var c = 100` after a `< 100` loop compiled to nothing (constant-cache
    skip applied to real variables, and VM registers happen to start at 0
    so quick tests kept passing)
Both are pinned here now, along with the rest of the language surface.

The decoder cases also enforce the performance contract: the compiled
examples/eod_decode.epl must never execute MORE instructions than the
hand-tuned examples/eod_decode.s on the same band ("espc as fast as the
assembly"). Byte-exactness is checked against an independent Python
reference decoder, not against the VM.

usage: python3 tools/test_espc.py      (or `make test`)
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
sys.path.insert(0, os.path.join(ROOT, "tools"))
import espc  # noqa: E402  (compiles .epl in-process)

RUNNER = os.path.join(ROOT, "build", "espc_vmtest")
ASMFILE = os.path.join(ROOT, "build", "eod_decode.bin")  # built from the .s


def run_task(src, input_bytes, lo, hi):
    """Compile src, run it on the real VM core, return (out_list, executed)."""
    blob, _n = espc.compile_src(src)
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
        tf.write(blob)
        task_path = tf.name
    in_path = task_path + ".in"
    with open(in_path, "wb") as f:
        f.write(input_bytes)
    try:
        r = subprocess.run([RUNNER, task_path, in_path, str(lo), str(hi)],
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0 or not r.stdout.startswith("status="):
            raise RuntimeError(f"runner failed: {r.returncode} {r.stderr.strip()}"
                               f" {r.stdout.strip()}")
        head, outhex = r.stdout.strip().split("out=")
        status = int(head.split("status=")[1].split()[0])
        executed = int(head.split("executed=")[1].split()[0])
        if status != 0:
            raise RuntimeError(f"VM status {status} (fault) for task:\n{src}")
        return list(bytes.fromhex(outhex)), executed
    finally:
        os.unlink(task_path)
        os.unlink(in_path)


# ---------------- synthetic RLE frame (same wire layout as EOD frames) -------
def make_frame(seed=1234, width=3072):
    """Deterministic RLE frame: 16-byte header + (count, palette) pairs."""
    rng = random.Random(seed)
    rle = bytearray()
    cur = 0
    while cur < width:
        run = min(rng.randint(1, 128), width - cur)
        cnt = ((run - 1) << 1) | rng.randint(0, 1)   # run = (cnt >> 1) + 1
        rle.append(cnt)
        rle.append(rng.randint(0, 63))
        cur += run
    return bytes(rle)


def band_reference(rle, lo, hi):
    """Independent decoder for one [lo,hi) pixel band (the ground truth)."""
    out = bytearray()
    cur = 0
    j = 0
    while j < len(rle) and cur < hi:
        cnt, idx = rle[j], rle[j + 1]
        j += 2
        run = (cnt >> 1) + 1
        s, e = max(cur, lo), min(cur + run, hi)
        if e > s:
            out += bytes([idx]) * (e - s)
        cur += run
    return bytes(out)


# ---------------- language cases (name, src, input, lo, hi, expected) --------
def language_cases():
    cases = []

    def add(name, src, inp, lo, hi, expected, note=""):
        cases.append((name, src, inp, lo, hi, expected, note))

    # the v0.3.0 off-by-one: `for` range must be INCLUSIVE (was 72, want 90)
    add("for-inclusive",
        "var f = 0\nfor i in 0..9 { f = f + i * 2 }\nemit f\n",
        b"", 0, 0, [90], "0..9 must run 10 times (f=sum i*2, i=0..9)")

    # single-iteration and variable-bound loops (post-bump backedge paths)
    add("for-single",
        "var s = 0\nfor i in 5..5 { s = s + i }\nemit s\n",
        b"", 0, 0, [5], "5..5 runs exactly once")
    add("for-var-bound",
        "var n = 3\nvar s = 0\nfor i in 0..n { s = s + i }\nemit s\n",
        b"", 0, 0, [6], "variable bound: 0+1+2+3")

    # the v0.3.1 constant-cache miscompile: assignment after a < N loop
    add("const-after-cmp",
        "var a = 0\nwhile (a < 100) { a = a + 40 }\nvar c = 100\nemit a\nemit c\n",
        b"", 0, 0, [120, 100],
        "'var c = 100' compiled to nothing in the buggy r15 cache")

    # arithmetic, comparisons as values, else, unary minus
    a, b_ = 10, (10 + 3) * 2 - 5
    c = 1 if b_ > 20 else 2
    d = (-(a - 12) + (b_ % 7)) & 255
    e = (1 if a < b_ else 0) + (1 if b_ > c else 0)
    add("arith-cmp-neg",
        "var a = 10\n"
        "var b = (a + 3) * 2 - 5\n"
        "var c = 0\n"
        "if (b > 20) { c = 1 } else { c = 2 }\n"
        "var d = -(a - 12) + (b % 7)\n"
        "var e = (a < b) + (b > c)\n"
        "emit b\nemit c\nemit d\nemit e\n",
        b"", 0, 0, [b_ & 255, c, d, e], "nested arith/else/neg/cmp-as-value")

    # emit / emitn semantics: repeats, zero and negative counts, low byte
    add("emit-emitn",
        "emit 7\nemitn 7, 3\nemitn 300, 2\nemitn 5, 0\nemitn 9, -2\n",
        b"", 0, 0, [7, 7, 7, 7, (300 & 255), (300 & 255)],
        "emitn counts and low-byte truncation; <=0 emits nothing")

    # input indexing with computed index (temp path) — layout: 0,10,20,...
    add("input-index",
        "var s = 0\nvar i = 0\nwhile (i < 5) { s = s + input[i * 2]\ni = i + 1 }\nemit s\n",
        bytes(range(0, 20, 2)), 0, 20, [0 + 4 + 8 + 12 + 16],
        "input[expr]: every even-index byte")

    # lo/hi slice bounds + max/min as expressions
    add("lo-hi-maxmin",
        "var v = max(lo, 5) + min(hi, 7)\nemit v\n",
        b"", 3, 200, [max(3, 5) + min(200, 7)],
        "lo/hi come from the chunk slice; max/min fold to single ops")
    return cases


def decoder_cases():
    """The shipped decoder example, byte-exact vs an independent reference,
    on several bands of a synthetic frame — plus the asm-parity contract."""
    rle = make_frame()
    payload = struct.pack("<IHHHHHH", 0, len(rle), 3072, 0, 0, 0, 0) + rle
    epl_src = open(os.path.join(ROOT, "examples", "eod_decode.epl")).read()
    out = []
    for band in (0, 5, 11):
        lo, hi = band * 256, (band + 1) * 256
        out.append((f"decoder-band-{band}", epl_src, payload, lo, hi,
                    list(band_reference(rle, lo, hi)),
                    "byte-exact vs reference decoder"))
    return out, rle, payload


def main():
    if not os.path.exists(RUNNER):
        print(f"espc_vmtest not built ({RUNNER}); run `make` first")
        return 1

    fails = 0

    for name, src, inp, lo, hi, want, note in language_cases():
        try:
            got, _ = run_task(src, inp, lo, hi)
            if got != want:
                fails += 1
                print(f"FAIL {name}: want {want}, got {got}  ({note})")
            else:
                print(f"ok   {name}")
        except Exception as ex:
            fails += 1
            print(f"FAIL {name}: {ex}  ({note})")

    # decoder: byte-exact + instruction-count parity with the assembly
    cases, rle, payload = decoder_cases()
    have_asm = os.path.exists(ASMFILE)
    for name, src, inp, lo, hi, want, note in cases:
        try:
            got, epl_exec = run_task(src, inp, lo, hi)
            if got != want:
                fails += 1
                print(f"FAIL {name}: output differs from reference  ({note})")
                continue
            if have_asm:
                asm_got, asm_exec = run_task_file(ASMFILE, inp, lo, hi)
                if asm_got != want:
                    fails += 1
                    print(f"FAIL {name}: ASM decoder differs from reference?!")
                    continue
                if epl_exec > asm_exec:
                    fails += 1
                    print(f"FAIL {name}: espc {epl_exec} > hand asm {asm_exec} "
                          f"instructions — perf-contract regression ({note})")
                else:
                    print(f"ok   {name}  espc {epl_exec} <= asm {asm_exec} insns")
            else:
                print(f"ok   {name}  ({epl_exec} insns; asm parity skipped — "
                      f"build/eod_decode.bin not built)")
        except Exception as ex:
            fails += 1
            print(f"FAIL {name}: {ex}")

    print(f"\nespc tests: {'ALL PASSED' if fails == 0 else str(fails) + ' FAILED'}")
    return 1 if fails else 0


def run_task_file(task_path, input_bytes, lo, hi):
    """Run an already-assembled task binary (e.g. the hand-written asm)."""
    in_path = task_path + ".in"
    with open(in_path, "wb") as f:
        f.write(input_bytes)
    try:
        r = subprocess.run([RUNNER, task_path, in_path, str(lo), str(hi)],
                           capture_output=True, text=True, timeout=60)
        head, outhex = r.stdout.strip().split("out=")
        return list(bytes.fromhex(outhex)), int(head.split("executed=")[1].split()[0])
    finally:
        os.unlink(in_path)


if __name__ == "__main__":
    sys.exit(main())
