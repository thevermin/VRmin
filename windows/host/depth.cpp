// 3D automático: una IA (Depth Anything V2 small, en ONNX) calcula la profundidad de cada imagen del escritorio o de una
// ventana y el Quest la usa para darle relieve. Corre con ONNX Runtime en la placa de video (DirectML; si no se puede,
// en el procesador). El modelo se baja la primera vez (≈50 MB). Al Quest: paquete 9, la profundidad comprimida.
#include "host.h"
#include <map>
#include <set>
#include <thread>
#include <condition_variable>
#include <urlmon.h>
#include <bcrypt.h>
#ifndef _stdcall
#define _stdcall __stdcall   // (la cabecera de ONNX Runtime lo usa; mingw solo define __stdcall)
#endif
#include "ort_sal.h"
#include "onnxruntime_c_api.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

static const wchar_t *MODEL_URL = L"https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/onnx/model_fp16.onnx";
static const char *MODEL_SHA256 = "2df6223f206b5164e21f664ace61dabeb9bb6a49b8b5a3e00510b4807d0f5b04";
static const int IN_W = 518, IN_H = 392;   // (múltiplos de 14, como pide el modelo; lo mismo que en la Mac)

static std::mutex lock;
static std::condition_variable cv;
static bool g_enabled = false;                 // escritorio (la pantalla principal)
static std::set<uint32_t> g_windows;           // ventanas con su propio 3D
static std::map<uint32_t, uint64_t> lastRunBy, cacheAt;
static std::map<uint32_t, std::pair<float, float>> range;
// Última imagen (ya achicada para la IA) de cada pantalla y ventana: al activar el 3D se usa enseguida
static std::map<uint32_t, std::vector<uint8_t>> cache;
static std::string g_status;
static bool loading = false, busy = false;
static uint32_t jobSid = 0;
static bool jobPending = false;
static std::vector<uint8_t> jobImg;

static const OrtApi *ort;
static OrtEnv *env;
static OrtSession *session;
static bool ready = false;

static void setStatus(const std::string &s) { std::lock_guard<std::mutex> g(lock); g_status = s; }
std::string depthStatus() { std::lock_guard<std::mutex> g(lock); return g_status; }
bool depthEnabled() { std::lock_guard<std::mutex> g(lock); return g_enabled; }

static std::string sha256File(const std::wstring &path) {
    std::string data = readFile(path);
    BCRYPT_ALG_HANDLE alg;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    uint8_t h[32];
    BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
    return hex;
}

static bool check(OrtStatus *s, const char *what) {
    if (!s) return true;
    logmsg("[3D] %s: %s", what, ort->GetErrorMessage(s));
    ort->ReleaseStatus(s);
    return false;
}

/// Achica una imagen BGRA a lo que pide la IA (promedio de 2×2 alrededor de cada punto)
static void shrink(const uint8_t *src, int w, int h, std::vector<uint8_t> &out) {
    out.resize((size_t)IN_W * IN_H * 4);
    for (int y = 0; y < IN_H; y++) {
        int sy = std::min(h - 2, (int)((y + 0.5) * h / IN_H));
        for (int x = 0; x < IN_W; x++) {
            int sx = std::min(w - 2, (int)((x + 0.5) * w / IN_W));
            const uint8_t *a = src + ((size_t)sy * w + sx) * 4, *b = a + (size_t)w * 4;
            uint8_t *o = &out[((size_t)y * IN_W + x) * 4];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)((a[c] + a[c + 4] + b[c] + b[c + 4]) >> 2);
            o[3] = 255;
        }
    }
}

/// Profundidad de una imagen ya achicada → al Quest
static void run(uint32_t sid, const std::vector<uint8_t> &img) {
    std::pair<float, float> r;
    { std::lock_guard<std::mutex> g(lock); auto it = range.find(sid); r = it == range.end() ? std::make_pair(0.f, 1.f) : it->second; }
    // (normalizada como pide el modelo: ImageNet, en planos R, G, B)
    std::vector<float> in((size_t)3 * IN_W * IN_H);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
    size_t plane = (size_t)IN_W * IN_H;
    for (size_t i = 0; i < plane; i++)
        for (int c = 0; c < 3; c++) in[c * plane + i] = (img[i * 4 + (2 - c)] / 255.f - mean[c]) / sd[c];
    OrtMemoryInfo *mi = nullptr;
    if (!check(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi), "memory")) return;
    int64_t shape[4] = {1, 3, IN_H, IN_W};
    OrtValue *x = nullptr, *y = nullptr;
    bool ok = check(ort->CreateTensorWithDataAsOrtValue(mi, in.data(), in.size() * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &x), "input");
    ort->ReleaseMemoryInfo(mi);
    const char *inName = "pixel_values", *outName = "predicted_depth";
    ok = ok && check(ort->Run(session, nullptr, &inName, &x, 1, &outName, 1, &y), "run");
    if (x) ort->ReleaseValue(x);
    if (!ok) { if (y) ort->ReleaseValue(y); return; }
    OrtTensorTypeAndShapeInfo *ti = nullptr;
    size_t nd = 0;
    int64_t dims[4] = {0, 0, 0, 0};
    float *vals = nullptr;
    check(ort->GetTensorTypeAndShape(y, &ti), "output");
    check(ort->GetDimensionsCount(ti, &nd), "output");
    if (nd >= 2 && nd <= 4) check(ort->GetDimensions(ti, dims, nd), "output");
    ort->ReleaseTensorTypeAndShapeInfo(ti);
    check(ort->GetTensorMutableData(y, (void **)&vals), "output");
    int h = nd >= 2 ? (int)dims[nd - 2] : 0, w = nd >= 2 ? (int)dims[nd - 1] : 0;
    if (w > 1 && h > 1 && vals) {
        // Normalizar (1 = cerca) con límites que cambian despacio para que no «respire»
        float mn = 1e30f, mx = -1e30f;
        for (int i = 0; i < w * h; i++) { mn = std::min(mn, vals[i]); mx = std::max(mx, vals[i]); }
        float lo = r.first + (mn - r.first) * 0.3f, hi = r.second + (mx - r.second) * 0.3f;
        { std::lock_guard<std::mutex> g(lock); range[sid] = {lo, hi}; }
        float span = std::max(hi - lo, 1e-4f);
        // A la mitad de tamaño alcanza (el relieve es suave)
        int ow = w / 2, oh = h / 2;
        std::vector<uint8_t> px((size_t)ow * oh);
        for (int yy = 0; yy < oh; yy++)
            for (int xx = 0; xx < ow; xx++) px[(size_t)yy * ow + xx] = (uint8_t)std::max(0.f, std::min(255.f, (vals[(size_t)(yy * 2) * w + xx * 2] - lo) / span * 255));
        size_t zn = 0;
        if (!questConnected()) return ort->ReleaseValue(y);
        void *z = tdefl_compress_mem_to_heap(px.data(), px.size(), &zn, 128);   // (deflate «crudo», sin cabecera: lo que espera el Quest)
        if (z && zn) {
            std::vector<uint8_t> p;
            p.push_back(9);
            p.push_back(sid == 0 ? 0 : 1);
            if (sid) for (int s = 24; s >= 0; s -= 8) p.push_back((uint8_t)(sid >> s));
            p.push_back((uint8_t)(ow >> 8)); p.push_back((uint8_t)ow);
            p.push_back((uint8_t)(oh >> 8)); p.push_back((uint8_t)oh);
            uint32_t raw = (uint32_t)px.size();
            for (int s = 24; s >= 0; s -= 8) p.push_back((uint8_t)(raw >> s));
            p.insert(p.end(), (uint8_t *)z, (uint8_t *)z + zn);
            questSend(std::move(p));
        }
        mz_free(z);
    }
    ort->ReleaseValue(y);
}

static void worker() {
    while (true) {
        uint32_t sid;
        std::vector<uint8_t> img;
        {
            std::unique_lock<std::mutex> g(lock);
            cv.wait(g, [] { return jobPending; });
            jobPending = false;
            sid = jobSid;
            img.swap(jobImg);
        }
        run(sid, img);
        std::lock_guard<std::mutex> g(lock);
        busy = false;
    }
}

/// Calcula ya la profundidad de la última imagen guardada (si la IA está lista y libre). Con lock tomado.
static void kickLocked(uint32_t sid) {
    auto it = cache.find(sid);
    if (!ready || busy || it == cache.end()) return;
    busy = true;
    lastRunBy[sid] = nowNs();
    jobSid = sid;
    jobImg = it->second;
    jobPending = true;
    cv.notify_one();
}

/// Carga ONNX Runtime (junto a VRmin Host, en «ai») y el modelo (se baja la primera vez)
static void load() {
    std::wstring dir = dataDir() + L"\\ai", model = dir + L"\\depth-anything-v2-small-fp16.onnx";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES || sha256File(model) != MODEL_SHA256) {
        setStatus(tr("Bajando la IA de 3D (≈50 MB, solo la primera vez)…", "Downloading the 3D AI (≈50 MB, first time only)…"));
        std::wstring tmp = model + L".part";
        DeleteFileW(tmp.c_str());
        if (FAILED(URLDownloadToFileW(nullptr, MODEL_URL, tmp.c_str(), 0, nullptr))) {
            setStatus(tr("No se pudo bajar la IA de 3D. ¿Hay internet?", "Couldn't download the 3D AI. Is the PC online?"));
            return;
        }
        if (sha256File(tmp) != MODEL_SHA256) {
            DeleteFileW(tmp.c_str());
            setStatus(tr("La IA de 3D bajada está dañada: probá de nuevo", "The downloaded 3D AI is damaged: try again"));
            return;
        }
        DeleteFileW(model.c_str());
        MoveFileW(tmp.c_str(), model.c_str());
    }
    setStatus(tr("Preparando la IA de 3D…", "Preparing the 3D AI…"));
    std::wstring lib = exeDir() + L"\\ai\\onnxruntime.dll";
    HMODULE m = LoadLibraryExW(lib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // (y DirectML.dll de la misma carpeta)
    typedef const OrtApiBase *(ORT_API_CALL * GetBase)();
    GetBase gb = m ? (GetBase)(void *)GetProcAddress(m, "OrtGetApiBase") : nullptr;
    ort = gb ? gb()->GetApi(ORT_API_VERSION) : nullptr;
    if (!ort) { setStatus(tr("Falta ONNX Runtime junto a VRmin Host (carpeta ai)", "ONNX Runtime is missing next to VRmin Host (ai folder)")); return; }
    if (!check(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "vrmin", &env), "env")) { setStatus(tr("No se pudo preparar el 3D", "Couldn't prepare 3D")); return; }
    // Primero en la placa de video (DirectML); si no, en el procesador
    const char *where = "CPU";
    for (int gpu = 1; gpu >= 0 && !session; gpu--) {
        OrtSessionOptions *so = nullptr;
        check(ort->CreateSessionOptions(&so), "options");
        check(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "options");
        if (gpu) {
            typedef OrtStatus *(ORT_API_CALL * AppendDml)(OrtSessionOptions *, int);
            AppendDml add = (AppendDml)(void *)GetProcAddress(m, "OrtSessionOptionsAppendExecutionProvider_DML");
            check(ort->DisableMemPattern(so), "options");
            check(ort->SetSessionExecutionMode(so, ORT_SEQUENTIAL), "options");
            if (!add || !check(add(so, 0), "DirectML")) { ort->ReleaseSessionOptions(so); continue; }
            where = "DirectML";
        } else {
            where = "CPU";
            check(ort->SetIntraOpNumThreads(so, std::max(1, (int)std::thread::hardware_concurrency() / 2)), "options");
        }
        if (!check(ort->CreateSession(env, model.c_str(), so, &session), gpu ? "session (DirectML)" : "session (CPU)")) session = nullptr;
        ort->ReleaseSessionOptions(so);
    }
    if (!session) { setStatus(tr("No se pudo preparar el 3D", "Couldn't prepare 3D")); return; }
    // Una pasada de prueba: la primera tarda más (prepara la placa de video)
    std::vector<uint8_t> warm((size_t)IN_W * IN_H * 4, 128);
    uint64_t t0 = nowNs();
    run(0xFFFFFFFF, warm);
    {
        std::lock_guard<std::mutex> g(lock);
        range.erase(0xFFFFFFFF);
        ready = true;
        g_status.clear();
        std::thread(worker).detach();
        // Lo que ya estaba pedido mientras cargaba
        if (g_enabled) kickLocked(0);
        else if (!g_windows.empty()) kickLocked(*g_windows.begin());
    }
    logmsg("[3D] Depth AI ready (%dx%d, %s, first pass %.0f ms)", IN_W, IN_H, where, (nowNs() - t0) / 1e6);
}

void depthPrepare() {
    {
        std::lock_guard<std::mutex> g(lock);
        if (ready || loading) return;
        loading = true;
    }
    std::thread([] {
        load();
        std::lock_guard<std::mutex> g(lock);
        loading = false;
    }).detach();
}

void depthSetDesktop(bool on) {
    std::lock_guard<std::mutex> g(lock);
    bool was = g_enabled;
    g_enabled = on;
    if (on && !was) kickLocked(0);
}
void depthSetWindow(uint32_t sid, bool on) {
    std::lock_guard<std::mutex> g(lock);
    bool was = g_windows.count(sid) > 0;
    if (on) g_windows.insert(sid);
    else { g_windows.erase(sid); lastRunBy.erase(sid); range.erase(sid); }
    if (on && !was) kickLocked(sid);
}

/// Una imagen nueva del escritorio (sid 0) o de una ventana (BGRA): calcular su profundidad (hasta 15 por segundo cada
/// una; la IA hace una por vez)
void depthSubmit(uint32_t sid, const uint8_t *bgra, int w, int h) {
    if (w < 16 || h < 16) return;
    uint64_t now = nowNs();
    std::unique_lock<std::mutex> g(lock);
    if (!ready) return;
    bool on = sid == 0 ? g_enabled : g_windows.count(sid) > 0;
    if (!on) {
        // Sin 3D: guardar de vez en cuando una copia chiquita, para arrancar al instante cuando se active
        if (now - cacheAt[sid] < 500000000ull) return;
        cacheAt[sid] = now;
        g.unlock();
        std::vector<uint8_t> small;
        shrink(bgra, w, h, small);
        g.lock();
        cache[sid].swap(small);
        return;
    }
    if (busy || now - lastRunBy[sid] < 66000000ull) return;
    busy = true;
    lastRunBy[sid] = now;
    g.unlock();
    std::vector<uint8_t> small;
    shrink(bgra, w, h, small);
    g.lock();
    cache[sid] = small;
    jobSid = sid;
    jobImg.swap(small);
    jobPending = true;
    cv.notify_one();
}

/// Prueba (en Windows): baja el modelo, calcula la profundidad de una imagen y anota cuánto tarda
void depthSelfTest() {
    load();
    if (!ready) { logmsg("[selftest] 3D: not ready (%s)", depthStatus().c_str()); return; }
    std::vector<uint8_t> img((size_t)1280 * 720 * 4);
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 1280; x++) {
            uint8_t *p = &img[((size_t)y * 1280 + x) * 4];
            bool box = x > 400 && x < 880 && y > 200 && y < 520;
            p[0] = box ? 40 : (uint8_t)(y / 3); p[1] = box ? 200 : 90; p[2] = box ? 60 : (uint8_t)(x / 5); p[3] = 255;
        }
    std::vector<uint8_t> small;
    shrink(img.data(), 1280, 720, small);
    uint64_t t0 = nowNs();
    for (int i = 0; i < 5; i++) run(1, small);
    auto r = range[1];
    logmsg("[selftest] 3D: %.0f ms per image, depth range %.3f..%.3f", (nowNs() - t0) / 5e6, r.first, r.second);
}
