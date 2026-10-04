# Декодер телетекста на видеокарте (OpenCL): тот же алгоритм, что
# vbidecode.decode_fast (Витерби по шаблонам, 2^(2W+1) состояний), но тысячи строк
# сразу — одна рабочая группа на пару (строка, смещение), один поток на состояние.
#   import vbidecode_gpu as G
#   if G.available(): dec = G.GPUDecoder(); data, offs, mets = dec.decode(lines)   # lines: (N, 2048)
# Шаблоны берутся из vbidecode (V.load_zoned / V.set_zoned) в момент создания
# GPUDecoder или вызова dec.set_templates().
import os
import numpy as np
import vbidecode as V

KERNEL = r'''
#define NS   %(NS)d
#define TW   %(TW)d          /* 2W */
#define NX   6
#define NPH  %(NPH)d
#define NZ   %(NZ)d
#define SPL  %(SPL)d
#define NSTEP %(NSTEP)d     /* шагов Витерби от W до NTOT */
#define NWORD (NS / 32)

__kernel void viterbi(__global const float *Y,      /* (строк, SPL) нормированные строки */
                      __global const int   *tline,  /* строка задачи */
                      __global const float *toff,   /* смещение задачи */
                      __global const float *T,      /* (NZ, NPH, NS, NX) */
                      __global const float *T2,     /* (NZ, NPH, NS, 2) */
                      __global const int   *zone,   /* зона для шага */
                      __global const int   *kmask,  /* известные биты состояния на шаге */
                      __global const int   *kval,
                      const int nsteps,             /* сколько шагов считать (часть строки) */
                      const int trace,
                      const float spb,
                      __global uint  *back,         /* (задач, NSTEP, NWORD) */
                      __global float *out_met,
                      __global uchar *out_bits)     /* (задач, NSTEP + TW + 1) */
{
    const int task = get_group_id(0), s = get_local_id(0);
    __local float met[2][NS];
    __local float yw[NX]; __local int geo[3];
    __local uint bits[NWORD];
    __local float rmin[NS]; __local int rarg[NS];
    const float off = toff[task];
    __global const float *y = Y + (size_t)tline[task] * SPL;
    int cur = 0;
    for (int st = 0; st < nsteps; st++) {
        const int j = st + TW / 2;
        if (s == 0) {
            float x0 = off + j * spb; int xs = (int)ceil(x0);
            int ph = ((int)((xs - x0) * NPH)) %% NPH; if (ph < 0) ph += NPH;
            int n = (int)ceil(off + (j + 1) * spb) - xs;
            geo[0] = xs; geo[1] = ph; geo[2] = n;
        }
        if (s < NWORD) bits[s] = 0;
        barrier(CLK_LOCAL_MEM_FENCE);
        if (s < NX) {
            int xs = geo[0] + s;
            yw[s] = (s < geo[2] && xs >= 0 && xs < SPL) ? y[xs] : 0.0f;
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        const int z = zone[st], ph = geo[1], n = geo[2];
        __global const float *t = T + (((size_t)(z * NPH + ph) * NS + s) * NX);
        float ysq = 0.0f, dot = 0.0f;
        for (int k = 0; k < NX; k++) { ysq += yw[k] * yw[k]; dot += yw[k] * t[k]; }
        float bm = ysq - 2.0f * dot + T2[((size_t)(z * NPH + ph) * NS + s) * 2 + (n - 5)];
        if ((s & kmask[st]) != kval[st]) bm = 1e9f;
        if (st == 0) {
            met[cur][s] = bm;
        } else {
            float m0 = met[cur][s >> 1], m1 = met[cur][(s >> 1) | (NS >> 1)];
            int ch = m1 < m0;
            met[cur ^ 1][s] = (ch ? m1 : m0) + bm;
            if (trace && ch) atomic_or(&bits[s >> 5], 1u << (s & 31));
            cur ^= 1;
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        if (trace && st > 0 && s < NWORD) back[((size_t)task * NSTEP + st) * NWORD + s] = bits[s];
    }
    rmin[s] = met[cur][s]; rarg[s] = s;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int h = NS / 2; h > 0; h >>= 1) {
        if (s < h && (rmin[s + h] < rmin[s] || (rmin[s + h] == rmin[s] && rarg[s + h] < rarg[s]))) {
            rmin[s] = rmin[s + h]; rarg[s] = rarg[s + h];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (s == 0) {
        out_met[task] = rmin[0];
        if (trace) {
            /* обратный проход: как vbidecode.viterbi_multi; бит k хранится в out[k] */
            __global uchar *o = out_bits + (size_t)task * (NSTEP + TW + 1);
            int q = rarg[0];
            for (int st = nsteps - 1; st > 0; st--) {
                o[st + TW] = q & 1;
                uint w = back[((size_t)task * NSTEP + st) * NWORD + (q >> 5)];
                q = (q >> 1) | (((w >> (q & 31)) & 1) ? (1 << TW) : 0);
            }
            for (int d = 0; d <= TW; d++) o[d] = (q >> (TW - d)) & 1;
        }
    }
}
'''

def available():
    """Есть ли видеокарта с OpenCL (и pyopencl)."""
    try:
        import pyopencl as cl
        return any(d.type & cl.device_type.GPU for p in cl.get_platforms() for d in p.get_devices())
    except Exception:
        return False

class GPUDecoder:
    def __init__(self, batch=4096):
        import pyopencl as cl
        self.cl = cl
        dev = next(d for p in cl.get_platforms() for d in p.get_devices() if d.type & cl.device_type.GPU)
        self.name = dev.name
        self.ctx = cl.Context([dev]); self.q = cl.CommandQueue(self.ctx)
        self.nstep = V.NTOT - V.W
        src = KERNEL % dict(NS=V.NS, TW=2 * V.W, NPH=V.NPH, NZ=V.NZ, SPL=2048, NSTEP=self.nstep)
        self.prg = cl.Program(self.ctx, src).build()
        self.knl = cl.Kernel(self.prg, 'viterbi')
        # шаги Витерби: зона и известные биты (преамбула, пустота после пакета)
        zone = np.array([V.zone_of(j) for j in range(V.W, V.NTOT)], np.int32)
        km = np.zeros(self.nstep, np.int32); kv = np.zeros(self.nstep, np.int32)
        for st, j in enumerate(range(V.W, V.NTOT)):
            for d in range(-V.W, V.W + 1):
                k = j + d
                if k < 24 or k >= V.NTOT:
                    bit = 2 * V.W - (d + V.W)                     # старший бит состояния = j-W
                    km[st] |= 1 << bit
                    if (V.ALLB[k] if k < 24 else 0): kv[st] |= 1 << bit
        mf = cl.mem_flags
        self.b_zone = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=zone)
        self.b_km = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=km)
        self.b_kv = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=kv)
        self.batch = batch
        self.set_templates()

    def set_templates(self):
        cl = self.cl; mf = cl.mem_flags
        T = np.ascontiguousarray(V._ZT, np.float32)
        c = np.cumsum(V._ZT.astype(np.float64) ** 2, axis=3)
        T2 = np.ascontiguousarray(np.stack([c[..., 4], c[..., 5]], -1), np.float32)
        self.b_T = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=T)
        self.b_T2 = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=T2)

    def _run(self, b_Y, tline, toff, nsteps, trace):
        cl = self.cl; mf = cl.mem_flags; nt = len(tline)
        tl = np.ascontiguousarray(tline, np.int32); to = np.ascontiguousarray(toff, np.float32)
        b_tl = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=tl)
        b_to = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=to)
        nw = V.NS // 32; obl = self.nstep + 2 * V.W + 1
        b_back = cl.Buffer(self.ctx, mf.READ_WRITE, size=max(4, nt * self.nstep * nw * 4 if trace else 4))
        b_met = cl.Buffer(self.ctx, mf.WRITE_ONLY, size=nt * 4)
        b_bits = cl.Buffer(self.ctx, mf.WRITE_ONLY, size=max(1, nt * obl if trace else 1))
        self.knl(self.q, (nt * V.NS,), (V.NS,), b_Y, b_tl, b_to, self.b_T, self.b_T2, self.b_zone, self.b_km, self.b_kv,
                 np.int32(nsteps), np.int32(trace), np.float32(V.SPB), b_back, b_met, b_bits)
        met = np.empty(nt, np.float32); cl.enqueue_copy(self.q, met, b_met)
        bits = None
        if trace:
            bits = np.empty((nt, obl), np.uint8); cl.enqueue_copy(self.q, bits, b_bits)
        return met, bits

    def decode(self, lines, lo=113.0, hi=121.0):
        """lines: (N, 2048) -> (байты (N, 42), смещения (N,), метрики на отсчёт (N,)) — как decode_fast."""
        cl = self.cl; mf = cl.mem_flags
        lines = np.asarray(lines, np.float32); N = len(lines)
        out_b = np.zeros((N, 42), np.uint8); out_o = np.zeros(N); out_m = np.zeros(N)
        offs = np.arange(lo, hi + 0.01, 0.5); K = len(offs)
        for a in range(0, N, self.batch):
            y = lines[a:a + self.batch]; n = len(y)
            y = y - y.mean(1, keepdims=True); sd = y.std(1, keepdims=True); y = y / np.where(sd > 1e-6, sd, 1)
            b_Y = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=np.ascontiguousarray(y, np.float32))
            # 1) смещение по началу строки (преамбула + PREFIX бит)
            met, _ = self._run(b_Y, np.repeat(np.arange(n), K), np.tile(offs, n), 24 + V.PREFIX - V.W, 0)
            o = offs[met.reshape(n, K).argmin(1)]
            # 2) полный Витерби для лучшего смещения и соседних ±0,25
            cand = (o[:, None] + np.array([-0.25, 0, 0.25])[None, :]).ravel()
            met, bits = self._run(b_Y, np.repeat(np.arange(n), 3), cand, self.nstep, 1)
            k = met.reshape(n, 3).argmin(1); sel = np.arange(n) * 3 + k
            data = bits[sel][:, 24:V.NTOT]
            out_b[a:a + n] = np.packbits(data.reshape(n, -1, 8)[:, :, ::-1], axis=2).reshape(n, 42)
            out_o[a:a + n] = cand[sel]; out_m[a:a + n] = met[sel] / (V.NTOT * V.SPB)
        return out_b, out_o, out_m
