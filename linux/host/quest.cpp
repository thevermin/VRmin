// Conexión con la app del Quest: aviso en la red (UDP 9101), emparejado y conexión TCP (9100)
// PC -> Quest: [largo u32][1 video | 2 sonido | 3 texto JSON | 7 escritorio]
// Quest -> PC: [largo u32][JSON] (pose, key, log, menú…)
#include "host.h"
#include <cerrno>
#include <ctime>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <condition_variable>
#include <deque>
#include <thread>

static const uint16_t NATIVE_PORT = 9100, BEACON_PORT = 9101;

std::atomic<int> g_pairCode{-1};
std::atomic<bool> g_questWifi{false};
std::string g_questName;

typedef int SOCKET;
static const SOCKET INVALID_SOCKET = -1;

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
        ssize_t r = send(s, p, std::min<size_t>(n, 1 << 20), MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}
static bool readExact(SOCKET s, uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t r = recv(s, p, n, 0);
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
        if (!ok && s != INVALID_SOCKET && s == qSock) shutdown(s, SHUT_RDWR);
    }
}

// ---------------- Emparejar ----------------
// La primera vez, la PC muestra un código de 6 números que hay que escribir en el Quest; después el Quest guarda una clave.
static std::mutex pairLock;
static std::string pairCodeStr, pairFor;
static uint64_t pairUntil = 0;
static int pairTries = 0;
static void randomBytes(uint8_t *b, size_t n) {
    if (getrandom(b, n, 0) != (ssize_t)n)
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
    std::string file = dataDir() + "/paired.json";
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
            time_t now_t = time(nullptr);
            tm t;
            gmtime_r(&now_t, &t);
            char since[40];
            strftime(since, sizeof since, "%Y-%m-%dT%H:%M:%SZ", &t);
            all.set(id, Json::object().set("token", newToken).set("name", name).set("since", since));
            writeFile(file, all.dump());
            chmod(file.c_str(), 0600);   // (las claves de los visores: solo para este usuario)
            pairCodeStr.clear();
            g_pairCode = -1;
            logmsg("[pairing] DONE %s (%s)", name.c_str(), ip.c_str());
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
        logmsg("%s %03u %03u", tr("Escribí este código en el Quest para emparejarlo:", "Type this code on the Quest to pair it:"), n / 1000, n % 1000);
    }
    return m.has("code") ? Auth::WrongCode : Auth::NeedCode;
}

// ---------------- Direcciones de la PC ----------------
struct Addrs { std::vector<uint32_t> bcast; std::vector<std::string> ll; };
static Addrs localAddrs() {
    Addrs a;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return a;
    for (ifaddrs *ad = list; ad; ad = ad->ifa_next) {
        if (!ad->ifa_addr || !(ad->ifa_flags & IFF_UP) || (ad->ifa_flags & IFF_LOOPBACK)) continue;
        if (ad->ifa_addr->sa_family == AF_INET && ad->ifa_netmask) {
            uint32_t ip = ntohl(((sockaddr_in *)ad->ifa_addr)->sin_addr.s_addr);
            uint32_t mask = ntohl(((sockaddr_in *)ad->ifa_netmask)->sin_addr.s_addr);
            if (mask == 0 || mask >= 0xFFFFFFFEu) continue;
            a.bcast.push_back(htonl(ip | ~mask));
        } else if (ad->ifa_addr->sa_family == AF_INET6) {
            // (el cable USB-C del Quest aparece como una red cableada: el Quest se conecta por su dirección fe80::)
            struct stat st;
            if (stat(("/sys/class/net/" + std::string(ad->ifa_name) + "/wireless").c_str(), &st) == 0) continue;
            char b[INET6_ADDRSTRLEN] = "";
            inet_ntop(AF_INET6, &((sockaddr_in6 *)ad->ifa_addr)->sin6_addr, b, sizeof b);
            std::string v = b;
            if (v.rfind("fe80", 0) == 0) a.ll.push_back(v);
        }
    }
    freeifaddrs(list);
    return a;
}

static std::string pcName() {
    char b[256] = "";
    if (gethostname(b, sizeof b - 1) != 0) return "PC";
    std::string out;
    for (char *c = b; *c; c++) if (*c != '"' && *c != '\\') out += *c;
    return out.empty() ? "PC" : out;
}

std::string questPcName() { return pcName(); }   // (el nombre con que el Quest encuentra a esta PC)

static void beaconThread() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
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
                if (sendto(s, msg.data(), msg.size(), 0, (sockaddr *)&to, sizeof to) < 0 && !warned) {
                    warned = true;
                    logmsg("[quest] Couldn't announce on the local network (%s): check the firewall (UDP 9101)", strerror(errno));
                }
            }
        } else announced = false;
        sleepMs(1000);
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
    sleepMs(50);
    close(c);
    if (gone) { questOnDisconnect(); logmsg("[quest] Quest app disconnected"); }
}

static void listenThread() {
    // IPv6 y IPv4 en el mismo puerto: por Wi-Fi llega por IPv4; por el cable USB-C, por IPv6 (fe80::)
    SOCKET ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool v6 = ls != INVALID_SOCKET;
    int r;
    if (v6) {
        int zero = 0, one = 1;
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(NATIVE_PORT);
        a.sin6_addr = in6addr_any;
        r = bind(ls, (sockaddr *)&a, sizeof a);
    } else {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // (sin IPv6: solo por Wi-Fi)
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(NATIVE_PORT);
        r = bind(ls, (sockaddr *)&a, sizeof a);
    }
    if (r != 0 || listen(ls, 4) != 0) {
        logmsg("[quest] Couldn't open port %d for the Quest app (%s)", NATIVE_PORT, strerror(errno));
        return;
    }
    while (true) {
        sockaddr_storage peer{};
        socklen_t plen = sizeof peer;
        SOCKET c = accept(ls, (sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) continue;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int big = 4 << 20;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        char ipb[INET6_ADDRSTRLEN] = "";
        if (peer.ss_family == AF_INET6) inet_ntop(AF_INET6, &((sockaddr_in6 *)&peer)->sin6_addr, ipb, sizeof ipb);
        else inet_ntop(AF_INET, &((sockaddr_in *)&peer)->sin_addr, ipb, sizeof ipb);
        std::string ip = ipb;
        if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);
        bool cable = ip.rfind("fe80", 0) == 0;
        if (cable) logmsg("[quest] Connection over the USB-C cable");
        // Emparejado: el primer mensaje del Quest trae su clave (o el código que le mostramos)
        timeval tv{5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
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
        if (!ok) { logmsg("[quest] Unpaired connection from %s: asked for the code", ip.c_str()); sleepMs(200); close(c); continue; }
        tv = timeval{0, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        {
            std::lock_guard<std::mutex> g(qLock);
            if (qSock != INVALID_SOCKET) shutdown(qSock, SHUT_RDWR);   // una sola app a la vez: la nueva reemplaza a la anterior
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
        std::thread(readerThread, c).detach();
    }
}

static void statsThread() {
    while (true) {
        sleepMs(5000);
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
