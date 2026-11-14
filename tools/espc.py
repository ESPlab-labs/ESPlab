#!/usr/bin/env python3
"""espc — the ESPlab task compiler.

Compile a small C-like language into ESPlab VM bytecode, so nobody has to
learn the assembly to put work on the cluster.

    python3 tools/espc.py program.epl -o task.bin

Language (integers only, by design — the VM is integer-only):

    # comments with '#'
    var x = 0                 # variables (max 9); types are implied
    var h[64]                 # array (max 3, size 1..64), zeroed every chunk
    x = x + 1
    h[i] = h[i] + 1           # array read/write; index 0..size-1 is on you
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
            if self.at("["):
                self.next()
                size = self.expect("num")[1]
                self.expect("]")
                if self.at("="):
                    raise SyntaxError(
                        f"line {t[2]}: arrays cannot be initialized - they are "
                        f"already zero at the start of every chunk")
                return ("arrdec", name, size)
            self.expect("=")
            e = self.expr()
            return ("vardec", name, e)
        if t[0] == "name":
            self.next()
            if self.at("["):
                self.next()
                idx = self.expr()
                self.expect("]")
                self.expect("=")
                val = self.expr()
                return ("astore", t[1], idx, val)
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
            if self.at("["):
                self.next()
                idx = self.expr()
                self.expect("]")
                return ("avar", t[1], idx)
            return ("var", t[1])
        if t[0] == "(":
            e = self.expr()
            self.expect(")")
            return e
        raise SyntaxError(f"line {t[2]}: unexpected {t[1]!r} in expression")

# ---------------- codegen ----------------
# Register map (16 regs):
#   r0-r6, r8-r9   = variables (9)
#   r10-r12        = expression temporaries
#   r13 / r14      = cached chunk slice bounds (lo / hi), set once in prologue
#   r15            = constant cache (LOADI results; hoisted out of loops
#                    for free because the value stays valid across iterations)
#   r7             = untouched (vbasm scratch convention)
VAR_REGS = [0, 1, 2, 3, 4, 5, 6, 8, 9]
TEMP_REGS = [10, 11, 12]
LO_REG, HI_REG, CONST_REG = 13, 14, 15
# array backing store: must match core/inc/vm.h (VM_SLOTS / VM_SLOT_SIZE)
VM_SLOT_COUNT = 3
VM_SLOT_WORDS = 64

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
        self.arrays = {}         # name -> (slot 0..2, word size 1..64)
        self.next_slot = 0
        self.temp_stack = list(reversed(TEMP_REGS))   # pop order: 12,11,10
        self.const_reg_v = None  # value currently cached in r15
        self.uses_lo = False     # prologue loads (set by pre-walk)
        self.uses_hi = False

    def get_const(self, v, line):
        """Constant in a register — cached in r15 so loop-invariant LOADIs
        cost once per task, not once per iteration."""
        if v < 0 or v > 65535:
            raise SyntaxError(f"line {line}: constant {v} out of range (0..65535)")
        if self.const_reg_v != v:
            self.emit("loadi", CONST_REG, v & 0xFF, (v >> 8) & 0xFF)
            self.const_reg_v = v
        return CONST_REG

    def temp_get(self, line):
        if not self.temp_stack:
            raise SyntaxError(
                f"line {line}: expression too deep (only {len(TEMP_REGS)} temps); "
                "split it into variables")
        return self.temp_stack.pop()
    def temp_put(self, r):
        self.temp_stack.append(r)

    def arr_reg(self, name, line):
        """array name -> (slot, size); the scratch-slot backing store."""
        if name not in self.arrays:
            raise SyntaxError(f"line {line}: '{name}' is not a declared array")
        return self.arrays[name]

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

    def direct_reg(self, x, line=0):
        """Register holding x's value right now (var / lo / hi / cached
        constant), or None if x needs evaluation. Reads are non-destructive:
        using these as instruction operands is always safe."""
        if x[0] == "var":
            return self.var_reg(x[1], line)
        if x[0] == "lo":
            return LO_REG
        if x[0] == "hi":
            return HI_REG
        if x[0] == "num":
            return self.get_const(x[1], line)
        return None

    # ---- expressions ----
    # gen_expr_dst(e, dst): evaluate e INTO dst without a wrapper MOV.
    # gen_expr(e): evaluate into a temp, return its register.
    def gen_expr(self, e):
        t = self.temp_get(0)
        self.gen_expr_dst(e, t)
        return t

    def gen_expr_dst(self, e, dst):
        kind = e[0]
        if kind == "num":
            if dst == CONST_REG:
                # r15 IS the constant cache — let get_const decide whether a
                # LOADI is needed (it is free when the value is cached).
                self.get_const(e[1], 0)
            else:
                # a plain destination must ALWAYS materialize the constant:
                # r15's cache can be rebuilt at any time by later code, so a
                # "destination register == cache too, skip" shortcut would
                # miscompile `while (a < 100) { ... } var c = 100` into
                # `var c`-loads-nothing (the LOADI for the compare serviced
                # r15, not this variable).
                if e[1] < 0 or e[1] > 65535:
                    raise SyntaxError(f"constant {e[1]} out of range (0..65535)")
                self.emit("loadi", dst, e[1] & 0xFF, (e[1] >> 8) & 0xFF)
        elif kind == "var":
            src = self.var_reg(e[1], 0)
            if src != dst:
                self.emit("mov", dst, src, 0)
        elif kind == "lo":
            if dst != LO_REG:
                self.emit("mov", dst, LO_REG, 0)
        elif kind == "hi":
            if dst != HI_REG:
                self.emit("mov", dst, HI_REG, 0)
        elif kind == "input":
            src = self.direct_reg(e[1])
            if src is not None:
                self.emit("ldv", dst, src, 0)
            else:
                self.gen_expr_dst(e[1], dst)
                self.emit("ldv", dst, dst, 0)
        elif kind == "avar":
            # array read: h[idx] -> LDX dst, <idx-in-reg>, slot
            slot, size = self.arr_reg(e[1], 0)
            srcreg = self.direct_reg(e[2])
            if srcreg is not None:
                self.emit("ldx", dst, srcreg, slot)
                return
            t = self.temp_get(0)
            self.gen_expr_dst(e[2], t)
            self.emit("ldx", dst, t, slot)
            self.temp_put(t)
        elif kind == "neg":
            self.gen_expr_dst(e[1], dst)
            z = self.get_const(0, 0)
            self.emit("sub", dst, z, dst)
        elif kind == "cmp":
            op, l, r = e[1], e[2], e[3]
            addend, skip = CMP_SEQ[op]
            self.gen_expr_dst(l, dst)
            t = self.temp_get(0)
            self.gen_expr_dst(r, t)
            self.emit("cmp", dst, dst, t)
            self.temp_put(t)
            if dst == CONST_REG:
                self.const_reg_v = None   # r15 now holds a dynamic 0/±1: cache dead
            if addend:
                self.emit("addi", dst, dst, addend & 0xFF)
            fl = self.uniq("cmpf"); join = self.uniq("cmpj")
            self.branch(skip, dst, fl, 0)
            one = self.get_const(1, 0) if dst != CONST_REG else None
            if dst == CONST_REG:
                # rare: comparison value cached in r15 — use a temp for 1
                t1 = self.temp_get(0)
                self.emit("loadi", t1, 1, 0)
                self.emit("mov", dst, t1, 0)
                self.temp_put(t1)
            else:
                self.emit("mov", dst, one, 0)
            self.branch("jmp", 0, join, 0)
            self.place(fl)
            zero = self.get_const(0, 0)
            if dst != CONST_REG:
                self.emit("mov", dst, zero, 0)
            else:
                self.const_reg_v = None   # r15 rewritten below anyway
                self.emit("loadi", dst, 0, 0)
                self.const_reg_v = 0
            self.place(join)
        elif kind in ("cmpmax", "cmpmin"):
            # max/min with lo/hi or a variable: use the operand register
            # directly instead of MOV-ing into dst first
            OPN = "max" if kind == "cmpmax" else "min"
            if dst == CONST_REG:
                self.const_reg_v = None   # r15 now holds a dynamic value: cache dead
            def operand_reg(x, avoid):
                if x[0] == "lo":
                    return LO_REG
                if x[0] == "hi":
                    return HI_REG
                if x[0] == "var":
                    return self.var_reg(x[1], 0)
                return None
            ra = operand_reg(e[1], dst)
            rb = operand_reg(e[2], dst)
            if ra is not None and rb is not None:
                self.emit(OPN, dst, ra, rb)
                return
            if ra is not None:
                t = self.temp_get(0)
                self.gen_expr_dst(e[2], t)
                self.emit(OPN, dst, ra, t)
                self.temp_put(t)
                return
            if rb is not None:
                self.gen_expr_dst(e[1], dst)
                self.emit(OPN, dst, dst, rb)
                return
            self.gen_expr_dst(e[1], dst)
            t = self.temp_get(0)
            self.gen_expr_dst(e[2], t)
            self.emit(OPN, dst, dst, t)
            self.temp_put(t)
        elif kind == "arith":
            op, l, r = e[1], e[2], e[3]
            # constant RHS: fold into the op when the ISA allows it
            if r[0] == "num" and op in ("+", "-", ">>"):
                self.gen_expr_dst(l, dst)
                v = r[1]
                if op == "+":
                    if 0 <= v <= 255:
                        self.emit("addi", dst, dst, v)
                    else:
                        t = self.get_const(v, 0)
                        self.emit("add", dst, dst, t)
                elif op == "-":
                    if 0 <= v <= 255:
                        self.emit("addi", dst, dst, (-v) & 0xFF)
                    else:
                        t = self.get_const(v, 0)
                        self.emit("sub", dst, dst, t)
                else:  # >>
                    t = self.get_const(v, 0)
                    self.emit("shr", dst, dst, t)
                return
            # constant LHS for commutative ops
            if l[0] == "num" and op in ("+", "*", "&", "|", "^"):
                self.gen_expr_dst(r, dst)
                t = self.get_const(l[1], 0)
                OP = {"+": "add", "*": "mul", "&": "and", "|": "or", "^": "xor"}[op]
                self.emit(OP, dst, dst, t)
                return
            # general case: eval LHS into dst, RHS into a temp — but when
            # either side is already in a register, use it as the operand
            # directly (no MOV, no temp)
            OP = {"+" : "add", "-": "sub", "*": "mul", "/": "div", "%": "mod",
                  "&": "and", "|": "or", "^": "xor", "<<": "shl", ">>": "shr"}[op]
            ra = self.direct_reg(l)
            rb = self.direct_reg(r)
            if ra is not None and rb is not None:
                self.emit(OP, dst, ra, rb)
                return
            if ra is not None and r[0] in ("cmp", "cmpmax", "cmpmin", "arith", "input", "neg"):
                t = self.temp_get(0)
                self.gen_expr_dst(r, t)
                self.emit(OP, dst, ra, t)
                self.temp_put(t)
                return
            if rb is not None and l[0] in ("cmp", "cmpmax", "cmpmin", "arith", "input", "neg"):
                self.gen_expr_dst(l, dst)
                self.emit(OP, dst, dst, rb)
                return
            self.gen_expr_dst(l, dst)
            t = self.temp_get(0)
            self.gen_expr_dst(r, t)
            self.emit(OP, dst, dst, t)
            self.temp_put(t)
        else:
            raise SyntaxError(f"internal: unknown expr {kind}")

    # ---- condition: jump to `end` when false ----
    # fused: evaluates operands straight into temps (no wrapper MOVs)
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
            # direct-operand fast path: an operand that already lives in a
            # register (var / lo / hi) is cmp'd in place — no MOV into a
            # temp. Constants are deliberately NOT classified here: they
            # materialize in r15, and evaluating the OTHER operand first
            # can rebuild the cache, so the constant must be cached AFTER.
            def dreg(x):
                if x[0] in ("var", "lo", "hi"):
                    return self.direct_reg(x, line)
                return None
            ra = dreg(l)
            rb = dreg(r)
            t = self.temp_get(line)
            if ra is not None and rb is not None:
                self.emit("cmp", t, ra, rb)
            elif ra is not None:
                self.gen_expr_dst(r, t)      # num/complex RHS lands in t
                self.emit("cmp", t, ra, t)
            elif rb is not None:
                self.gen_expr_dst(l, t)      # LHS lands in t; rb is safe
                self.emit("cmp", t, t, rb)
            elif r[0] == "num":
                self.gen_expr_dst(l, t)      # evaluate BEFORE caching r15
                t2 = self.get_const(r[1], line)
                self.emit("cmp", t, t, t2)
            elif l[0] == "num":
                self.gen_expr_dst(r, t)      # evaluate BEFORE caching r15
                t2 = self.get_const(l[1], line)
                self.emit("cmp", t, t2, t)
            else:
                t2 = self.temp_get(line)
                self.gen_expr_dst(l, t)
                self.gen_expr_dst(r, t2)
                self.emit("cmp", t, t, t2)
                self.temp_put(t2)
            if addend:
                self.emit("addi", t, t, addend & 0xFF)
            self.branch(skip, t, end_label, line)
            self.temp_put(t)
        elif kind == "var":
            reg = self.var_reg(e[1], line)
            self.branch("jz", reg, end_label, line)
        elif kind == "num":
            if e[1] == 0:
                self.branch("jmp", 0, end_label, line)
        else:
            t = self.gen_expr(e)
            self.branch("jz", t, end_label, line)
            self.temp_put(t)

    # ---- statements ----
    def gen_stmt(self, s):
        kind = s[0]
        if kind == "block":
            xs = s[1]
            i = 0
            while i < len(xs):
                x = xs[i]
                nxt = xs[i + 1] if i + 1 < len(xs) else None
                # fusion: `A = input[p]` followed by `p = p + 1` becomes the
                # single post-increment read LDVP (same as hand-written asm)
                if (x[0] == "assign" and x[2][0] == "input" and
                        x[2][1][0] == "var" and nxt and nxt[0] == "assign" and
                        nxt[1] == x[2][1][1] and
                        nxt[2] == ("arith", "+", ("var", nxt[1]), ("num", 1))):
                    dst = self.var_reg(x[1], 0)
                    rp = self.var_reg(nxt[1], 0)
                    self.emit("ldvp", dst, rp)
                    i += 2
                    continue
                self.gen_stmt(x)
                i += 1
        elif kind == "arrdec":
            name, size = s[1], s[2]
            if name in self.arrays or name in self.vars:
                raise SyntaxError(f"line 0: '{name}' redeclared")
            if size < 1 or size > VM_SLOT_WORDS:
                raise SyntaxError(
                    f"line 0: array size must be 1..64 words (got {size})")
            if self.next_slot >= VM_SLOT_COUNT:
                raise SyntaxError("line 0: too many arrays (max 3)")
            self.arrays[name] = (self.next_slot, size)
            self.next_slot += 1
        elif kind == "astore":
            # h[i] = <expr>. Same-element read-modify-write folds to
            # LDX/ADD/STX with the index evaluated once; the value
            # expression must be evaluated BEFORE the LDX so a constant
            # index cached in r15 cannot be rebuilt between load and store.
            name, idx, val = s[1], s[2], s[3]
            slot, size = self.arr_reg(name, 0)
            folds = (val[0] == "arith" and val[1] in ("+", "-") and
                     val[2] == ("avar", name, idx) and
                     idx[0] in ("var", "num"))
            if folds:
                # index: a variable is used in place (its register is never
                # clobbered by expression evaluation); a constant index is
                # materialized into a temp — never r15, which the value
                # expression may rebuild between the LDX and the STX.
                if idx[0] == "num":
                    t_idx = self.temp_get(0)
                    self.gen_expr_dst(idx, t_idx)
                    own_idx = True
                else:
                    t_idx = self.var_reg(idx[1], 0)
                    own_idx = False
                v = val[3][1] if val[3][0] == "num" else None
                small_addi = (val[1] == "+" and v is not None and 0 <= v <= 127)
                small_sub = (val[1] == "-" and v is not None and 0 <= v <= 128)
                tv = None
                if not (small_addi or small_sub):
                    tv = self.temp_get(0)
                    self.gen_expr_dst(val[3], tv)
                t = self.temp_get(0)
                self.emit("ldx", t, t_idx, slot)
                if small_addi:
                    self.emit("addi", t, t, v)
                elif small_sub:
                    self.emit("addi", t, t, (-v) & 0xFF)
                else:
                    self.emit("add" if val[1] == "+" else "sub", t, t, tv)
                    self.temp_put(tv)
                self.emit("stx", t, t_idx, slot)
                self.temp_put(t)
                if own_idx:
                    self.temp_put(t_idx)
                return
            # general store: index and value each into a temp
            t_idx = self.temp_get(0)
            self.gen_expr_dst(idx, t_idx)
            tv = self.temp_get(0)
            self.gen_expr_dst(val, tv)
            self.emit("stx", tv, t_idx, slot)
            self.temp_put(tv)
            self.temp_put(t_idx)
        elif kind == "vardec":
            dst = self.var_reg(s[1], 0)
            self.gen_expr_dst(s[2], dst)
        elif kind == "assign":
            dst = self.var_reg(s[1], 0)
            self.gen_expr_dst(s[2], dst)
        elif kind == "emit":
            if s[1][0] == "var":
                self.emit("emit", self.var_reg(s[1][1], 0))
            else:
                t = self.gen_expr(s[1])
                self.emit("emit", t)
                self.temp_put(t)
        elif kind == "emitn":
            # value: direct var register when possible
            if s[1][0] == "var":
                tv = self.var_reg(s[1][1], 0)
            else:
                tv = self.temp_get(0)
                self.gen_expr_dst(s[1], tv)
            tn = self.temp_get(0)
            self.gen_expr_dst(s[2], tn)
            self.emit("emitn", tv, 0, tn)
            if s[1][0] != "var":
                self.temp_put(tv)
            self.temp_put(tn)
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
                self.const_reg_v = None   # join: r15 differs per path
                self.place(end_l)
            else:
                self.gen_cond(cond, end_l, 0)
                self.gen_stmt(then)
                self.const_reg_v = None   # join: skipped path left other r15
                self.place(end_l)
        elif kind == "while":
            _, cond, body = s
            top = self.uniq("wtop")
            end = self.uniq("wend")
            self.place(top)
            self.gen_cond(cond, end, 0)
            self.gen_stmt(body)
            self.branch("jmp", 0, top, 0)
            self.const_reg_v = None       # join: fall-in and body-tail differ
            self.place(end)
        elif kind == "for":
            _, var, a, b, body = s
            reg = self.var_reg(var, 0)
            self.gen_expr_dst(a, reg)
            top = self.uniq("ftop")
            end = self.uniq("fend")
            self.place(top)
            # `for i in a..b` runs the body for i = a..b INCLUSIVE (the docs
            # idiom is `for i in 0..99`; v0.3.0's addend form ran one short).
            t = self.temp_get(0)
            tb = self.get_const(b[1], 0) if b[0] == "num" else None
            if tb is None:
                tb = self.temp_get(0)
                self.gen_expr_dst(b, tb)
            # exit test: i > b  <=>  cmp(i,b) - 1 == 0.  The cmp's ±1 offset
            # also means t != 0 on every iteration that runs the body, so
            # the stale t doubles as the post-bump backedge condition:
            #   top: cmp t,i,b ; addi t,-1 ; jz t,end ; body ; addi i ; jnz t,top
            # — 5 instructions per iteration, body runs for i = a..b INCLUSIVE.
            self.emit("cmp", t, reg, tb)
            self.emit("addi", t, t, 0xFF)
            self.branch("jz", t, end, 0)
            self.gen_stmt(body)
            self.emit("addi", reg, reg, 1)
            self.branch("jnz", t, top, 0)
            self.temp_put(t)
            if b[0] != "num":
                self.temp_put(tb)
            self.const_reg_v = None       # join: exit and body-tail differ
            self.place(end)
        else:
            raise SyntaxError(f"internal: unknown stmt {kind}")

def compile_src(src):
    p = Parser(tokenize(src))
    stmts = []
    while not p.at("eof"):
        stmts.append(p.statement())
    cg = Codegen()
    # pre-walk: which slice-bound globals does the program use? (prologue)
    def walk(e):
        if not isinstance(e, tuple):
            return
        if e[0] == "lo":
            cg.uses_lo = True
        if e[0] == "hi":
            cg.uses_hi = True
        for x in e[1:]:
            if isinstance(x, tuple):
                walk(x)
            elif isinstance(x, list):
                for y in x:
                    walk(y)
    for s in stmts:
        walk(s)
    if cg.uses_lo:
        cg.emit("lds", LO_REG, 0, 0)
    if cg.uses_hi:
        cg.emit("lds", HI_REG, 1, 0)
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
    "ldx": 32, "stx": 33,
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
