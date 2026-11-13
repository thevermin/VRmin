// Conexión con el juego: el runtime de OpenXR de VRmin (la misma DLL que en la Mac) se conecta a 127.0.0.1:9002.
// Le mandamos las poses del Quest (líneas JSON); nos manda cada cuadro (por memoria compartida o por la red).
#include "host.h"
#include <condition_variable>
#include <map>
#include <thread>

std::atomic<bool> g_halfRate{false};
bool questTakeKey();
bool lobbyShowsDesk();

static std::mutex cLock;                    // conexiones de control (las que reciben poses)
static std::map<SOCKET, std::string> clients;   // socket -> lo que quedó sin mandar
static std::atomic<uint64_t> lastGameFrame{0};
static std::atomic<int> openConns{0};

bool gameActive() { uint64_t t = lastGameFrame; return t && nowNs() - t < 500000000ull; }

static std::string configLine(bool hello) {
    Stream st = currentStream();
    return profilesConfigLine(g_halfRate ? st.fps / 2 : st.fps, hello);
}

/// Nunca espera a nadie: si un juego no lee, lo suyo se junta aparte (y lo viejo se descarta)
static void sendTo(SOCKET s, const std::string &line) {
    auto it = clients.find(s);
    if (it == clients.end()) return;
    std::string &buf = it->second;
    if (buf.size() > 64 * 1024) buf.clear();   // (lleva rato sin leer: la próxima pose completa reemplaza a todo)
    buf += line;
    buf += '\n';
    int n = send(s, buf.data(), (int)buf.size(), 0);
    if (n > 0) buf.erase(0, n);
}
void gameSendLine(const std::string &line) {
    std::lock_guard<std::mutex> g(cLock);
    for (auto &kv : clients) sendTo(kv.first, line);
}
void gameConfigChanged() { gameSendLine(configLine(false)); }

// ---------------- Cuadros: del juego al codificador ----------------
static VideoEncoder *g_enc;
static std::mutex frameLock;
static std::vector<uint8_t> warpBuf, nv12[2];
static int nvIdx = 0;
static std::mutex holdLock;
static std::vector<uint8_t> holdNv;     // el último cuadro (para repetirlo mientras el juego carga)
static int holdW = 0, holdH = 0;
static uint32_t holdPose = 0;
static uint64_t holdAt = 0;
static int framesTotal = 0;
static std::atomic<int64_t> frameNo{0};

struct Tag { uint32_t pose; uint64_t start; };
static std::mutex tagLock;
static std::map<uint64_t, Tag> tags;

static void onEncoded(const uint8_t *d, size_t n, bool key, uint64_t tag) {
    Tag t{0, nowNs()};
    {
        std::lock_guard<std::mutex> g(tagLock);
        auto it = tags.find(tag);
        if (it != tags.end()) { t = it->second; tags.erase(it); }
        while (tags.size() > 16) tags.erase(tags.begin());
    }
    double elapsed = (double)(nowNs() - t.start) / 100000.0;   // décimas de ms
    perfAddEnc(elapsed / 10.0);
    std::vector<uint8_t> p(8 + n);
    p[0] = 1;
    p[1] = (uint8_t)(t.pose >> 24); p[2] = (uint8_t)(t.pose >> 16); p[3] = (uint8_t)(t.pose >> 8); p[4] = (uint8_t)t.pose;
    p[5] = key ? 1 : 0;
    uint16_t ms = (uint16_t)std::min(65535.0, elapsed);
    p[6] = (uint8_t)(ms >> 8); p[7] = (uint8_t)ms;
    memcpy(p.data() + 8, d, n);
    questSend(std::move(p));
}

static void submit(const std::vector<uint8_t> &nv, int w, int h, uint32_t pose, uint64_t start, bool allowKey) {
    Stream st = currentStream();
    uint64_t fn = (uint64_t)++frameNo;
    { std::lock_guard<std::mutex> g(tagLock); tags[fn] = Tag{pose, start}; }
    bool key = allowKey && questTakeKey();
    if (!g_enc->encode(nv.data(), w, h, st.fps, st.bitrate, st.hevc, key, fn)) {
        if (key) questRequestKey();
        perfAddDrop();
    }
}

/// Un cuadro del juego (dos ojos lado a lado, ew×eh cada uno)
static void handlePixels(const uint8_t *src, uint32_t pose, bool rgba, int ew, int eh) {
    if (ew <= 0 || eh <= 0) return;
    framesTotal++;
    Stream st = currentStream();
    if (framesTotal == 1) logmsg("[game] First frame received: %dx%d per eye, %s · sending %dx%d", ew, eh, rgba ? "RGBA" : "BGRA", st.W, st.H);
    lastGameFrame = nowNs();
    // Modo Centinela: el juego se ve en la ventana de la PC; al visor apoyado va solo 1 cuadro de cada 20
    if (sentinelOn() && !sentinelFrame(src, rgba, ew, eh)) return;
    if (g_enc->busy() || !questConnected()) { perfAddDrop(); return; }   // (el codificador está atrasado: descartar)
    uint64_t start = nowNs();
    std::vector<uint8_t> &nv = nv12[nvIdx ^= 1];
    nv.resize((size_t)st.W * st.H * 3 / 2);
    if (!st.fov.on() && ew == st.eye && eh == st.eye) bgraToNv12(src, ew * 2 * 4, st.W, st.H, nv.data(), rgba);
    else {
        warpEyes(src, ew, eh, rgba, st, warpBuf);
        bgraToNv12(warpBuf.data(), st.W * 4, st.W, st.H, nv.data(), false);
    }
    perfAddMac((double)(nowNs() - start) / 1e6);
    { std::lock_guard<std::mutex> g(holdLock); holdNv = nv; holdW = st.W; holdH = st.H; holdPose = pose; holdAt = nowNs(); }
    submit(nv, st.W, st.H, pose, start, true);
}

void gameQualityChanged() {
    std::lock_guard<std::mutex> g(frameLock);
    { std::lock_guard<std::mutex> h(holdLock); holdNv.clear(); }
    if (g_enc) g_enc->reset();
}

// ---------------- Lectura de cada conexión del juego ----------------
struct ShmHdr { volatile uint32_t magic, nonce, ack, nslots, slotSize; uint32_t reserved[3]; volatile LONG state[3]; };

// (el socket no espera al mandar —así una copia del juego trabada no frena a nadie—; para leer se espera con select)
static bool readExact(SOCKET s, uint8_t *p, size_t n) {
    while (n > 0) {
        int r = recv(s, (char *)p, (int)std::min<size_t>(n, 1 << 24), 0);
        if (r == 0) return false;
        if (r < 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(s, &rs);
            timeval tv{1, 0};
            select(0, &rs, nullptr, nullptr, &tv);
            continue;
        }
        p += r; n -= r;
    }
    return true;
}

static void readLoop(SOCKET s) {
    std::vector<uint8_t> buf(1 << 20);
    uint8_t *shm = nullptr;
    HANDLE map = nullptr;
    size_t shmSlot = 0;
    auto unmap = [&] { if (shm) UnmapViewOfFile(shm); if (map) CloseHandle(map); shm = nullptr; map = nullptr; };
    while (true) {
        uint8_t h[4];
        if (!readExact(s, h, 4)) break;
        uint32_t n = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
        if (n == 0 || n > (128u << 20)) break;
        if (n > buf.size()) buf.resize(n);
        if (!readExact(s, buf.data(), n)) break;
        const uint8_t *b = buf.data();
        switch (b[0]) {
        case 3: logmsg("[game] %.*s", (int)n - 1, (const char *)b + 1); break;
        case 7: perfSetRuntime(std::string((const char *)b + 1, n - 1)); break;
        case 14: questSendText(std::string((const char *)b + 1, n - 1)); break;   // vibración de un control
        case 9: {
            profilesBegin(std::string((const char *)b + 1, n - 1));
            std::lock_guard<std::mutex> g(cLock);
            sendTo(s, configLine(true));
            break;
        }
        case 4: {
            std::lock_guard<std::mutex> g(cLock);
            clients.erase(s);   // esta conexión es solo de imágenes: no le mandamos poses
            logmsg("[game] Image connection identified");
            break;
        }
        case 5: {
            // Oferta de memoria compartida: nonce, tamaño de cada lugar y dónde está (en Windows: un nombre de memoria)
            if (n <= 9) break;
            uint32_t nonce = (uint32_t)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4];
            size_t slot = (size_t)b[5] << 24 | b[6] << 16 | b[7] << 8 | b[8];
            std::string where((const char *)b + 9, n - 9);
            unmap();
            size_t total = 64 + slot * 3;
            if (where.rfind("mapa:", 0) == 0) {
                map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide(where.substr(5)).c_str());
            } else {
                HANDLE f = CreateFileW(wide(where).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) { map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, 0, nullptr); CloseHandle(f); }
            }
            if (map) shm = (uint8_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, total);
            ShmHdr *hd = (ShmHdr *)shm;
            if (shm && hd->magic == 0x31535256u && hd->nonce == nonce) {
                shmSlot = slot;
                MemoryBarrier();
                hd->ack = nonce;   // confirmación: el runtime pasa a usarla
                logmsg("[game] Shared memory active (frames no longer go through the network)");
            } else {
                unmap();
                logmsg("[game] Shared memory: couldn't open %s (error %lu); staying on the network", where.c_str(), GetLastError());
            }
            break;
        }
        case 6: case 12: {
            if (n < 11 || !shm) break;
            int slot = b[1];
            if (slot >= 3) break;
            ShmHdr *hd = (ShmHdr *)shm;
            MemoryBarrier();
            int ew = b[7] << 8 | b[8], eh = b[9] << 8 | b[10];
            uint32_t pose = (uint32_t)b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5];
            if ((size_t)ew * 2 * eh * 4 <= shmSlot) {
                std::lock_guard<std::mutex> g(frameLock);
                handlePixels(shm + 64 + slot * shmSlot, pose, b[6] == 1, ew, eh);
            }
            MemoryBarrier();
            hd->state[slot] = 0;   // lugar libre para el próximo cuadro
            break;
        }
        case 10:
            // Copia directa en la GPU: eso es de la Mac. Le contestamos que no, así sigue con la memoria compartida
            if (shm) { ShmHdr *hd = (ShmHdr *)shm; MemoryBarrier(); hd->reserved[0] = 0x4c494146u; }
            break;
        case 11:
            if (n >= 2 && shm && b[1] < 3) { ShmHdr *hd = (ShmHdr *)shm; hd->state[b[1]] = 0; }
            break;
        case 2:
            if (n > 10) {
                uint32_t pose = (uint32_t)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4];
                int ew = b[6] << 8 | b[7], eh = b[8] << 8 | b[9];
                if (ew > 0 && eh > 0 && n >= 10 + (size_t)ew * 2 * eh * 4) {
                    std::lock_guard<std::mutex> g(frameLock);
                    handlePixels(b + 10, pose, b[5] == 1, ew, eh);
                }
            }
            break;
        default: break;
        }
    }
    unmap();
}

void gameStart() {
    g_enc = new VideoEncoder("game", onEncoded);
    // Juego cargando (conectado pero sin cuadros desde hace más de 0,3 s, hasta 2 min): repetir el último cuadro (más
    // oscuro después de 3 s, así una traba corta del juego no se nota)
    std::thread([] {
        std::vector<uint8_t> dark;
        uint64_t darkFor = 0;
        while (true) {
            Sleep(100);
            if (openConns == 0 || !questConnected()) continue;
            std::lock_guard<std::mutex> g(holdLock);
            if (holdNv.empty()) continue;
            uint64_t now = nowNs(), age = now - holdAt;
            if (age < 300000000ull || age > 120000000000ull || g_enc->busy()) continue;
            if (age < 3000000000ull) {
                lastGameFrame = now;
                submit(holdNv, holdW, holdH, holdPose, now, false);
                continue;
            }
            if (darkFor != holdAt) {
                dark = holdNv;
                size_t ys = (size_t)holdW * holdH;
                for (size_t i = 0; i < ys; i++) dark[i] = (uint8_t)(16 + (dark[i] - 16) * 45 / 100);
                for (size_t i = ys; i < dark.size(); i++) dark[i] = (uint8_t)(128 + ((int)dark[i] - 128) * 45 / 100);
                darkFor = holdAt;
            }
            lastGameFrame = now;
            submit(dark, holdW, holdH, holdPose, now, false);
        }
    }).detach();
    std::thread([] {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(9002);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOL excl = TRUE;
        setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
        if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
            logmsg("ERROR: couldn't open port 9002 for the game (is another VRmin Host open?)");
            return;
        }
        while (true) {
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            BOOL one = TRUE;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            int big = 16 << 20;
            setsockopt(c, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
            u_long nb = 1;
            ioctlsocket(c, FIONBIO, &nb);
            int total;
            {
                std::lock_guard<std::mutex> g(cLock);
                clients[c] = "";
                total = (int)clients.size();
            }
            openConns++;
            logmsg("[game] A game connected to VRmin (open connections: %d)", total);
            {
                std::lock_guard<std::mutex> g(cLock);
                sendTo(c, configLine(false));
            }
            questRequestKey();
            uiRefresh();
            std::thread([c] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                readLoop(c);
                int left;
                {
                    std::lock_guard<std::mutex> g(cLock);
                    clients.erase(c);
                    left = (int)clients.size();
                }
                closesocket(c);
                if (--openConns == 0) { profilesEnd(); std::lock_guard<std::mutex> g(holdLock); holdNv.clear(); }
                logmsg("[game] Connection with a game closed (left: %d)", left);
                uiRefresh();
            }).detach();
        }
    }).detach();
}
