// Conexión con la app del Quest: aviso en la red (UDP 9101), emparejado y conexión TCP (9100)
// PC -> Quest: [largo u32][1 video | 2 sonido | 3 texto JSON | 7 escritorio]
// Quest -> PC: [largo u32][JSON] (pose, key, log, menú…)
#include "host.h"
#include <iphlpapi.h>
#include <bcrypt.h>
#include <condition_variable>
#include <deque>
#include <thread>

static const uint16_t NATIVE_PORT = 9100, BEACON_PORT = 9101;

std::atomic<int> g_pairCode{-1};
std::atomic<bool> g_questWifi{false};
std::string g_questName;

static std::mutex qLock;
static SOCKET qSock = INVALID_SOCKET;
static size_t qPending = 0;
static bool qNeedKey = true;
static std::deque<std::vector<uint8_t>> qQueue;
static std::condition_variable qCv;
static std::atomic<bool> g_keyWanted{true};
static int statSent = 0, statDropped = 0, statWaitKey = 0;

bool questConnected() { std::lock_guard<std::mutex> g(qLock); return qSock != INVALID_SOCKET; }
void questRequestKey() { g_keyWanted = true; }
bool questTakeKey() { return g_keyWanted.exchange(false); }

/// Nunca bloquea: si el Wi-Fi no da abasto, se descarta video y se pide un cuadro completo
void questSend(std::vector<uint8_t> &&p) {
    if (p.empty()) return;
    uint8_t t = p[0];
    std::lock_guard<std::mutex> g(qLock);
    if (qSock == INVALID_SOCKET) return;
    if (t == 1) {
        bool key = p.size() > 5 && p[5] == 1;
        if (qNeedKey && !key) { statWaitKey++; return; }
        if (qPending > 4000000) { statDropped++; qNeedKey = true; g_keyWanted = true; return; }
        qNeedKey = false;
        statSent++;
    } else if (t == 2 && qPending > 1000000) {
        return;
    } else if ((t == 7 || t == 8) && qPending > 3000000) {
        deskRequestKey();
        return;
    }
    qPending += p.size() + 4;
    qQueue.push_back(std::move(p));
    qCv.notify_one();
}
void questSendText(const std::string &json) {
    std::vector<uint8_t> p(1 + json.size());
    p[0] = 3;
    memcpy(p.data() + 1, json.data(), json.size());
    questSend(std::move(p));
}

static bool sendAll(SOCKET s, const uint8_t *p, size_t n) {
    while (n > 0) {
        int r = send(s, (const char *)p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}
static bool readExact(SOCKET s, uint8_t *p, size_t n) {
    while (n > 0) {
        int r = recv(s, (char *)p, (int)n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static void senderThread() {
    while (true) {
        std::vector<uint8_t> p;
        SOCKET s;
        {
            std::unique_lock<std::mutex> g(qLock);
            qCv.wait(g, [] { return !qQueue.empty(); });
            p = std::move(qQueue.front());
            qQueue.pop_front();
            s = qSock;
        }
        bool ok = s != INVALID_SOCKET;
        if (ok) {
            uint8_t len[4] = {(uint8_t)(p.size() >> 24), (uint8_t)(p.size() >> 16), (uint8_t)(p.size() >> 8), (uint8_t)p.size()};
            ok = sendAll(s, len, 4) && sendAll(s, p.data(), p.size());
        }
        std::lock_guard<std::mutex> g(qLock);
        qPending -= std::min(qPending, p.size() + 4);
        if (!ok && s != INVALID_SOCKET && s == qSock) shutdown(s, SD_BOTH);
    }
}

// ---------------- Emparejar ----------------
// La primera vez, la PC muestra un código de 6 números que hay que escribir en el Quest; después el Quest guarda una clave.
static std::mutex pairLock;
static std::string pairCodeStr, pairFor;
static uint64_t pairUntil = 0;
static int pairTries = 0;
static void randomBytes(uint8_t *b, size_t n) {
    if (BCryptGenRandom(nullptr, b, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(rand() & 255);
}
static bool ctEq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
enum class Auth { Ok, NeedCode, WrongCode };
static Auth pairCheck(const Json &m, const std::string &ip, std::string &newToken) {
    std::lock_guard<std::mutex> g(pairLock);
    std::string id = m["id"].str();
    if (id.size() < 8 || id.size() > 80) return Auth::NeedCode;
    std::string name = m["device"].str("Quest");
    std::wstring file = dataDir() + L"\\paired.json";
    Json all = Json::parse(readFile(file));
    if (all.t != Json::Obj) all = Json::object();
    std::string tok = m["token"].str();
    if (!tok.empty() && all[id].has("token") && ctEq(tok, all[id]["token"].str())) return Auth::Ok;
    uint64_t now = nowNs();
    if (m.has("code") && !pairCodeStr.empty() && now < pairUntil && pairFor == id) {
        if (ctEq(m["code"].str(), pairCodeStr)) {
            uint8_t r[32];
            randomBytes(r, sizeof r);
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", r[i]);
            newToken = hex;
            SYSTEMTIME t;
            GetSystemTime(&t);
            char since[40];
            snprintf(since, sizeof since, "%04d-%02d-%02dT%02d:%02d:%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            all.set(id, Json::object().set("token", newToken).set("name", name).set("since", since));
            writeFile(file, all.dump());
            pairCodeStr.clear();
            g_pairCode = -1;
            logmsg("[pairing] DONE %s (%s)", name.c_str(), ip.c_str());
            uiRefresh();
            return Auth::Ok;
        }
        if (++pairTries >= 5) { pairCodeStr.clear(); logmsg("[pairing] EXPIRED"); }
        else return Auth::WrongCode;
    }
    if (pairCodeStr.empty() || now >= pairUntil || pairFor != id) {
        uint8_t r[4];
        randomBytes(r, 4);
        uint32_t n = ((uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3]) % 1000000;
        char c[8];
        snprintf(c, sizeof c, "%06u", n);
        pairCodeStr = c;
        pairFor = id;
        pairUntil = now + 300ull * 1000000000ull;
        pairTries = 0;
        g_pairCode = (int)n;
        g_questName = name;
        logmsg("[pairing] CODE %s %s", c, name.c_str());
        uiRefresh();
    }
    return m.has("code") ? Auth::WrongCode : Auth::NeedCode;
}

// ---------------- Direcciones de la PC ----------------
struct Addrs { std::vector<uint32_t> bcast; std::vector<std::string> ll; };
static Addrs localAddrs() {
    Addrs a;
    ULONG sz = 32768;
    std::vector<uint8_t> buf(sz);
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr,
                             (IP_ADAPTER_ADDRESSES *)buf.data(), &sz) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz);
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr,
                                 (IP_ADAPTER_ADDRESSES *)buf.data(), &sz) != NO_ERROR) return a;
    }
    for (auto *ad = (IP_ADAPTER_ADDRESSES *)buf.data(); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK || ad->IfType == IF_TYPE_TUNNEL) continue;
        for (auto *u = ad->FirstUnicastAddress; u; u = u->Next) {
            sockaddr *sa = u->Address.lpSockaddr;
            if (sa->sa_family == AF_INET) {
                uint32_t ip = ntohl(((sockaddr_in *)sa)->sin_addr.s_addr);
                int pl = u->OnLinkPrefixLength;
                if (pl <= 0 || pl >= 31) continue;
                uint32_t mask = pl == 0 ? 0 : 0xFFFFFFFFu << (32 - pl);
                a.bcast.push_back(htonl(ip | ~mask));
            } else if (sa->sa_family == AF_INET6 && ad->IfType == IF_TYPE_ETHERNET_CSMACD) {
                // (el cable USB-C del Quest aparece como una red cableada: el Quest se conecta por su dirección fe80::)
                char s[INET6_ADDRSTRLEN] = "";
                inet_ntop(AF_INET6, &((sockaddr_in6 *)sa)->sin6_addr, s, sizeof s);
                std::string v = s;
                size_t pc = v.find('%');
                if (pc != std::string::npos) v = v.substr(0, pc);
                if (v.rfind("fe80", 0) == 0) a.ll.push_back(v);
            }
        }
    }
    return a;
}

static std::string pcName() {
    wchar_t b[256];
    DWORD n = 256;
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, b, &n)) return "PC";
    std::string s = utf8(std::wstring(b, n)), out;
    for (char c : s) if (c != '"' && c != '\\') out += c;
    return out.empty() ? "PC" : out;
}

std::string questPcName() { return pcName(); }   // (el nombre con que el Quest encuentra a esta PC)

static void beaconThread() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    std::string name = pcName();
    bool announced = false, warned = false;
    while (true) {
        if (!questConnected()) {
            Addrs a = localAddrs();
            Json ll = Json::array();
            for (auto &x : a.ll) ll.push(x);
            std::string msg = "{\"qvb\":1,\"port\":9100,\"name\": " + Json::esc(name) + ",\"ll\":" + ll.dump() + "}";
            std::vector<uint32_t> targets = {0xFFFFFFFFu};
            for (auto b : a.bcast) targets.push_back(b);
            if (!announced) { announced = true; logmsg("[quest] Announcing on the local network for the Quest app (%d networks)", (int)a.bcast.size()); }
            for (uint32_t t : targets) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(BEACON_PORT);
                to.sin_addr.s_addr = t;
                if (sendto(s, msg.data(), (int)msg.size(), 0, (sockaddr *)&to, sizeof to) < 0 && !warned) {
                    warned = true;
                    logmsg("[quest] Couldn't announce on the local network (error %d): check Windows Firewall", WSAGetLastError());
                }
            }
        } else announced = false;
        Sleep(1000);
    }
}

void handleQuestMessage(const std::string &line);
void questOnConnect();
void questOnDisconnect();

static void readerThread(SOCKET c) {
    std::vector<uint8_t> body;
    while (true) {
        uint8_t h[4];
        if (!readExact(c, h, 4)) break;
        uint32_t n = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
        if (n == 0 || n > (1u << 20)) break;
        body.resize(n);
        if (!readExact(c, body.data(), n)) break;
        if (body[0] == 0x0B) { micPush(body.data() + 1, n - 1); continue; }   // (lo que graba el micrófono del visor)
        handleQuestMessage(std::string((const char *)body.data(), n));
    }
    bool gone;
    {
        std::lock_guard<std::mutex> g(qLock);
        gone = qSock == c;
        if (gone) qSock = INVALID_SOCKET;
    }
    Sleep(50);
    closesocket(c);
    if (gone) { questOnDisconnect(); logmsg("[quest] Quest app disconnected"); uiRefresh(); }
}

static void listenThread() {
    // IPv6 y IPv4 en el mismo puerto: por Wi-Fi llega por IPv4; por el cable USB-C, por IPv6 (fe80::)
    SOCKET ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool v6 = ls != INVALID_SOCKET;
    int r;
    if (v6) {
        DWORD zero = 0;
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(NATIVE_PORT);
        a.sin6_addr = in6addr_any;
        r = bind(ls, (sockaddr *)&a, sizeof a);
    } else {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // (Windows sin IPv6: solo por Wi-Fi)
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(NATIVE_PORT);
        r = bind(ls, (sockaddr *)&a, sizeof a);
    }
    if (r != 0 || listen(ls, 4) != 0) {
        logmsg("[quest] Couldn't open port %d for the Quest app (error %d)", NATIVE_PORT, WSAGetLastError());
        return;
    }
    while (true) {
        sockaddr_storage peer{};
        int plen = sizeof peer;
        SOCKET c = accept(ls, (sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) continue;
        BOOL one = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        int big = 4 << 20;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof big);
        char ipb[INET6_ADDRSTRLEN] = "";
        if (peer.ss_family == AF_INET6) inet_ntop(AF_INET6, &((sockaddr_in6 *)&peer)->sin6_addr, ipb, sizeof ipb);
        else inet_ntop(AF_INET, &((sockaddr_in *)&peer)->sin_addr, ipb, sizeof ipb);
        std::string ip = ipb;
        if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);
        bool cable = ip.rfind("fe80", 0) == 0;
        if (cable) logmsg("[quest] Connection over the USB-C cable");
        // Emparejado: el primer mensaje del Quest trae su clave (o el código que le mostramos)
        DWORD tv = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        bool ok = false;
        std::string newToken;
        uint8_t h[4];
        if (readExact(c, h, 4)) {
            uint32_t n = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
            std::vector<uint8_t> body(n < 4096 ? n : 0);
            if (n > 0 && n < 4096 && readExact(c, body.data(), n)) {
                Json o = Json::parse(std::string((const char *)body.data(), n));
                if (o["type"].str() == "auth") {
                    Auth r = pairCheck(o, ip, newToken);
                    if (r == Auth::Ok) ok = true;
                    else {
                        std::string m = std::string("\x03") + "{\"type\":\"auth\",\"ok\":false,\"need\":\"code\",\"wrong\":" + (r == Auth::WrongCode ? "true" : "false") + "}";
                        uint8_t len[4] = {(uint8_t)(m.size() >> 24), (uint8_t)(m.size() >> 16), (uint8_t)(m.size() >> 8), (uint8_t)m.size()};
                        sendAll(c, len, 4);
                        sendAll(c, (const uint8_t *)m.data(), m.size());
                    }
                }
            }
        }
        if (!ok) { logmsg("[quest] Unpaired connection from %s: asked for the code", ip.c_str()); Sleep(200); closesocket(c); continue; }
        tv = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        {
            std::lock_guard<std::mutex> g(qLock);
            if (qSock != INVALID_SOCKET) shutdown(qSock, SD_BOTH);   // una sola app a la vez: la nueva reemplaza a la anterior
            qSock = c; qNeedKey = true; qPending = 0; qQueue.clear();
        }
        g_questWifi = !cable;
        logmsg("[quest] Quest app connected from %s", ip.c_str());
        questSendText(newToken.empty() ? "{\"type\":\"auth\",\"ok\":true}" : "{\"type\":\"auth\",\"ok\":true,\"token\":\"" + newToken + "\"}");
        Stream st = currentStream();
        Json cfg = Json::object();
        cfg.set("type", "config").set("eye", st.eye).set("fps", st.fps).set("vw", st.W).set("vh", st.H)
           .set("fov", st.fov.on() ? st.fov.text() : std::string("0")).set("codec", st.hevc ? "hevc" : "h264");
        questSendText(cfg.dump());
        questRequestKey();
        questOnConnect();
        uiRefresh();
        std::thread(readerThread, c).detach();
    }
}

static void statsThread() {
    while (true) {
        Sleep(5000);
        int a, d, w;
        size_t pe;
        bool c;
        {
            std::lock_guard<std::mutex> g(qLock);
            c = qSock != INVALID_SOCKET; a = statSent; d = statDropped; w = statWaitKey; pe = qPending;
            statSent = statDropped = statWaitKey = 0;
        }
        if (c && (a || d)) logmsg("[quest] last 5 s: %d frames sent, %d dropped by the network, %d waiting for a full frame, %d KB queued", a, d, w, (int)(pe / 1024));
        if (c && a > 0) logmsg("[performance] %s", perfSnapshot().dump().c_str());
    }
}

void questStart() {
    std::thread(senderThread).detach();
    std::thread(beaconThread).detach();
    std::thread(listenThread).detach();
    std::thread(statsThread).detach();
}
