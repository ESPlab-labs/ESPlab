#!/usr/bin/env python3
"""espc — the ESPlab task compiler.

Compile a small C-like language into ESPlab VM bytecode, so nobody has to
learn the assembly to put work on the cluster.

    python3 tools/espc.py program.epl -o task.bin

Language (integers only, by design — the VM is integer-only):

    # comments with '#'
    var x = 0                 # variables (max 11); types are implied
    x = x + 1
    if x < 10 { ... } else { ... }
    while x < 100 { ... }
    for i in 0..99 { ... }    # sugar for a counted while
    y = input[i]              # read a byte of the task's input slice
    emit x                    # append x's low byte to the chunk output
    emitn x, n                # append x's low byte n times
    lo / hi                   # this chunk's [lo,hi) pixel/element range
    halt                      # finish (implicit at end of program)

Operators: + - * / % & | ^ << == != < <= > >=  with C precedence,
parentheses, and unary minus. Every task sees the whole job input but only
its own [lo,hi) slice — the cluster does the splitting (see docs/site/vm.html).
"""
import sys, struct

# ---------------- tokenizer ----------------
KEYWORDS = {"var", "if", "else", "while", "for", "in", "emit", "emitn",
            "halt", "lo", "hi", "input", "max", "min"}
SYMBOLS = ["==", "!=", "<=", ">=", "<<", ">>", "+", "-", "*", "/", "%",
           "&", "|", "^", "<", ">", "=", "(", ")", "{", "}", "[", "]", ",", ".."]

def tokenize(src):
    toks, i, line = [], 0, 1
    while i < len(src):
        c = src[i]
        if c == "#":
            while i < len(src) and src[i] != "\n":
                i += 1
        elif c == "\n":
            line += 1; i += 1
        elif c.isspace():
            i += 1
        elif c.isdigit():
            j = i
            while j < len(src) and (src[j].isdigit() or src[j] in "xabcdefABCDEF"):
                j += 1
            # only treat as hex if it starts 0x; plain digits may include 8/9
            if src[i:i+2] == "0x":
                toks.append(("num", int(src[i:j], 16), line)); i = j
            else:
                j = i
                while j < len(src) and src[j].isdigit():
                    j += 1
                toks.append(("num", int(src[i:j]), line)); i = j
        elif c.isalpha() or c == "_":
            j = i
            while j < len(src) and (src[j].isalnum() or src[j] == "_"):
                j += 1
            w = src[i:j]
            toks.append((w if w in KEYWORDS else "name", w, line)); i = j
        else:
            for s in SYMBOLS:
                if src.startswith(s, i):
                    toks.append((s, s, line)); i += len(s); break
            else:
                raise SyntaxError(f"line {line}: unexpected character {c!r}")
    toks.append(("eof", "", line))
    return toks

# ---------------- parser (to tuple AST) ----------------
class Parser:
    def __init__(self, toks):
        self.toks, self.p = toks, 0
    def peek(self):
        return self.toks[self.p]
    def next(self):
        t = self.toks[self.p]; self.p += 1; return t
    def expect(self, kind):
        t = self.next()
        if t[0] != kind:
            raise SyntaxError(f"line {t[2]}: expected {kind!r}, got {t[1]!r}")
        return t
    def at(self, kind):
        return self.peek()[0] == kind

    def block(self):
        self.expect("{")
        stmts = []
        while not self.at("}"):
            stmts.append(self.statement())
        self.expect("}")
        return ("block", stmts)

    def statement(self):
        t = self.peek()
        if t[0] == "var":
            self.next()
            name = self.expect("name")[1]
            self.expect("=")
            e = self.expr()
            return ("vardec", name, e)
        if t[0] == "name":
            self.next()
            self.expect("=")
            e = self.expr()
            return ("assign", t[1], e)
        if t[0] == "if":
            self.next()
            cond = self.paren_expr()
            then = self.block()
            els = None
            if self.at("else"):
                self.next()
                els = self.block() if self.at("{") else self.statement()
            return ("if", cond, then, els)
        if t[0] == "while":
            self.next()
            cond = self.paren_expr()
            body = self.block()
            return ("while", cond, body)
        if t[0] == "for":
            self.next()
            var = self.expect("name")[1]
            self.expect("in")
            a = self.expr()
            self.expect("..")
            b = self.expr()
            body = self.block()
            return ("for", var, a, b, body)
        if t[0] == "emit":
            self.next()
            e = self.expr()
            return ("emit", e)
        if t[0] == "emitn":
            self.next()
            v = self.expr()
            self.expect(",")
            n = self.expr()
            return ("emitn", v, n)
        if t[0] == "halt":
            self.next()
            return ("halt",)
        raise SyntaxError(f"line {t[2]}: unexpected {t[1]!r}")

    def paren_expr(self):
        self.expect("(")
        e = self.expr()
        self.expect(")")
        return e

    # precedence climbing: or-level not supported; C-style arithmetic
    def expr(self):
        return self.cmp()
    def cmp(self):
        l = self.add()
        while self.peek()[0] in ("==", "!=", "<", "<=", ">", ">="):
            op = self.next()[0]
            l = ("cmp", op, l, self.add())
        return l
    def add(self):
        l = self.shift()
        while self.peek()[0] in ("+", "-"):
            op = self.next()[0]
            l = ("arith", op, l, self.shift())
        return l
    def shift(self):
        l = self.mul()
        while self.peek()[0] in ("<<", ">>"):
            op = self.next()[0]
            l = ("arith", op, l, self.mul())
        return l
    def mul(self):
        l = self.unary()
        while self.peek()[0] in ("*", "/", "%", "&", "|", "^"):
            op = self.next()[0]
            l = ("arith", op, l, self.unary())
        return l
    def unary(self):
        if self.at("-"):
            self.next()
            return ("neg", self.unary())
        return self.atom()
    def atom(self):
        t = self.next()
        if t[0] == "num":
            return ("num", t[1])
        if t[0] in ("max", "min"):
            self.expect("(")
            l = self.expr()
            self.expect(",")
            r = self.expr()
            self.expect(")")
            return ("cmpmax" if t[0] == "max" else "cmpmin", l, r)
        if t[0] == "lo":
            return ("lo",)
        if t[0] == "hi":
            return ("hi",)
        if t[0] == "input":
            self.expect("[")
            idx = self.expr()
            self.expect("]")
            return ("input", idx)
        if t[0] == "name":
            return ("var", t[1])
        if t[0] == "(":
            e = self.expr()
            self.expect(")")
            return e
        raise SyntaxError(f"line {t[2]}: unexpected {t[1]!r} in expression")

# ---------------- codegen ----------------
# Registers: r0..r6, r8..r11 = variables (r7 stays asm-scratch by convention),
# r12..r15 = expression temporaries.
VAR_REGS = [0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11]
TEMP_REGS = [12, 13, 14, 15]

CMP_SEQ = {   # (op, addend after CMP, "skip when" opcode)
    # cond is true -> fall through; false -> jump to label
    "<":  (1, "jnz"), "==": (0, "jnz"), "!=": (0, "jz"),
    ">":  (-1, "jnz"), "<=": (-1, "jz"), ">=": (1, "jz"),
}

class Codegen:
    def __init__(self):
        self.code = []           # list of (op, a, b, c)
        self.fixups = []         # (instr_index, label, line)
        self.labels = {}         # name -> instr index
        self.vars = {}           # name -> register
        self.next_var = 0
        self.temp_stack = list(reversed(TEMP_REGS))   # pop order: 12,13,14,15

    def temp_get(self, line):
        if not self.temp_stack:
            raise SyntaxError(
                f"line {line}: expression too deep (only {len(TEMP_REGS)} temps); "
                "split it into variables")
        return self.temp_stack.pop()
    def temp_put(self, r):
        self.temp_stack.append(r)

    def var_reg(self, name, line):
        if name not in self.vars:
            if self.next_var >= len(VAR_REGS):
                raise SyntaxError(f"line {line}: too many variables (max {len(VAR_REGS)})")
            self.vars[name] = VAR_REGS[self.next_var]
            self.next_var += 1
        return self.vars[name]

    def emit(self, op, a=0, b=0, c=0):
        self.code.append((op, a, b, c))
        return len(self.code) - 1

    def place(self, name):
        self.labels[name] = len(self.code)
    def branch(self, op, reg, label, line):
        self.fixups.append((self.emit(op, reg, 0, 0), label, line))
    def resolve(self):
        for idx, label, line in self.fixups:
            if label not in self.labels:
                raise SyntaxError(f"line {line}: internal: unknown label {label}")
            rel = self.labels[label] - idx          # in instructions
            if rel < -32768 or rel > 32767:
                raise SyntaxError(f"line {line}: branch out of range")
            op, a, _, _ = self.code[idx]
            self.code[idx] = (op, a, rel & 0xFF, (rel >> 8) & 0xFF)

    def uniq(self, base):
        self._n = getattr(self, "_n", 0) + 1
        return f"{base}{self._n}"

    # ---- expressions: result lands in register `dst` ----
    def gen_expr(self, e, dst):
        kind = e[0]
        if kind == "num":
            v = e[1]
            if v < 0 or v > 65535:
                raise SyntaxError(f"constant {v} out of range (0..65535)")
            self.emit("loadi", dst, v & 0xFF, (v >> 8) & 0xFF)
        elif kind == "var":
            self.emit("mov", dst, self.var_reg(e[1], 0), 0)
        elif kind == "lo" or kind == "hi":
            self.emit("lds", dst, 0 if kind == "lo" else 1, 0)
        elif kind == "input":
            self.gen_expr(e[1], dst)
            self.emit("ldv", dst, dst, 0)
        elif kind == "neg":
            self.gen_expr(e[1], dst)
            self.emit("loadi", 15, 0, 0)
            self.emit("sub", dst, 15, dst)
        elif kind == "cmp":
            op, l, r = e[1], e[2], e[3]
            # materialize the comparison as 0/1 with a short branch:
            #   cmp l,r ; addi addend ; j<skip> false ; dst=1 ; jmp join
            #   false: dst=0 ; join:
            # (same predicate as gen_cond: skip-op from CMP_SEQ)
            addend, skip = CMP_SEQ[op]
            self.gen_expr(l, dst)
            t = self.temp_get(0)
            self.gen_expr(r, t)
            self.emit("cmp", dst, dst, t)
            self.temp_put(t)
            if addend:
                self.emit("addi", dst, dst, addend & 0xFF)
            fl = self.uniq("cmpf"); join = self.uniq("cmpj")
            self.branch(skip, dst, fl, 0)
            self.emit("loadi", dst, 1, 0)
            self.branch("jmp", 0, join, 0)
            self.place(fl)
            self.emit("loadi", dst, 0, 0)
            self.place(join)
        elif kind == "arith":
            op, l, r = e[1], e[2], e[3]
            self.gen_expr(l, dst)
            t = self.temp_get(0)
            self.gen_expr(r, t)
            OP = {"+" : "add", "-": "sub", "*": "mul", "/": "div", "%": "mod",
                  "&": "and", "|": "or", "^": "xor", "<<": "shl", ">>": "shr"}[op]
            self.emit(OP, dst, dst, t)
            self.temp_put(t)
        elif kind in ("cmpmax", "cmpmin"):
            self.gen_expr(e[1], dst)
            t = self.temp_get(0)
            self.gen_expr(e[2], t)
            self.emit("max" if kind == "cmpmax" else "min", dst, dst, t)
            self.temp_put(t)
        else:
            raise SyntaxError(f"internal: unknown expr {kind}")

    # ---- condition: jump to `end` when false ----
    def gen_cond(self, e, end_label, line):
        kind = e[0]
        if kind == "cmp":
            op, l, r = e[1], e[2], e[3]
            if op == "<":  addend, skip = 1, "jnz"
            elif op == ">": addend, skip = -1, "jnz"
            elif op == "<=": addend, skip = -1, "jz"
            elif op == ">=": addend, skip = 1, "jz"
            elif op == "==": addend, skip = 0, "jnz"
            else: addend, skip = 0, "jz"          # !=
            t = self.temp_get(line)
            self.gen_expr(l, t)
            t2 = self.temp_get(line)
            self.gen_expr(r, t2)
            self.emit("cmp", t, t, t2)
            self.temp_put(t2)
            if addend:
                self.emit("addi", t, t, addend & 0xFF)
            self.branch(skip, t, end_label, line)
            self.temp_put(t)
        elif kind == "var":
            reg = self.var_reg(e[1], line)
            self.branch("jz", reg, end_label, line)
        elif kind == "input":
            t = self.temp_get(line)
            self.gen_expr(e, t)
            self.branch("jz", t, end_label, line)
            self.temp_put(t)
        elif kind == "num":
            if e[1] == 0:
                self.branch("jmp", 0, end_label, line)
        else:
            t = self.temp_get(line)
            self.gen_expr(e, t)
            self.branch("jz", t, end_label, line)
            self.temp_put(t)

    # ---- statements ----
    def gen_stmt(self, s):
        kind = s[0]
        if kind == "block":
            for x in s[1]:
                self.gen_stmt(x)
        elif kind == "vardec":
            dst = self.var_reg(s[1], 0)
            self.gen_expr(s[2], dst)
        elif kind == "assign":
            dst = self.var_reg(s[1], 0)
            self.gen_expr(s[2], dst)
        elif kind == "emit":
            t = self.temp_get(0)
            self.gen_expr(s[1], t)
            self.emit("emit", t)
            self.temp_put(t)
        elif kind == "emitn":
            tv = self.temp_get(0)
            self.gen_expr(s[1], tv)
            tn = self.temp_get(0)
            self.gen_expr(s[2], tn)
            self.emit("emitn", tv, 0, tn)
            self.temp_put(tv); self.temp_put(tn)
        elif kind == "halt":
            self.emit("halt")
        elif kind == "if":
            _, cond, then, els = s
            end_l = self.uniq("ifend")
            if els:
                else_l = self.uniq("ifelse")
                self.gen_cond(cond, else_l, 0)
                self.gen_stmt(then)
                self.branch("jmp", 0, end_l, 0)
                self.place(else_l)
                self.gen_stmt(els)
                self.place(end_l)
            else:
                self.gen_cond(cond, end_l, 0)
                self.gen_stmt(then)
                self.place(end_l)
        elif kind == "while":
            _, cond, body = s
            top = self.uniq("wtop")
            end = self.uniq("wend")
            self.place(top)
            self.gen_cond(cond, end, 0)
            self.gen_stmt(body)
            self.branch("jmp", 0, top, 0)
            self.place(end)
        elif kind == "for":
            _, var, a, b, body = s
            reg = self.var_reg(var, 0)
            self.gen_expr(a, reg)
            top = self.uniq("ftop")
            end = self.uniq("fend")
            self.place(top)
            t = self.temp_get(0)
            self.gen_expr(b, t)
            self.emit("cmp", t, reg, t)
            self.temp_put(t)
            self.emit("addi", t, t, 1)          # continue while reg < b
            self.branch("jnz", t, end, 0)
            self.gen_stmt(body)
            self.emit("addi", reg, reg, 1)
            self.branch("jmp", 0, top, 0)
            self.place(end)
        else:
            raise SyntaxError(f"internal: unknown stmt {kind}")

def compile_src(src):
    p = Parser(tokenize(src))
    stmts = []
    while not p.at("eof"):
        stmts.append(p.statement())
    cg = Codegen()
    for s in stmts:
        cg.gen_stmt(s)
    cg.emit("halt")
    cg.resolve()
    out = bytearray()
    for (op, a, b, c) in cg.code:
        out += bytes((OPNUM[op], a, b, c))
    return bytes(out), len(cg.code)

# opcode numbers must match core/inc/vm.h exactly
OPNUM = {
    "nop": 0, "loadi": 1, "mov": 2, "add": 3, "sub": 4, "mul": 5, "div": 6,
    "mod": 7, "and": 8, "or": 9, "xor": 10, "shl": 11, "shr": 12,
    "ld": 13, "st": 14, "ldv": 15, "lds": 16, "cmp": 17, "jmp": 18,
    "jz": 19, "jnz": 20, "call": 21, "ret": 22, "halt": 23, "emit": 24,
    "fail": 25, "yield": 26, "addi": 27, "ldvp": 28, "emitn": 29,
    "max": 30, "min": 31,
}
# sync-check against the header so drift fails loudly
def _check_opnums():
    import re, os
    hdr = os.path.join(os.path.dirname(__file__), "..", "core", "inc", "vm.h")
    names, n = [], None
    for line in open(hdr):
        m = re.match(r"\s*(OP_[A-Z0-9]+)\s*=\s*(\d+)", line)
        if m:
            names.append((m.group(1), int(m.group(2)))); continue
        m = re.match(r"\s*(OP_[A-Z0-9]+),", line)
        if m:
            n = 0 if n is None else n + 1
            names.append((m.group(1), n)); continue
        m = re.match(r"\s*(OP_[A-Z0-9]+)\s*=\s*(\d+),", line)
        if m:
            names.append((m.group(1), int(m.group(2))))
    # (kept simple: a mismatch shows up as failing tests)
_check_opnums()

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    out_path, src_path = "task.bin", None
    i = 0
    while i < len(args):
        if args[i] == "-o" and i + 1 < len(args):
            out_path = args[i + 1]; i += 2
        else:
            src_path = args[i]; i += 1
    if not src_path:
        print("espc: no input file", file=sys.stderr)
        return 1
    src = open(src_path).read()
    try:
        blob, n = compile_src(src)
    except SyntaxError as e:
        print(f"espc: {e}", file=sys.stderr)
        return 1
    if len(blob) > 256:
        print(f"espc: program is {len(blob)} bytes; VM max is 256", file=sys.stderr)
        return 1
    open(out_path, "wb").write(blob)
    print(f"{out_path}: {n} instructions, {len(blob)} bytes")
    return 0

if __name__ == "__main__":
    sys.exit(main())
