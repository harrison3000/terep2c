#!/usr/bin/env python3
"""Copy a track, giving car 1 a synthetic model that uses every opcode of
the model bytecode (the demo's cars only use FLAT, FACING, SORT2, TEXTURED,
WHEEL and PIXEL), to make a test map (data/Asphalt-synth).

    python3 tools/synth_model.py [--no-shaded-tab] SRC_TRACK DST_TRACK

The synthetic program is built around car 1's own model (called with
CALL) and its polygons, so the result is derived from the track's files
and is not stored in the repository.  --no-shaded-tab leaves out
OP_SHADED_TAB.
"""
import os, shutil, struct, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
no_tab = '--no-shaded-tab' in sys.argv
src, dst = args
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(src, dst)
name = next(n for n in os.listdir(dst) if n.lower() == 'car1.dat')
path = os.path.join(dst, name)
f = open(path, 'rb').read()
mo = struct.unpack_from('<H', f, 4)[0]
orig = f[mo:]


def op_len(code, pc):
    op, n = code[pc] & 0x1f, code[pc + 1]
    return {1: 5, 2: 7, 3: 7, 4: 1 + 1 + (n + 1) * 2 + 2, 6: 2 + (n + 1) * 4, 7: 4 + (n + 1) * 4,
            8: 2 + (n + 1) * 6, 9: 21, 10: 1 + 0xba, 16: 5}[op]


# the first FLAT and the first TEXTURED polygon of the original model
pc, flat, tex = 0, None, None
while flat is None or tex is None:
    op = orig[pc] & 0x1f
    if op == 16:                        # SORT2: follow its first branch
        pc += 1 + struct.unpack_from('<H', orig, pc + 1)[0]
        continue
    if op == 4 and flat is None:
        flat = orig[pc:pc + op_len(orig, pc)]
    if op == 8 and tex is None:
        tex = orig[pc:pc + op_len(orig, pc)]
    pc += op_len(orig, pc)
n = flat[1]
points = [struct.unpack_from('<H', flat, 2 + 2 * i)[0] for i in range(n + 1)]

w16 = lambda v: struct.pack('<H', v & 0xffff)
shaded = bytes([6, n]) + b''.join(w16(p) + w16(0x400 + 0x300 * i) for i, p in enumerate(points))
shaded_tab = bytes([7, n]) + w16(0x200) + b''.join(w16(p) + w16(2 * i) for i, p in enumerate(points))
sprite = bytes([9]) + w16(points[0]) + w16(0x40) + b''.join(
    struct.pack('<I', v << 16 | u) for u, v in ((0, 0), (31, 0), (31, 31), (0, 31)))

m = bytearray()
fixups = {}                             # position of a rel16 -> (label, base)


def jump(op, label, base_extra=2):
    fixups[len(m) + 1] = (label, base_extra)
    m.extend([op, 0, 0])


labels = {}
jump(0x12, 'main')                                  # CALL the car's own model
m += bytes([2]) + w16(points[0]) + w16(points[2]) + w16(0)  # NEARER
m += bytes([5])                                     # NOP
jump(0x13, 'visible')                               # JUMP_VISIBLE
m += shaded + bytes([0])                            # hidden: SHADED, END
labels['visible'] = len(m)
m += tex
jump(0x11, 'c')                                     # JUMP forward
labels['b'] = len(m)
m += sprite + bytes([0])                            # SPRITE, END
labels['c'] = len(m)
m += bytes([3]) + w16(points[0]) + w16(points[1]) + w16(points[2])  # FACING
jump(0x14, 'h')                                     # JUMP_HIDDEN
m += flat
m += (shaded if no_tab else shaded_tab)             # over the FLAT
jump(0x11, 'b')                                     # JUMP backward
labels['h'] = len(m)
m += bytes([1]) + w16(points[1]) + w16(0x0f)        # PIXEL
jump(0x11, 'b')                                     # JUMP backward
labels['main'] = len(m)
m += orig                                           # SORT2 offsets are relative: it can move
for at, (label, extra) in fixups.items():
    struct.pack_into('<H', m, at, (labels[label] - (at + extra)) & 0xffff)

assert mo + len(m) <= 10000
open(path, 'wb').write(f[:mo] + bytes(m))
