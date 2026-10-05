"""Сборка trcli для Linux и macOS прямо на Windows компилятором Zig (zig c++).

    python tools/cross_build.py <путь к zig.exe>

Готовые архивы — в dist/: trcli-linux-x86_64.tar.gz, trcli-linux-arm64.tar.gz,
trcli-macos-universal.tar.gz (один файл для Apple Silicon и Intel).
"""
import concurrent.futures as cf, io, os, re, struct, subprocess, sys, tarfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, 'cpp')
ZIG = sys.argv[1] if len(sys.argv) > 1 else 'zig'
OUT = os.path.join(CPP, 'build-cross')
CORE = re.search(r'set\(CORE\s+(.*?)\)', open(os.path.join(CPP, 'CMakeLists.txt'), encoding='utf-8').read(), re.S).group(1).split()
TARGETS = {
    'linux-x86_64': 'x86_64-linux-gnu.2.17',      # glibc 2.17: идёт почти на любом Linux
    'linux-arm64': 'aarch64-linux-gnu.2.17',
    'macos-arm64': 'aarch64-macos.11.0',
    'macos-x86_64': 'x86_64-macos.10.15',
}
FLAGS = ['-std=gnu++20', '-O2', '-fno-char8_t', '-DNDEBUG', '-w', '-I', os.path.join(CPP, 'src')]

def resources_cpp(path):
    """ресурсы из res/resources.rc — массивами (как в CMakeLists для Linux/macOS)"""
    out = ['// generated from res/ (do not edit)\n#include <cstddef>\n']
    table = ['struct EmbeddedRes { const char *name; const unsigned char *data; size_t size; };\nextern const EmbeddedRes EMBEDDED_RES[] = {\n']
    for line in open(os.path.join(CPP, 'res', 'resources.rc'), encoding='utf-8'):
        m = re.match(r'^(\w+)\s+RCDATA\s+"([^"]+)"', line)
        if not m: continue
        data = open(os.path.join(CPP, 'res', m.group(2)), 'rb').read()
        out.append('static const unsigned char r_%s[] = {%s0};\n' % (m.group(1), ''.join('%d,' % b for b in data)))
        table.append('  {"%s", r_%s, %d},\n' % (m.group(1), m.group(1), len(data)))
    table.append('  {nullptr, nullptr, 0}};\n')
    open(path, 'w').write(''.join(out + table))

def compile_one(target, triple, src):
    obj = os.path.join(OUT, target, os.path.basename(src) + '.o')
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src): return obj
    r = subprocess.run([ZIG, 'c++', '-target', triple] + FLAGS + ['-c', src, '-o', obj], capture_output=True, text=True)
    if r.returncode: raise RuntimeError('%s %s:\n%s' % (target, os.path.basename(src), r.stderr[-3000:]))
    return obj

def build(target, triple, gen):
    os.makedirs(os.path.join(OUT, target), exist_ok=True)
    srcs = [os.path.join(CPP, s) for s in CORE] + [os.path.join(CPP, 'src', 'cli.cpp'), gen]
    with cf.ThreadPoolExecutor(max(2, os.cpu_count() // 2)) as ex:
        objs = list(ex.map(lambda s: compile_one(target, triple, s), srcs))
    exe = os.path.join(OUT, target, 'trcli')
    libs = ['-lpthread', '-ldl'] if 'linux' in triple else []
    r = subprocess.run([ZIG, 'c++', '-target', triple, '-O2', '-s'] + objs + libs + ['-o', exe], capture_output=True, text=True)
    if r.returncode: raise RuntimeError('%s link:\n%s' % (target, r.stderr[-3000:]))
    return exe

def fat_macho(slices, out):
    """универсальный файл Mach-O: заголовок FAT и выровненные части arm64 и x86_64"""
    CPU = {'x86_64': (0x01000007, 3), 'arm64': (0x0100000C, 0)}
    align = 14                                      # 2^14 = 16 КБ
    hdr = struct.pack('>II', 0xCAFEBABE, len(slices))
    off = 1 << align; parts = []
    for arch, data in slices:
        cpu, sub = CPU[arch]
        hdr += struct.pack('>IIIII', cpu, sub, off, len(data), align)
        parts.append((off, data)); off = (off + len(data) + (1 << align) - 1) // (1 << align) * (1 << align)
    buf = bytearray(off)
    buf[:len(hdr)] = hdr
    for o, d in parts: buf[o:o + len(d)] = d
    open(out, 'wb').write(bytes(buf))

def package(name, exe):
    os.makedirs(os.path.join(ROOT, 'dist'), exist_ok=True)
    path = os.path.join(ROOT, 'dist', name + '.tar.gz')
    with tarfile.open(path, 'w:gz') as t:
        for f, arc, mode in [(exe, 'trcli', 0o755), (os.path.join(ROOT, 'README.md'), 'README.md', 0o644), (os.path.join(ROOT, 'LICENSE'), 'LICENSE', 0o644)]:
            ti = t.gettarinfo(f, name + '/' + arc); ti.mode = mode; ti.uid = ti.gid = 0; ti.uname = ti.gname = ''
            with open(f, 'rb') as fh: t.addfile(ti, fh)
    return path

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    gen = os.path.join(OUT, 'resources_gen.cpp'); resources_cpp(gen)
    t0 = time.time(); exes = {}
    for target, triple in TARGETS.items():
        t = time.time()
        exes[target] = build(target, triple, gen)
        print('%-13s built  %6.1f MB  %4.0f s' % (target, os.path.getsize(exes[target]) / 2**20, time.time() - t), flush=True)
    uni = os.path.join(OUT, 'trcli-macos-universal')
    fat_macho([('arm64', open(exes['macos-arm64'], 'rb').read()), ('x86_64', open(exes['macos-x86_64'], 'rb').read())], uni)
    for name, exe in [('trcli-linux-x86_64', exes['linux-x86_64']), ('trcli-linux-arm64', exes['linux-arm64']), ('trcli-macos-universal', uni)]:
        p = package(name, exe)
        print('%-24s %6.1f MB  %s' % (name, os.path.getsize(p) / 2**20, p), flush=True)
    print('all done in %.0f s' % (time.time() - t0))
