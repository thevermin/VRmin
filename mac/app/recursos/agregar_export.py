#!/usr/bin/env python3
# Agrega a una DLL de Wine (PE) funciones que le faltan y que no hacen nada (devuelven 0 = STATUS_SUCCESS): una sección
# nueva con la tabla de exportaciones completa (la vieja más las nuevas) y el código de las nuevas. Sin dependencias
# (solo la biblioteca estándar: corre con el python3 de macOS). Si ya están, no toca nada.
# Wine reemplaza las funciones que un programa importa y no existen por unas que lo cierran al llamarlas: el servicio
# de Steam usa D3DKMTSetProcessSchedulingPriorityClass (gdi32) para darle prioridad al compositor de SteamVR, se cerraba,
# y Steam se quedaba esperándolo para siempre (y con él SteamVR y los juegos).
#   agregar_export.py <dll> <Nombre>=<bytes de argumentos en x86> ...
import os, struct, sys

def main(path, funcs):
    d = bytearray(open(path, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    machine, nsec = struct.unpack_from('<HH', d, pe + 4)
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    is64 = struct.unpack_from('<H', d, opt)[0] == 0x20b
    salign, falign = struct.unpack_from('<II', d, opt + 32)
    dd = opt + (112 if is64 else 96)            # directorios de datos
    secs = opt + optsz
    sections = []
    for i in range(nsec):
        o = secs + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, o + 8)
        sections.append((o, va, vsize, raw, rsize))
    def off(rva):
        for _, va, vs, raw, rs in sections:
            if va <= rva < va + max(vs, rs): return raw + rva - va
        raise ValueError(hex(rva))
    def cstr(rva):
        o = off(rva); return bytes(d[o:d.index(0, o)])
    erva, esize = struct.unpack_from('<II', d, dd)
    e = off(erva)
    (_, _, _, _, name_rva, base, nfun, nnames, afun, anames, aords) = struct.unpack_from('<IIHHIIIIIII', d, e)
    funcs_rva = [struct.unpack_from('<I', d, off(afun) + 4 * i)[0] for i in range(nfun)]
    names = [cstr(struct.unpack_from('<I', d, off(anames) + 4 * i)[0]) for i in range(nnames)]
    ords = [struct.unpack_from('<H', d, off(aords) + 2 * i)[0] for i in range(nnames)]
    nuevos = [(n.encode(), a) for n, a in funcs if n.encode() not in names]
    if not nuevos:
        print('ya estaban'); return
    # (lugar en la tabla de secciones para una más)
    if secs + 40 * (nsec + 1) > sections[0][3]:
        sys.exit('no hay lugar para otra sección')
    last = max(sections, key=lambda s: s[1])
    new_va = (last[1] + max(last[2], last[4]) + salign - 1) // salign * salign
    new_raw = (len(d) + falign - 1) // falign * falign
    blob = bytearray()
    def put(b, align=4):
        while len(blob) % align: blob.append(0)
        p = len(blob); blob.extend(b); return new_va + p
    # código de las funciones nuevas: devuelven 0
    code_rva = {}
    for n, args in nuevos:
        code = b'\x31\xc0\xc3' if is64 else (b'\x31\xc0\xc2' + struct.pack('<H', args) if args else b'\x31\xc0\xc3')
        code_rva[n] = put(code, 16)
    # funciones: las viejas (las reenviadas copian su texto acá: deben quedar dentro del directorio nuevo) y las nuevas
    all_funcs = list(funcs_rva)
    for n, _ in nuevos:
        all_funcs.append(code_rva[n])
    pairs = sorted([(names[i], ords[i]) for i in range(nnames)] + [(n, nfun + k) for k, (n, _) in enumerate(nuevos)])
    dir_start = len(blob)
    while dir_start % 4: blob.append(0); dir_start += 1
    hdr_pos = len(blob); blob.extend(b'\0' * 40)
    af_pos = len(blob); blob.extend(b'\0' * 4 * len(all_funcs))
    an_pos = len(blob); blob.extend(b'\0' * 4 * len(pairs))
    ao_pos = len(blob); blob.extend(b'\0' * 2 * len(pairs))
    dllname = put(cstr(name_rva) + b'\0', 1)
    name_rvas = [put(n + b'\0', 1) for n, _ in pairs]
    for i, f in enumerate(all_funcs):
        if i < nfun and erva <= f < erva + esize:   # reenviada: «dll.función»
            f = put(cstr(f) + b'\0', 1)
        struct.pack_into('<I', blob, af_pos + 4 * i, f)
    for i, (n, o) in enumerate(pairs):
        struct.pack_into('<I', blob, an_pos + 4 * i, name_rvas[i])
        struct.pack_into('<H', blob, ao_pos + 2 * i, o)
    struct.pack_into('<IIHHIIIIIII', blob, hdr_pos, 0, 0, 0, 0, dllname, base, len(all_funcs), len(pairs),
                     new_va + af_pos, new_va + an_pos, new_va + ao_pos)
    dir_rva, dir_size = new_va + hdr_pos, len(blob) - hdr_pos
    rsize = (len(blob) + falign - 1) // falign * falign
    d.extend(b'\0' * (new_raw - len(d)))
    d.extend(blob + b'\0' * (rsize - len(blob)))
    so = secs + 40 * nsec
    struct.pack_into('<8sIIIIIIHHI', d, so, b'.vrmin\0\0', len(blob), new_va, rsize, new_raw, 0, 0, 0, 0, 0x60000020)
    struct.pack_into('<H', d, pe + 6, nsec + 1)
    struct.pack_into('<I', d, opt + 56, (new_va + len(blob) + salign - 1) // salign * salign)   # SizeOfImage
    struct.pack_into('<II', d, dd, dir_rva, dir_size)
    struct.pack_into('<I', d, opt + 64, 0)   # (sin suma de verificación)
    # (a un archivo nuevo que reemplaza al otro: los programas que ya la tienen abierta siguen con la vieja)
    open(path + '.nueva', 'wb').write(d)
    os.replace(path + '.nueva', path)
    print('agregadas:', ', '.join(n.decode() for n, _ in nuevos))

if __name__ == '__main__':
    main(sys.argv[1], [(a.split('=')[0], int(a.split('=')[1])) for a in sys.argv[2:]])
