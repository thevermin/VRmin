// Preparar Linux para los juegos VR. Con Proton, los juegos de Windows usan la VR de Linux:
//  - OpenXR: el runtime activo del usuario (~/.config/openxr/1/active_runtime.json), que Proton usa por wineopenxr
//  - OpenVR (SteamVR): el primer runtime de ~/.config/openvr/openvrpaths.vrpath, del que Proton carga
//    bin/linux64/vrclient.so (ahí va OpenComposite para Linux)
// Mientras VRmin Host está abierto apuntan a VRmin; al cerrarlo quedan como estaban (lo de antes se guarda aparte).
// El runtime de VRmin (libvrmin_openxr.so) y OpenComposite vienen al lado del programa (runtime/ y OpenComposite/) y se
// instalan en ~/.local/share/VRmin cuando faltan o cambiaron.
// El Steam de Flatpak no ve esas carpetas: tiene su propia configuración (~/.var/app/com.valvesoftware.Steam/.config)
// y una copia del runtime y de OpenComposite dentro de su carpeta (que se ve con la misma ruta adentro y afuera).
#include "host.h"
#include <cerrno>
#include <climits>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string configDir() {
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    return x && *x ? x : std::string(h ? h : "") + "/.config";
}
static bool exists(const std::string &p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
static void mkdirs(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
}

/// Dónde se prepara la VR: el Steam de siempre y, si está, el de Flatpak
struct Target {
    std::string name;   // (para el registro)
    std::string cfg;    // la carpeta de configuración que ven los juegos
    std::string data;   // dónde van el runtime, OpenComposite y lo que se guarda de antes
    std::string xrActiveFile() const { return cfg + "/openxr/1/active_runtime.json"; }
    std::string vrpathFile() const { return cfg + "/openvr/openvrpaths.vrpath"; }
    std::string ourXrJson() const { return data + "/runtime/vrmin_openxr.json"; }
    std::string ocDir() const { return data + "/OpenComposite"; }
};
static std::vector<Target> targets() {
    std::vector<Target> t = {{"", configDir(), dataDir()}};
    const char *h = getenv("HOME");
    std::string fp = std::string(h ? h : "") + "/.var/app/com.valvesoftware.Steam";
    struct stat st;
    if (h && *h && stat(fp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) t.push_back({" (Flatpak Steam)", fp + "/.config", fp + "/.local/share/VRmin"});
    return t;
}

bool vrRuntimeInstalled() { return exists(targets()[0].ourXrJson()); }
bool vrOcInstalled() { return exists(targets()[0].ocDir() + "/bin/linux64/vrclient.so"); }

static bool xrActive(const Target &t) {
    char b[PATH_MAX];
    ssize_t n = readlink(t.xrActiveFile().c_str(), b, sizeof b - 1);
    return n > 0 && std::string(b, n) == t.ourXrJson();
}
static bool ocActive(const Target &t) {
    Json j = Json::parse(readFile(t.vrpathFile()));
    const Json &r = j["runtime"];
    return r.t == Json::Arr && !r.a.empty() && r.a[0].str() == t.ocDir();
}

/// OpenXR: active_runtime.json pasa a ser un enlace al de VRmin. El que había (archivo o enlace) se guarda tal cual.
static bool setXr(const Target &t, bool ours) {
    std::string f = t.xrActiveFile(), saved = t.data + "/previous_active_runtime.json";
    if (ours) {
        if (xrActive(t)) return true;
        mkdirs(f.substr(0, f.rfind('/')));
        mkdirs(t.data);
        if (exists(f) && !exists(saved) && rename(f.c_str(), saved.c_str()) != 0) return false;
        unlink(f.c_str());
        return symlink(t.ourXrJson().c_str(), f.c_str()) == 0;
    }
    if (!xrActive(t)) return true;
    unlink(f.c_str());
    return !exists(saved) || rename(saved.c_str(), f.c_str()) == 0;
}

/// OpenVR: OpenComposite primero en la lista de runtimes (los demás quedan después, como en Windows)
static bool setVrpath(const Target &t, bool ours) {
    std::string f = t.vrpathFile();
    Json j = Json::parse(readFile(f));
    std::string made = t.data + "/made_openvrpaths";   // (no había: al cerrar se borra)
    if (!ours && exists(made)) { unlink(made.c_str()); return unlink(f.c_str()) == 0 || errno == ENOENT; }
    if (j.t != Json::Obj) {
        if (!ours) return true;
        mkdirs(t.data);
        writeFile(made, "");
        j = Json::object();
        j.set("config", Json::array().push(t.cfg + "/openvr")).set("log", Json::array().push(t.cfg + "/openvr"))
         .set("external_drivers", Json()).set("jsonid", "vrpathreg").set("version", 1);
    }
    Json rt = Json::array();
    if (ours) rt.push(t.ocDir());
    for (auto &x : j["runtime"].a) if (x.str() != t.ocDir()) rt.push(x);
    j.set("runtime", rt);
    mkdirs(f.substr(0, f.rfind('/')));
    return writeFile(f, j.dump());
}

// ---------- Instalar el runtime y OpenComposite (desde la carpeta del programa) ----------
static std::string exeDir() {
    char b[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    std::string p = n > 0 ? std::string(b, n) : "";
    return p.substr(0, p.rfind('/'));
}

/// Copia un archivo si falta o cambió. false: no se pudo (o no hay de dónde)
static bool installFile(const std::string &from, const std::string &to) {
    std::string data = readFile(from);
    if (data.empty()) return false;
    if (readFile(to) == data) return true;
    mkdirs(to.substr(0, to.rfind('/')));
    if (!writeFile(to, data)) { logmsg("[setup] Couldn't install %s (%s)", to.c_str(), strerror(errno)); return false; }
    logmsg("[setup] Installed %s", to.c_str());
    return true;
}

static void install(const Target &t) {
    std::string src = exeDir();
    if (installFile(src + "/runtime/libvrmin_openxr.so", t.data + "/runtime/libvrmin_openxr.so")) {
        // (con la ruta completa: el de Flatpak la ve igual)
        Json m = Json::object().set("file_format_version", "1.0.0")
                     .set("runtime", Json::object().set("name", "VRmin").set("library_path", t.data + "/runtime/libvrmin_openxr.so"));
        if (readFile(t.ourXrJson()) != m.dump() && !writeFile(t.ourXrJson(), m.dump()))
            logmsg("[setup] Couldn't write %s (%s)", t.ourXrJson().c_str(), strerror(errno));
    } else if (!exists(t.ourXrJson())) logmsg("[setup] %s/runtime/libvrmin_openxr.so not found (build it with linux/runtime/build.sh)", src.c_str());
    if (installFile(src + "/OpenComposite/bin/linux64/vrclient.so", t.ocDir() + "/bin/linux64/vrclient.so")) {
        // (Steam no reemplaza un runtime de OpenVR que tiene bin/version.txt)
        installFile(src + "/OpenComposite/bin/version.txt", t.ocDir() + "/bin/version.txt");
        installFile(src + "/OpenComposite/LICENSE-OpenComposite.txt", t.ocDir() + "/LICENSE-OpenComposite.txt");
    } else if (!exists(t.ocDir() + "/bin/linux64/vrclient.so"))
        logmsg("[setup] %s/OpenComposite/bin/linux64/vrclient.so not found (build it with linux/opencomposite/build.sh)", src.c_str());
}

bool vrSetupActivate() {
    bool ok = true;
    for (const Target &t : targets()) {
        install(t);
        const char *w = t.name.c_str();
        if (!exists(t.ourXrJson())) { logmsg("[setup] VRmin's OpenXR runtime for Linux isn't installed yet%s: VR games won't use VRmin", w); ok = false; }
        else if (!setXr(t, true)) { logmsg("[setup] Couldn't make VRmin the active OpenXR runtime%s (%s)", w, strerror(errno)); ok = false; }
        if (!exists(t.ocDir() + "/bin/linux64/vrclient.so")) { logmsg("[setup] OpenComposite for Linux isn't installed yet%s: SteamVR games won't use VRmin", w); ok = false; }
        else if (!setVrpath(t, true)) { logmsg("[setup] Couldn't activate OpenComposite%s", w); ok = false; }
        logmsg("[setup] OpenXR: %s · OpenComposite: %s%s", xrActive(t) ? "VRmin" : "other", ocActive(t) ? "active" : "no", w);
    }
    return ok;
}

void vrSetupOnExit() {
    for (const Target &t : targets()) {
        bool x = xrActive(t), o = ocActive(t);
        if (x) setXr(t, false);
        if (o) setVrpath(t, false);
        if (x || o) logmsg("[setup] The VR settings are back as they were%s", t.name.c_str());
    }
}

/// La memoria compartida que ofrece el runtime (un archivo en /dev/shm). El Steam de Flatpak tiene su propio /dev/shm:
/// en la PC es una carpeta /dev/shm/flatpak-com.valvesoftware.Steam-…, con el mismo archivo adentro.
int openGameShm(const std::string &path) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd >= 0 || path.rfind("/dev/shm/", 0) != 0 || path.find('/', 9) != std::string::npos) return fd;
    int err = errno;
    if (DIR *d = opendir("/dev/shm")) {
        while (dirent *e = readdir(d)) {
            if (strncmp(e->d_name, "flatpak-com.valvesoftware.Steam-", 32) != 0) continue;
            fd = open(("/dev/shm/" + std::string(e->d_name) + path.substr(8)).c_str(), O_RDWR);
            if (fd >= 0) break;
        }
        closedir(d);
    }
    if (fd < 0) errno = err;
    return fd;
}
