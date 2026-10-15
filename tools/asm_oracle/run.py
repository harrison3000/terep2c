#!/usr/bin/env python3
# Ground-truth oracle: run the reassembled original asm (16-bit real mode) under unicorn.
import sys, os, re, hashlib, struct
from unicorn import *
from unicorn.x86_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
CODE_SEG, DATA_SEG, STACK_SEG = 0x1000, 0x2000, 0xF000

def load_map():
    syms = {}
    data_real = None
    sec = None
    for line in open(os.path.join(HERE, 'main.map')):
        if line.startswith('---- Section'):
            sec = line.split()[2]
        m = re.match(r'\s*([0-9A-F]+)\s+([0-9A-F]+)\s+(\S+)$', line)
        if m and sec:
            syms[(sec, m.group(3))] = (int(m.group(1), 16), int(m.group(2), 16))
    return syms

syms = load_map()
data_real = syms[('.data', 'nova_linha')][0] - syms[('.data', 'nova_linha')][1]
def code(name): return syms[('.text', name)][1]
def data(name): return syms[('.data', name)][1]

binimg = open(os.path.join(HERE, 'main.bin'), 'rb').read()
mu = Uc(UC_ARCH_X86, UC_MODE_16)
mu.mem_map(0, 0x110000)
mu.mem_write(CODE_SEG * 16, binimg[:data_real])
mu.mem_write(DATA_SEG * 16, binimg[data_real:])
cr4 = mu.reg_read(UC_X86_REG_CR4)
mu.reg_write(UC_X86_REG_CR4, cr4 | (1 << 9) | (1 << 10))
cr0 = mu.reg_read(UC_X86_REG_CR0)
mu.reg_write(UC_X86_REG_CR0, (cr0 & ~(1 << 2)) | (1 << 1))

files = {}
next_seg = [0x3000]
track_dir = sys.argv[1]

def open_ci(name):
    for e in os.listdir(track_dir):
        if e.lower() == name.lower():
            return open(os.path.join(track_dir, e), 'rb')
    return None

def setcf(uc, v):
    fl = uc.reg_read(UC_X86_REG_EFLAGS)
    uc.reg_write(UC_X86_REG_EFLAGS, (fl | 1) if v else (fl & ~1))

def hook_intr(uc, intno, user):
    if intno != 0x21:
        ip = uc.reg_read(UC_X86_REG_IP)
        raise Exception('unexpected interrupt %x at %x' % (intno, ip))
    ax = uc.reg_read(UC_X86_REG_AX); ah = ax >> 8; al = ax & 0xff
    bx = uc.reg_read(UC_X86_REG_BX); cx = uc.reg_read(UC_X86_REG_CX); dx = uc.reg_read(UC_X86_REG_DX)
    ds = uc.reg_read(UC_X86_REG_DS)
    setcf(uc, 0)
    if ah == 0x3d:
        raw = uc.mem_read(ds * 16 + dx, 64)
        name = bytes(raw).split(b'\0')[0].decode()
        f = open_ci(name)
        if not f:
            setcf(uc, 1); uc.reg_write(UC_X86_REG_AX, 2); return
        h = 5
        while h in files: h += 1
        files[h] = f
        uc.reg_write(UC_X86_REG_AX, h)
    elif ah == 0x3e:
        f = files.pop(bx, None)
        if f: f.close()
        else: setcf(uc, 1)
    elif ah == 0x3f:
        f = files.get(bx)
        if not f: setcf(uc, 1); uc.reg_write(UC_X86_REG_AX, 6); return
        buf = f.read(cx)
        for i, b in enumerate(buf):
            uc.mem_write(ds * 16 + ((dx + i) & 0xffff), bytes([b]))
        uc.reg_write(UC_X86_REG_AX, len(buf))
    elif ah == 0x42:
        f = files.get(bx)
        if not f: setcf(uc, 1); return
        off = struct.unpack('<i', struct.pack('<I', (cx << 16) | dx))[0]
        f.seek(off, [0, 1, 2][al])
        p = f.tell()
        uc.reg_write(UC_X86_REG_AX, p & 0xffff); uc.reg_write(UC_X86_REG_DX, p >> 16)
    elif ah == 0x48:
        paras = bx
        if next_seg[0] + paras > 0xE000:
            setcf(uc, 1); return
        uc.reg_write(UC_X86_REG_AX, next_seg[0])
        next_seg[0] += paras
    else:
        raise Exception('unhandled dos %x' % ah)

mu.hook_add(UC_HOOK_INTR, hook_intr)

def call(name, regs=None):
    for r in (UC_X86_REG_DS, UC_X86_REG_ES):
        mu.reg_write(r, DATA_SEG)
    mu.reg_write(UC_X86_REG_SS, STACK_SEG)
    mu.reg_write(UC_X86_REG_SP, 0xFFFC)
    mu.mem_write(STACK_SEG * 16 + 0xFFFC, b'\0\0')
    mu.reg_write(UC_X86_REG_CS, CODE_SEG)
    for k, v in (regs or {}).items():
        mu.reg_write(k, v)
    mu.emu_start(CODE_SEG * 16 + code(name), CODE_SEG * 16 + code('entry_stop'))

def dsw(off):
    return struct.unpack('<H', mu.mem_read(DATA_SEG * 16 + off, 2))[0]

frames = int(sys.argv[2])
script = []
if len(sys.argv) > 3 and sys.argv[3]:
    for p in sys.argv[3].split(','):
        a, b = p.split(':'); script.append((int(a), int(b, 0)))
ppmdir = sys.argv[4] if len(sys.argv) > 4 else None

call('f_init')
if mu.reg_read(UC_X86_REG_AX) != 0:
    print('init failed'); sys.exit(1)
print('cars=%d' % struct.unpack('<h', mu.mem_read(DATA_SEG * 16 + 0x5bba, 2))[0])

def fnv(b):
    h = 1469598103934665603
    for x in b:
        h ^= x; h = (h * 1099511628211) & 0xffffffffffffffff
    return h

import zlib, re
# the ranges in dsmask.h
_h = open(os.environ.get('DSMASK', os.path.join(HERE, 'dsmask.h'))).read()
DS_MASK = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+) \}', _h)]
for fr in range(frames):
    for (f, sc) in script:
        if f == fr:
            call('FUN_keyboard_56df', {UC_X86_REG_AX: sc})
    call('FUN_timer_5680'); call('FUN_timer_5680')
    call('FUN_main_render')
    vseg = dsw(0xdb10)
    fb = bytes(mu.mem_read(vseg * 16, 64000))
    ds = bytearray(mu.mem_read(DATA_SEG * 16, 0xf000))
    for a, b in DS_MASK:
        ds[a:b + 1] = bytes(b + 1 - a)
    for base in (0xdb16, 0xdb68):           # ds_is_poly_attr in dsmask.h
        for o in range(base, base + 0x50):
            if (o - base) % 8 >= 4:
                ds[o] = 0
    for c in range(struct.unpack('<h', ds[0x5bba:0x5bbc])[0]):
        o = struct.unpack('<H', ds[0x5bbc + 2 * c:0x5bbe + 2 * c])[0] + 0x1e
        ds[o:o + 2] = bytes(2)
    print(fr, '%08x' % zlib.crc32(fb), '%08x' % zlib.crc32(ds), flush=True)
    if ppmdir and (not os.environ.get('DUMP_FRAME') or int(os.environ['DUMP_FRAME']) == fr):
        pal = ds[0x1a4d:0x1a4d + 768]
        with open(os.path.join(ppmdir, 'f%04d.ppm' % fr), 'wb') as f:
            f.write(b'P6 320 200 255\n')
            f.write(bytes(c for p in fb for c in pal[p * 3:p * 3 + 3]))
