"""Minimal Python 1.5 .pyc reader / disassembler (for the game's AI scripts in res\\python).
  pyc15.py FILE.pyc                 list the functions (code objects)
  pyc15.py FILE.pyc NAME            disassemble the code object(s) called NAME
  pyc15.py FILE.pyc --calls WORD    disassemble every code object that mentions WORD in its names
"""
import struct, sys

OPS = {0: "STOP_CODE", 1: "POP_TOP", 2: "ROT_TWO", 3: "ROT_THREE", 4: "DUP_TOP", 10: "UNARY_POSITIVE",
       11: "UNARY_NEGATIVE", 12: "UNARY_NOT", 13: "UNARY_CONVERT", 15: "UNARY_INVERT", 19: "BINARY_POWER",
       20: "BINARY_MULTIPLY", 21: "BINARY_DIVIDE", 22: "BINARY_MODULO", 23: "BINARY_ADD", 24: "BINARY_SUBTRACT",
       25: "BINARY_SUBSCR", 30: "SLICE+0", 31: "SLICE+1", 32: "SLICE+2", 33: "SLICE+3", 40: "STORE_SLICE+0",
       41: "STORE_SLICE+1", 42: "STORE_SLICE+2", 43: "STORE_SLICE+3", 50: "DELETE_SLICE+0", 51: "DELETE_SLICE+1",
       52: "DELETE_SLICE+2", 53: "DELETE_SLICE+3", 60: "STORE_SUBSCR", 61: "DELETE_SUBSCR", 62: "BINARY_LSHIFT",
       63: "BINARY_RSHIFT", 64: "BINARY_AND", 65: "BINARY_XOR", 66: "BINARY_OR", 70: "PRINT_EXPR",
       71: "PRINT_ITEM", 72: "PRINT_NEWLINE", 80: "BREAK_LOOP", 82: "LOAD_LOCALS", 83: "RETURN_VALUE",
       85: "EXEC_STMT", 87: "POP_BLOCK", 88: "END_FINALLY", 89: "BUILD_CLASS", 90: "STORE_NAME",
       91: "DELETE_NAME", 92: "UNPACK_TUPLE", 93: "UNPACK_LIST", 94: "UNPACK_ARG", 95: "STORE_ATTR",
       96: "DELETE_ATTR", 97: "STORE_GLOBAL", 98: "DELETE_GLOBAL", 99: "UNPACK_VARARG", 100: "LOAD_CONST",
       101: "LOAD_NAME", 102: "BUILD_TUPLE", 103: "BUILD_LIST", 104: "BUILD_MAP", 105: "LOAD_ATTR",
       106: "COMPARE_OP", 107: "IMPORT_NAME", 108: "IMPORT_FROM", 110: "JUMP_FORWARD", 111: "JUMP_IF_FALSE",
       112: "JUMP_IF_TRUE", 113: "JUMP_ABSOLUTE", 114: "FOR_LOOP", 115: "LOAD_LOCAL", 116: "LOAD_GLOBAL",
       120: "SETUP_LOOP", 121: "SETUP_EXCEPT", 122: "SETUP_FINALLY", 123: "RESERVE_FAST", 124: "LOAD_FAST",
       125: "STORE_FAST", 126: "DELETE_FAST", 127: "SET_LINENO", 130: "RAISE_VARARGS", 131: "CALL_FUNCTION",
       132: "MAKE_FUNCTION", 133: "BUILD_SLICE"}
CMP = ("<", "<=", "==", "!=", ">", ">=", "in", "not in", "is", "is not", "exception match", "BAD")
NAME_OPS = {90, 91, 95, 96, 97, 98, 101, 105, 107, 108, 116}
LOCAL_OPS = {124, 125, 126}
REL_JUMPS = {110, 111, 112, 114, 120, 121, 122}


class Code:
    pass


class Reader:
    def __init__(self, data, pos):
        self.d, self.p = data, pos

    def byte(self):
        b = self.d[self.p]; self.p += 1; return b

    def i16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]; self.p += 2; return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v

    def obj(self):
        t = chr(self.byte())
        if t == "0": return None
        if t == "N": return None
        if t == "E": return Ellipsis
        if t == "i": return self.i32()
        if t == "l":
            n = self.i32(); v = 0
            for k in range(abs(n)): v |= self.i16() << (15 * k)
            return -v if n < 0 else v
        if t == "f":
            n = self.byte(); s = self.d[self.p:self.p + n]; self.p += n; return float(s)
        if t == "x":
            n = self.byte(); r = float(self.d[self.p:self.p + n]); self.p += n
            n = self.byte(); i = float(self.d[self.p:self.p + n]); self.p += n; return complex(r, i)
        if t == "s":
            n = self.i32(); s = self.d[self.p:self.p + n]; self.p += n; return s
        if t in "([":
            n = self.i32(); return tuple(self.obj() for _ in range(n))
        if t == "{":
            out = {}
            while True:
                k = self.obj()
                if k is None and self.d[self.p - 1] == ord("0"): break
                out[k] = self.obj()
            return out
        if t == "c":
            c = Code()
            c.argcount, c.nlocals, c.stacksize, c.flags = self.i16(), self.i16(), self.i16(), self.i16()
            c.code = self.obj(); c.consts = self.obj(); c.names = self.obj(); c.varnames = self.obj()
            c.filename = self.obj(); c.name = self.obj(); c.firstlineno = self.i16(); c.lnotab = self.obj()
            return c
        raise ValueError("unknown marshal type %r at %d" % (t, self.p - 1))


def s(x):
    return x.decode("latin1") if isinstance(x, bytes) else repr(x)


def walk(c, path=""):
    yield path + s(c.name), c
    for k in c.consts or ():
        if isinstance(k, Code): yield from walk(k, path + s(c.name) + ".")


def dis(c):
    b, i, line = c.code, 0, 0
    print("=== %s (args %d, line %d) vars %s" % (s(c.name), c.argcount, c.firstlineno, [s(v) for v in c.varnames or ()]))
    while i < len(b):
        op = b[i]
        name = OPS.get(op, "OP%d" % op)
        if op >= 90:
            arg = b[i + 1] | (b[i + 2] << 8); n = 3
            if op == 127: line = arg; i += n; continue
            if op == 100: a = repr(c.consts[arg]) if not isinstance(c.consts[arg], Code) else "<code %s>" % s(c.consts[arg].name)
            elif op in NAME_OPS: a = s(c.names[arg])
            elif op in LOCAL_OPS: a = s(c.varnames[arg])
            elif op == 106: a = CMP[arg]
            elif op in REL_JUMPS: a = "-> %d" % (i + n + arg)
            else: a = str(arg)
            print("%5d L%-4d %-18s %s" % (i, line, name, a))
        else:
            n = 1
            print("%5d L%-4d %s" % (i, line, name))
        i += n


data = open(sys.argv[1], "rb").read()
top = Reader(data, 8).obj()
funcs = list(walk(top))
if len(sys.argv) == 2:
    for path, c in funcs: print(path, "args", c.argcount)
elif sys.argv[2] == "--calls":
    w = sys.argv[3]
    for path, c in funcs:
        if any(w == s(n) for n in (c.names or ())): print("##", path); dis(c)
else:
    for path, c in funcs:
        if s(c.name) == sys.argv[2] or path == sys.argv[2]: print("##", path); dis(c)
