// Imágenes en el procesador: achicar el borde de cada ojo (foveated encoding) y pasar a NV12 para el codificador
#include "host.h"
#include <cmath>
#include <condition_variable>
#include <thread>

// ---------------- Hilos para repartir filas ----------------
namespace {
struct Pool {
    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable cv, done;
    const std::function<void(int, int)> *fn = nullptr;
    int rows = 0, parts = 0, next = 0, left = 0;
    uint64_t gen = 0;
    Pool() {
        int n = (int)std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        for (int i = 0; i < n - 1; i++) th.emplace_back([this] { loop(); });
        parts = n;
    }
    void loop() {
        uint64_t seen = 0;
        while (true) {
            std::unique_lock<std::mutex> g(m);
            cv.wait(g, [&] { return gen != seen && next < parts; });
            seen = gen;
            work(g);
        }
    }
    void work(std::unique_lock<std::mutex> &g) {
        while (next < parts) {
            int k = next++;
            int y0 = rows * k / parts, y1 = rows * (k + 1) / parts;
            auto f = fn;
            g.unlock();
            if (y1 > y0) (*f)(y0, y1);
            g.lock();
            if (--left == 0) done.notify_all();
        }
    }
    void run(int r, const std::function<void(int, int)> &f) {
        std::unique_lock<std::mutex> g(m);
        fn = &f; rows = r; next = 0; left = parts; gen++;
        cv.notify_all();
        work(g);
        done.wait(g, [&] { return left == 0; });
    }
};
std::mutex poolUse;
}
void parallelRows(int rows, const std::function<void(int, int)> &fn) {
    static Pool *pool = new Pool();
    std::lock_guard<std::mutex> g(poolUse);
    pool->run(rows, fn);
}

// ---------------- BGRA -> NV12 (BT.709, rango limitado: lo mismo que marca el codificador) ----------------
void bgraToNv12(const uint8_t *bgra, int stride, int w, int h, uint8_t *nv12, bool rgba) {
    const int bi = rgba ? 2 : 0, ri = rgba ? 0 : 2;
    uint8_t *Y = nv12, *UV = nv12 + (size_t)w * h;
    parallelRows((h + 1) / 2, [&](int r0, int r1) {
        for (int r = r0; r < r1; r++) {
            int y = r * 2;
            const uint8_t *s0 = bgra + (size_t)y * stride, *s1 = y + 1 < h ? s0 + stride : s0;
            uint8_t *y0 = Y + (size_t)y * w, *y1 = y + 1 < h ? y0 + w : nullptr, *uv = UV + (size_t)r * w;
            for (int x = 0; x < w; x += 2) {
                int sr = 0, sg = 0, sb = 0;
                for (int k = 0; k < 2 && x + k < w; k++) {
                    const uint8_t *p = s0 + (x + k) * 4, *q = s1 + (x + k) * 4;
                    int R = p[ri], G = p[1], B = p[bi];
                    y0[x + k] = (uint8_t)((47 * R + 157 * G + 16 * B + 4096 + 128) >> 8);
                    if (y1) { int R2 = q[ri], G2 = q[1], B2 = q[bi]; y1[x + k] = (uint8_t)((47 * R2 + 157 * G2 + 16 * B2 + 4096 + 128) >> 8); sr += R2; sg += G2; sb += B2; }
                    else { sr += R; sg += G; sb += B; }
                    sr += R; sg += G; sb += B;
                }
                sr >>= 2; sg >>= 2; sb >>= 2;
                int U = ((-26 * sr - 87 * sg + 112 * sb + 128) >> 8) + 128, V = ((112 * sr - 102 * sg - 10 * sb + 128) >> 8) + 128;
                uv[x] = (uint8_t)std::min(240, std::max(16, U));
                if (x + 1 < w) uv[x + 1] = (uint8_t)std::min(240, std::max(16, V));
            }
        }
    });
}

// ---------------- Cada ojo a la imagen que se manda ----------------
// Para cada columna (y fila) de la imagen que se codifica: de qué punto de la imagen del juego sale y cuánto abarca.
// Con foveated encoding el borde abarca más de un punto del juego: se promedian varios (si no, el borde titila).
struct Map1D { std::vector<uint8_t> n; std::vector<int> i[2]; std::vector<uint16_t> f[2]; };
static Map1D makeMap(const std::vector<float> *lut, int outN, int srcN) {
    Map1D m;
    m.n.resize(outN);
    for (int k = 0; k < 2; k++) { m.i[k].resize(outN); m.f[k].resize(outN); }
    auto at = [&](double u) -> double {   // 0..1 codificado -> 0..1 del juego
        if (!lut) return u;
        double x = u * 1023.0;
        int i = (int)std::floor(x);
        if (i < 0) return (*lut)[0];
        if (i >= 1023) return (*lut)[1023];
        double t = x - i;
        return (*lut)[i] * (1 - t) + (*lut)[i + 1] * t;
    };
    for (int o = 0; o < outN; o++) {
        double u = (o + 0.5) / outN;
        double s = at(u) * srcN - 0.5;
        double span = (at(std::min(1.0, u + 0.5 / outN)) - at(std::max(0.0, u - 0.5 / outN))) * srcN;
        int taps = span > 1.6 ? 2 : 1;
        m.n[o] = (uint8_t)taps;
        for (int k = 0; k < taps; k++) {
            double v = taps == 1 ? s : s + (k ? 0.25 : -0.25) * span;
            v = std::min((double)srcN - 1.001, std::max(0.0, v));
            int i = (int)std::floor(v);
            m.i[k][o] = i;
            m.f[k][o] = (uint16_t)((v - i) * 256);
        }
    }
    return m;
}

void warpEyes(const uint8_t *src, int ew, int eh, bool rgba, const Stream &st, std::vector<uint8_t> &out) {
    static std::mutex cacheLock;
    static std::string cacheKey;
    static Map1D mx[2], my;
    int OW = st.W / 2, OH = st.H;
    {
        std::lock_guard<std::mutex> g(cacheLock);
        char key[160];
        snprintf(key, sizeof key, "%d,%d,%d,%d,%s", ew, eh, OW, OH, st.fov.on() ? st.fov.text().c_str() : "0");
        if (cacheKey != key) {
            cacheKey = key;
            for (int e = 0; e < 2; e++) {
                std::vector<float> l;
                if (st.fov.on()) l = st.fov.forward(st.fov.cx[e]);
                mx[e] = makeMap(st.fov.on() ? &l : nullptr, OW, ew);
            }
            std::vector<float> l;
            if (st.fov.on()) l = st.fov.forward(st.fov.cy);
            my = makeMap(st.fov.on() ? &l : nullptr, OH, eh);
        }
    }
    out.resize((size_t)st.W * st.H * 4);
    const size_t sstride = (size_t)ew * 2 * 4;
    const int bi = rgba ? 2 : 0, ri = rgba ? 0 : 2;
    parallelRows(OH, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const int ty = my.n[y];
            uint8_t *d = out.data() + (size_t)y * st.W * 4;
            for (int e = 0; e < 2; e++) {
                const Map1D &M = mx[e];
                const uint8_t *base = src + (size_t)e * ew * 4;
                for (int x = 0; x < OW; x++) {
                    const int tx = M.n[x];
                    uint32_t acc0 = 0, acc1 = 0, acc2 = 0;
                    for (int a = 0; a < ty; a++) {
                        const int yy = my.i[a][y], fy = my.f[a][y];
                        const uint8_t *r0 = base + (size_t)yy * sstride, *r1 = yy + 1 < eh ? r0 + sstride : r0;
                        for (int b = 0; b < tx; b++) {
                            const int xx = M.i[b][x], fx = M.f[b][x];
                            const uint8_t *p00 = r0 + xx * 4, *p10 = r1 + xx * 4;
                            const uint8_t *p01 = xx + 1 < ew ? p00 + 4 : p00, *p11 = xx + 1 < ew ? p10 + 4 : p10;
                            const int w00 = (256 - fx) * (256 - fy), w01 = fx * (256 - fy), w10 = (256 - fx) * fy, w11 = fx * fy;
                            acc0 += (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 16;
                            acc1 += (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 16;
                            acc2 += (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 16;
                        }
                    }
                    const int cnt = tx * ty;
                    uint32_t c[3] = {acc0, acc1, acc2};
                    uint8_t *o = d + ((size_t)e * OW + x) * 4;
                    if (cnt == 1) { o[0] = (uint8_t)c[bi]; o[1] = (uint8_t)c[1]; o[2] = (uint8_t)c[ri]; }
                    else { o[0] = (uint8_t)(c[bi] / cnt); o[1] = (uint8_t)(c[1] / cnt); o[2] = (uint8_t)(c[ri] / cnt); }
                    o[3] = 255;
                }
            }
        }
    });
}
