// Actualizaciones: la versión de Linux publicada más nueva en GitHub (con curl, que viene en casi todas las
// distribuciones). VRmin Host se reemplaza solo: baja el .tar.gz, copia lo nuevo encima, se cierra y se abre la nueva.
#include "host.h"
#include <csignal>
#include <cstdio>
#include <thread>
#include <unistd.h>

static const char *RELEASES = "https://github.com/thevermin/VRmin/releases/download/";

static std::string run(const std::string &cmd) {
    std::string out;
    if (FILE *p = popen(cmd.c_str(), "r")) {
        char b[16384];
        size_t n;
        while ((n = fread(b, 1, sizeof b, p)) > 0) out.append(b, n);
        pclose(p);
    }
    return out;
}
/// Entre comillas simples para el shell
static std::string q(const std::string &s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
}
/// "0.10" es más nueva que "0.9" (se compara por partes, como números)
static bool newer(const std::string &a, const std::string &b) {
    const char *x = a.c_str(), *y = b.c_str();
    while (*x || *y) {
        long p = strtol(x, (char **)&x, 10), r = strtol(y, (char **)&y, 10);
        if (p != r) return p > r;
        if (*x == '.') x++;
        if (*y == '.') y++;
        if (!isdigit((unsigned char)*x) && !isdigit((unsigned char)*y)) break;
    }
    return false;
}

static std::mutex uLock;
static std::string g_ver, g_url, g_msg, g_busy;

void updatesCheck(bool manual) {
    if (manual) { std::lock_guard<std::mutex> g(uLock); g_msg = tr("Buscando…", "Checking…"); }
    Json list = Json::parse(run("curl -fsSL --max-time 20 -H 'Accept: application/vnd.github+json' "
                                "'https://api.github.com/repos/thevermin/VRmin/releases?per_page=30' 2>/dev/null"));
    std::lock_guard<std::mutex> g(uLock);
    if (list.t != Json::Arr) {
        if (manual) g_msg = tr("No se pudo buscar. ¿Hay internet? (hace falta curl)", "Couldn't check. Is the PC online? (curl is needed)");
        return;
    }
    std::string ver, url;
    for (auto &j : list.a) {
        if (j["draft"].boolean(false) || j["prerelease"].boolean(false)) continue;
        for (auto &a : j["assets"].a) {
            std::string n = a["name"].str(), u = a["browser_download_url"].str();   // «VRmin.Linux.0.1.tar.gz»
            if (u.rfind(RELEASES, 0) != 0 || n.rfind("VRmin.Linux.", 0) != 0 || n.size() < 19 || n.compare(n.size() - 7, 7, ".tar.gz")) continue;
            std::string v = n.substr(12, n.size() - 19);
            if (!v.empty() && isdigit((unsigned char)v[0]) && (ver.empty() || newer(v, ver))) ver = v, url = u;
        }
    }
    if (ver.empty() || !newer(ver, VRMIN_LINUX_VERSION)) ver.clear(), url.clear();
    else if (ver != g_ver) logmsg("[update] VRmin for Linux %s is available", ver.c_str());
    g_ver = ver; g_url = url;
    if (manual) g_msg = ver.empty() ? tr("Estás al día", "You're up to date") : tr("Hay una versión nueva: botón arriba", "There's a new version: button at the top");
}

void updatesStart() {
    std::thread([] { while (true) { updatesCheck(false); sleep(6 * 3600); } }).detach();
}

Json updatesState() {
    std::lock_guard<std::mutex> g(uLock);
    return Json::object().set("host", g_ver).set("hostBack", false).set("msg", g_msg).set("busy", g_busy);
}

/// Baja la nueva, la copia encima de esta (archivo por archivo, con rename: el programa abierto no molesta),
/// y cuando esta se cierra se abre la nueva
bool updatesInstallHost() {
    std::string ver, url;
    {
        std::lock_guard<std::mutex> g(uLock);
        if (!g_busy.empty() || g_url.empty()) return false;
        ver = g_ver; url = g_url;
        g_busy = tr("Bajando la versión nueva…", "Downloading the new version…");
    }
    auto done = [](const char *msg) {
        if (msg) logmsg("[update] %s: github.com/thevermin/VRmin/releases/latest", msg);
        std::lock_guard<std::mutex> g(uLock);
        g_busy.clear();
        return !msg;
    };
    std::string dir = exeDir();
    if (access(dir.c_str(), W_OK) != 0) return done("I can't replace the app where it is: download the new one from the releases page");
    std::string t = dataDir() + "/update";
    logmsg("[update] Downloading VRmin for Linux %s…", ver.c_str());
    std::string out = run("rm -rf " + q(t) + " && mkdir -p " + q(t) + " && curl -fsSL --max-time 600 -o " + q(t + "/new.tar.gz") + " " + q(url) +
                          " && tar -xzf " + q(t + "/new.tar.gz") + " -C " + q(t) + " && echo ok");
    std::string src = t + "/VRmin Linux";
    if (out.find("ok") == std::string::npos || access((src + "/vrmin-host").c_str(), X_OK) != 0) return done("Couldn't download it. Is the PC online?");
    // (cada archivo: se copia al lado y se renombra encima)
    out = run("cd " + q(src) + " && find . -type f | while IFS= read -r f; do mkdir -p " + q(dir) + "/\"$(dirname \"$f\")\" && cp -p \"$f\" " + q(dir) +
              "/\"$f.new\" && mv -f " + q(dir) + "/\"$f.new\" " + q(dir) + "/\"$f\" || exit 1; done && echo ok");
    if (out.find("ok") == std::string::npos) return done("Couldn't replace the app: download the new one from the releases page");
    logmsg("[update] Done: VRmin Host closes and the new version opens");
    // Cuando esta se cierra, se abre la nueva (aparte: no se cierra con esta)
    std::string after = "while kill -0 " + std::to_string(getpid()) + " 2>/dev/null; do sleep 0.3; done; rm -rf " + q(t) + "; exec " + q(dir + "/vrmin-host");
    run("setsid sh -c " + q(after) + " >/dev/null 2>&1 < /dev/null &");
    done(nullptr);
    kill(getpid(), SIGTERM);   // (se cierra como con Ctrl+C: deja los ajustes de VR como estaban)
    return true;
}
