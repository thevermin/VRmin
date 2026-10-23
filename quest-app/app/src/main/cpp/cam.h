// Cámaras del Quest (Passthrough Camera API de Meta, Horizon OS v74+) y lector mínimo de modelos .glb
// Se incluye desde main.cpp (usa sus LOGI/LOGE).
#pragma once
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImageReader.h>
#include <time.h>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <functional>
#include <tuple>
#include <algorithm>

// ---------------------------------------------------------------------------
// Cámaras
// ---------------------------------------------------------------------------
struct CamEye {
    bool ok = false;
    std::string id;
    ACameraDevice *dev = nullptr;
    AImageReader *rd = nullptr;
    ACameraCaptureSession *ses = nullptr;
    ACaptureRequest *req = nullptr;
    ACameraOutputTarget *tgt = nullptr;
    ACaptureSessionOutputContainer *cont = nullptr;
    ACaptureSessionOutput *out = nullptr;
    float fx = 0, fy = 0, cx = 0, cy = 0, sk = 0;   // en píxeles del sensor completo
    int sensW = 0, sensH = 0;                        // tamaño del sensor para el que valen esos datos
    int w = 0, h = 0;                                // tamaño de la imagen que pedimos
    float t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1};     // dónde está la cámara respecto de la cabeza
    bool bootClock = false;                          // las marcas de tiempo usan el reloj desde el encendido
    int errors = 0;
};
static CamEye g_cam[2];
static ACameraManager *g_camMgr = nullptr;
static char g_camStatus[220] = "sin permiso todavía";

static void camDevDisconnected(void *, ACameraDevice *) { }
static void camDevError(void *ctx, ACameraDevice *, int err) { CamEye *e = (CamEye *)ctx; e->errors++; e->ok = false; LOGE("Cámara %s: error %d", e->id.c_str(), err); }
static void camSesClosed(void *, ACameraCaptureSession *) { }
static void camSesReady(void *, ACameraCaptureSession *) { }
static void camSesActive(void *, ACameraCaptureSession *) { }

static bool camEntryF(const ACameraMetadata *m, uint32_t tag, float *out, int n) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || (int)e.count < n) return false;
    for (int i = 0; i < n; i++) out[i] = e.data.f[i];
    return true;
}
static int camEntryInt(const ACameraMetadata *m, uint32_t tag, int def) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count < 1) return def;
    switch (e.type) {
        case ACAMERA_TYPE_BYTE: return e.data.u8[0];
        case ACAMERA_TYPE_INT32: return e.data.i32[0];
        case ACAMERA_TYPE_INT64: return (int)e.data.i64[0];
        default: return def;
    }
}

/// Abre las dos cámaras de passthrough (izquierda y derecha). Hace falta el permiso de cámara del visor.
static bool camOpen() {
    if (!g_camMgr) g_camMgr = ACameraManager_create();
    if (!g_camMgr) { snprintf(g_camStatus, sizeof g_camStatus, "no hay administrador de cámaras"); return false; }
    ACameraIdList *ids = nullptr;
    if (ACameraManager_getCameraIdList(g_camMgr, &ids) != ACAMERA_OK || !ids) { snprintf(g_camStatus, sizeof g_camStatus, "no se pudo listar las cámaras"); return false; }
    int found = 0, listed = ids->numCameras;
    for (int i = 0; i < ids->numCameras; i++) {
        const char *id = ids->cameraIds[i];
        ACameraMetadata *m = nullptr;
        if (ACameraManager_getCameraCharacteristics(g_camMgr, id, &m) != ACAMERA_OK || !m) continue;
        int source = camEntryInt(m, 0x80004d00, -1), pos = camEntryInt(m, 0x80004d01, -1);
        if (source != 0 || pos < 0 || pos > 1 || g_cam[pos].ok) { ACameraMetadata_free(m); continue; }
        CamEye &E = g_cam[pos];
        E.id = id;
        float in[5];
        if (camEntryF(m, ACAMERA_LENS_INTRINSIC_CALIBRATION, in, 5)) { E.fx = in[0]; E.fy = in[1]; E.cx = in[2]; E.cy = in[3]; E.sk = in[4]; }
        camEntryF(m, ACAMERA_LENS_POSE_TRANSLATION, E.t, 3);
        camEntryF(m, ACAMERA_LENS_POSE_ROTATION, E.r, 4);
        {
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 4) {
                E.sensW = e.data.i32[2]; E.sensH = e.data.i32[3];
            }
        }
        E.bootClock = camEntryInt(m, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, 0) == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
        // Tamaño: 1280x1280 si existe (Horizon OS v83+: ve más arriba y más abajo), si no 1280x960
        int bw = 0, bh = 0, score = -1;
        {
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
                for (uint32_t k = 0; k + 3 < e.count; k += 4) {
                    int fmt = e.data.i32[k], w = e.data.i32[k + 1], h = e.data.i32[k + 2], input = e.data.i32[k + 3];
                    if (input || (fmt != AIMAGE_FORMAT_PRIVATE && fmt != AIMAGE_FORMAT_YUV_420_888)) continue;
                    int sc = (w == 1280 && h == 1280) ? 3 : (w == 1280 && h == 960) ? 2 : (w <= 1280 ? 1 : 0);
                    if (sc > score) { score = sc; bw = w; bh = h; }
                }
            }
        }
        ACameraMetadata_free(m);
        if (!bw) { bw = 1280; bh = 960; }
        if (!E.sensW) { E.sensW = bw; E.sensH = bh; }
        E.w = bw; E.h = bh;

        static ACameraDevice_StateCallbacks dcb{};
        dcb.onDisconnected = camDevDisconnected; dcb.onError = camDevError;
        ACameraDevice_StateCallbacks cb = dcb; cb.context = &E;
        camera_status_t st = ACameraManager_openCamera(g_camMgr, id, &cb, &E.dev);
        if (st != ACAMERA_OK || !E.dev) { snprintf(g_camStatus, sizeof g_camStatus, "no se pudo abrir la cámara %s (error %d)", id, (int)st); E.dev = nullptr; continue; }
        if (AImageReader_newWithUsage(bw, bh, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 5, &E.rd) != AMEDIA_OK) { E.rd = nullptr; continue; }
        ANativeWindow *win = nullptr;
        AImageReader_getWindow(E.rd, &win);
        ACaptureSessionOutputContainer_create(&E.cont);
        ACaptureSessionOutput_create(win, &E.out);
        ACaptureSessionOutputContainer_add(E.cont, E.out);
        ACameraOutputTarget_create(win, &E.tgt);
        ACameraDevice_createCaptureRequest(E.dev, TEMPLATE_PREVIEW, &E.req);
        if (!E.req) continue;
        ACaptureRequest_addTarget(E.req, E.tgt);
        static ACameraCaptureSession_stateCallbacks scb{};
        scb.onClosed = camSesClosed; scb.onReady = camSesReady; scb.onActive = camSesActive;
        st = ACameraDevice_createCaptureSession(E.dev, E.cont, &scb, &E.ses);
        if (st != ACAMERA_OK || !E.ses) { snprintf(g_camStatus, sizeof g_camStatus, "no se pudo empezar la cámara %s (error %d)", id, (int)st); continue; }
        st = ACameraCaptureSession_setRepeatingRequest(E.ses, nullptr, 1, &E.req, nullptr);
        if (st != ACAMERA_OK) { snprintf(g_camStatus, sizeof g_camStatus, "la cámara %s no manda imágenes (error %d)", id, (int)st); continue; }
        E.ok = true;
        found++;
    }
    ACameraManager_deleteCameraIdList(ids);
    if (found)
        snprintf(g_camStatus, sizeof g_camStatus, "cámaras %s%s · %dx%d · f %.0f/%.0f c %.0f/%.0f · sensor %dx%d · pos %.3f,%.3f,%.3f · rot %.3f,%.3f,%.3f,%.3f · reloj %s",
                 g_cam[0].ok ? "I" : "-", g_cam[1].ok ? "D" : "-", g_cam[0].ok ? g_cam[0].w : g_cam[1].w, g_cam[0].ok ? g_cam[0].h : g_cam[1].h,
                 g_cam[0].fx, g_cam[0].fy, g_cam[0].cx, g_cam[0].cy, g_cam[0].sensW, g_cam[0].sensH,
                 g_cam[0].t[0], g_cam[0].t[1], g_cam[0].t[2], g_cam[0].r[0], g_cam[0].r[1], g_cam[0].r[2], g_cam[0].r[3],
                 g_cam[0].bootClock ? "encendido" : "monotónico");
    else if (!strncmp(g_camStatus, "sin permiso", 11) || !strncmp(g_camStatus, "pidiendo", 8))
        snprintf(g_camStatus, sizeof g_camStatus, "no se encontraron las cámaras de passthrough (%d en la lista)", listed);
    LOGI("Cámaras: %s", g_camStatus);
    return found > 0;
}

/// Cierra las cámaras (para volver a abrirlas: después de que el visor estuvo apagado a veces dejan de mandar imágenes).
/// Antes hay que soltar las imágenes que se estén usando.
static void camClose() {
    for (CamEye &E : g_cam) {
        if (E.ses) { ACameraCaptureSession_stopRepeating(E.ses); ACameraCaptureSession_close(E.ses); E.ses = nullptr; }
        if (E.req) { if (E.tgt) ACaptureRequest_removeTarget(E.req, E.tgt); ACaptureRequest_free(E.req); E.req = nullptr; }
        if (E.tgt) { ACameraOutputTarget_free(E.tgt); E.tgt = nullptr; }
        if (E.cont) { if (E.out) ACaptureSessionOutputContainer_remove(E.cont, E.out); ACaptureSessionOutputContainer_free(E.cont); E.cont = nullptr; }
        if (E.out) { ACaptureSessionOutput_free(E.out); E.out = nullptr; }
        if (E.dev) { ACameraDevice_close(E.dev); E.dev = nullptr; }
        if (E.rd) { AImageReader_delete(E.rd); E.rd = nullptr; }
        E.ok = false;
    }
}

/// Reloj de la cámara → reloj monotónico (nanosegundos)
static int64_t camToMonotonic(const CamEye &E, int64_t ts) {
    if (!E.bootClock) return ts;
    timespec b{}, m{};
    clock_gettime(CLOCK_BOOTTIME, &b);
    clock_gettime(CLOCK_MONOTONIC, &m);
    int64_t off = ((int64_t)b.tv_sec - m.tv_sec) * 1000000000LL + ((int64_t)b.tv_nsec - m.tv_nsec);
    return ts - off;
}

// ---------------------------------------------------------------------------
// Lector mínimo de JSON y de .glb (para el modelo del control que da Meta)
// ---------------------------------------------------------------------------
struct JVal {
    enum T { Null, Num, Str, Arr, Obj, Bool } t = Null;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal *get(const char *k) const { for (auto &p : o) if (p.first == k) return &p.second; return nullptr; }
    double num(const char *k, double def) const { const JVal *v = get(k); return v && v->t == Num ? v->n : def; }
};
struct JParser {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool str(std::string &out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) { p++; if (*p == 'u') { p += 4; out += '?'; } else out += *p; p++; }
            else out += *p++;
        }
        if (p < e) p++;
        return true;
    }
    bool val(JVal &v, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.t = JVal::Obj; p++;
            for (;;) {
                ws();
                if (p < e && *p == '}') { p++; return true; }
                std::string k;
                if (!str(k)) return false;
                ws(); if (p >= e || *p != ':') return false; p++;
                v.o.emplace_back(k, JVal());
                if (!val(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.t = JVal::Arr; p++;
            for (;;) {
                ws();
                if (p < e && *p == ']') { p++; return true; }
                v.a.emplace_back();
                if (!val(v.a.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.t = JVal::Str; return str(v.s); }
        if (!strncmp(p, "true", 4)) { v.t = JVal::Bool; v.n = 1; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.t = JVal::Bool; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char *end = nullptr;
        std::string tmp(p, std::min<size_t>(64, e - p));
        v.n = strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        v.t = JVal::Num; p += end - tmp.c_str();
        return true;
    }
};

/// Malla de un .glb: todas las primitivas de triángulos, con las transformaciones de sus nodos aplicadas.
/// Devuelve posiciones (en metros, en el espacio del modelo) e índices.
static bool glbLoad(const std::vector<uint8_t> &buf, std::vector<float> &pos, std::vector<uint32_t> &idx) {
    if (buf.size() < 20 || memcmp(buf.data(), "glTF", 4)) return false;
    uint32_t jlen; memcpy(&jlen, &buf[12], 4);
    if (20 + (size_t)jlen > buf.size()) return false;
    JVal root; JParser jp{(const char *)&buf[20], (const char *)&buf[20] + jlen};
    if (!jp.val(root) || root.t != JVal::Obj) return false;
    const uint8_t *bin = nullptr; size_t binLen = 0;
    size_t off = 20 + ((jlen + 3) & ~3u);
    if (off + 8 <= buf.size()) { uint32_t bl; memcpy(&bl, &buf[off], 4); bin = &buf[off + 8]; binLen = std::min<size_t>(bl, buf.size() - off - 8); }
    const JVal *acc = root.get("accessors"), *views = root.get("bufferViews"), *meshes = root.get("meshes"), *nodes = root.get("nodes");
    if (!acc || !views || !meshes || !bin) return false;
    // lee un accessor como floats (vec3) o índices
    auto readAcc = [&](int ai, int comps, std::vector<float> *f, std::vector<uint32_t> *u) -> bool {
        if (ai < 0 || ai >= (int)acc->a.size()) return false;
        const JVal &A = acc->a[ai];
        int bv = (int)A.num("bufferView", -1), ct = (int)A.num("componentType", 0), cnt = (int)A.num("count", 0);
        if (bv < 0 || bv >= (int)views->a.size()) return false;
        const JVal &V = views->a[bv];
        size_t base = (size_t)V.num("byteOffset", 0) + (size_t)A.num("byteOffset", 0);
        int csz = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : 1;
        size_t stride = (size_t)V.num("byteStride", 0);
        if (!stride) stride = (size_t)csz * comps;
        if (base + stride * (cnt ? cnt - 1 : 0) + (size_t)csz * comps > binLen) return false;
        for (int i = 0; i < cnt; i++) {
            const uint8_t *q = bin + base + stride * i;
            for (int c = 0; c < comps; c++) {
                const uint8_t *r = q + c * csz;
                if (f) {
                    float x = 0;
                    if (ct == 5126) memcpy(&x, r, 4);
                    else if (ct == 5123) { uint16_t s; memcpy(&s, r, 2); x = s / 65535.f; }
                    else if (ct == 5122) { int16_t s; memcpy(&s, r, 2); x = std::max(-1.f, s / 32767.f); }
                    else if (ct == 5121) x = *r / 255.f;
                    else if (ct == 5120) x = std::max(-1.f, (int8_t)*r / 127.f);
                    f->push_back(x);
                } else {
                    uint32_t x = 0;
                    if (ct == 5125) memcpy(&x, r, 4);
                    else if (ct == 5123) { uint16_t s; memcpy(&s, r, 2); x = s; }
                    else x = *r;
                    u->push_back(x);
                }
            }
        }
        return true;
    };
    // matriz de un nodo (column-major 4x4)
    auto nodeMat = [](const JVal &N, float M[16]) {
        for (int i = 0; i < 16; i++) M[i] = (i % 5 == 0) ? 1.f : 0.f;
        const JVal *m = N.get("matrix");
        if (m && m->a.size() == 16) { for (int i = 0; i < 16; i++) M[i] = (float)m->a[i].n; return; }
        float t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
        if (const JVal *v = N.get("translation")) for (int i = 0; i < 3 && i < (int)v->a.size(); i++) t[i] = (float)v->a[i].n;
        if (const JVal *v = N.get("scale")) for (int i = 0; i < 3 && i < (int)v->a.size(); i++) s[i] = (float)v->a[i].n;
        if (const JVal *v = N.get("rotation")) for (int i = 0; i < 4 && i < (int)v->a.size(); i++) q[i] = (float)v->a[i].n;
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                      2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                      2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
        for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) M[c * 4 + r] = R[c * 3 + r] * s[c];
        M[12] = t[0]; M[13] = t[1]; M[14] = t[2];
    };
    auto mulM = [](const float a[16], const float b[16], float o[16]) {
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
            float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = s;
        }
    };
    auto addMesh = [&](int mi, const float M[16]) {
        if (mi < 0 || mi >= (int)meshes->a.size()) return;
        const JVal *prims = meshes->a[mi].get("primitives");
        if (!prims) return;
        for (const JVal &P : prims->a) {
            if ((int)P.num("mode", 4) != 4) continue;
            const JVal *at = P.get("attributes");
            if (!at) continue;
            std::vector<float> pp; std::vector<uint32_t> ii;
            if (!readAcc((int)at->num("POSITION", -1), 3, &pp, nullptr)) continue;
            uint32_t vn = (uint32_t)(pp.size() / 3);
            if (P.get("indices")) { if (!readAcc((int)P.num("indices", -1), 1, nullptr, &ii)) continue; }
            else for (uint32_t i = 0; i < vn; i++) ii.push_back(i);
            uint32_t b = (uint32_t)(pos.size() / 3);
            for (uint32_t i = 0; i < vn; i++) {
                float x = pp[i * 3], y = pp[i * 3 + 1], z = pp[i * 3 + 2];
                pos.push_back(M[0] * x + M[4] * y + M[8] * z + M[12]);
                pos.push_back(M[1] * x + M[5] * y + M[9] * z + M[13]);
                pos.push_back(M[2] * x + M[6] * y + M[10] * z + M[14]);
            }
            for (size_t i = 0; i + 2 < ii.size(); i += 3)
                if (ii[i] < vn && ii[i + 1] < vn && ii[i + 2] < vn) { idx.push_back(b + ii[i]); idx.push_back(b + ii[i + 1]); idx.push_back(b + ii[i + 2]); }
        }
    };
    // recorrer la escena (o todos los nodos raíz)
    std::vector<int> roots;
    if (const JVal *scenes = root.get("scenes")) {
        int sc = (int)root.num("scene", 0);
        if (sc >= 0 && sc < (int)scenes->a.size()) if (const JVal *ns = scenes->a[sc].get("nodes")) for (auto &n : ns->a) roots.push_back((int)n.n);
    }
    if (roots.empty() && nodes) for (int i = 0; i < (int)nodes->a.size(); i++) roots.push_back(i);
    std::function<void(int, const float *, int)> walk = [&](int ni, const float *parent, int depth) {
        if (!nodes || ni < 0 || ni >= (int)nodes->a.size() || depth > 32) return;
        const JVal &N = nodes->a[ni];
        float L[16], W[16]; nodeMat(N, L); mulM(parent, L, W);
        if (N.get("mesh")) addMesh((int)N.num("mesh", -1), W);
        if (const JVal *ch = N.get("children")) for (auto &c : ch->a) walk((int)c.n, W, depth + 1);
    };
    float I[16]; for (int i = 0; i < 16; i++) I[i] = (i % 5 == 0) ? 1.f : 0.f;
    for (int r : roots) walk(r, I, 0);
    if (pos.empty() && meshes) for (int i = 0; i < (int)meshes->a.size(); i++) addMesh(i, I);
    return !pos.empty() && !idx.empty();
}

/// Normales suaves (uniendo vértices repetidos en la misma posición) y la malla agrandada `grow` metros
static void smoothGrow(const std::vector<float> &pos, const std::vector<uint32_t> &idx, float grow,
                       std::vector<float> &outPos, std::vector<float> &outNrm) {
    size_t n = pos.size() / 3;
    std::map<std::tuple<long, long, long>, uint32_t> weld;
    std::vector<uint32_t> rep(n);
    for (size_t i = 0; i < n; i++) {
        auto k = std::make_tuple(lroundf(pos[i * 3] * 20000), lroundf(pos[i * 3 + 1] * 20000), lroundf(pos[i * 3 + 2] * 20000));
        auto it = weld.find(k);
        if (it == weld.end()) { weld[k] = (uint32_t)i; rep[i] = (uint32_t)i; } else rep[i] = it->second;
    }
    std::vector<float> acc(n * 3, 0.f);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const float *a = &pos[idx[t] * 3], *b = &pos[idx[t + 1] * 3], *c = &pos[idx[t + 2] * 3];
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float f[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) acc[rep[idx[t + k]] * 3 + j] += f[j];
    }
    // sentido de las normales: hacia afuera (lejos del centro del modelo), por si los triángulos vienen al revés
    float ctr[3] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) for (int j = 0; j < 3; j++) ctr[j] += pos[i * 3 + j] / (float)std::max<size_t>(1, n);
    double out = 0;
    for (size_t i = 0; i < n; i++) if (rep[i] == i) for (int j = 0; j < 3; j++) out += acc[i * 3 + j] * (pos[i * 3 + j] - ctr[j]);
    float sgn = out < 0 ? -1.f : 1.f;
    outPos.resize(n * 3); outNrm.resize(n * 3);
    for (size_t i = 0; i < n; i++) {
        const float *g = &acc[rep[i] * 3];
        float l = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        float nn[3] = {0, 0, 0};
        if (l > 1e-12f) for (int j = 0; j < 3; j++) nn[j] = g[j] / l * sgn;
        for (int j = 0; j < 3; j++) { outNrm[i * 3 + j] = nn[j]; outPos[i * 3 + j] = pos[i * 3 + j] + nn[j] * grow; }
    }
}
