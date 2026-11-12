// Preparar Linux para los juegos VR. Con Proton, los juegos de Windows usan la VR de Linux:
//  - OpenXR: el runtime activo del usuario (~/.config/openxr/1/active_runtime.json), que Proton usa por wineopenxr
//  - OpenVR (SteamVR): el primer runtime de ~/.config/openvr/openvrpaths.vrpath, del que Proton carga
//    bin/linux64/vrclient.so (ahí va OpenComposite para Linux)
// Mientras VRmin Host está abierto apuntan a VRmin; al cerrarlo quedan como estaban (lo de antes se guarda aparte).
#include "host.h"
#include <cerrno>
#include <climits>
#include <sys/stat.h>
#include <unistd.h>

static std::string configDir() {
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    return x && *x ? x : std::string(h ? h : "") + "/.config";
}
static std::string xrActiveFile() { return configDir() + "/openxr/1/active_runtime.json"; }
static std::string vrpathFile() { return configDir() + "/openvr/openvrpaths.vrpath"; }
static std::string ourXrJson() { return dataDir() + "/runtime/vrmin_openxr.json"; }
static std::string ocDir() { return dataDir() + "/OpenComposite"; }
static bool exists(const std::string &p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
static void mkdirs(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
}

bool vrRuntimeInstalled() { return exists(ourXrJson()); }
bool vrOcInstalled() { return exists(ocDir() + "/bin/linux64/vrclient.so"); }

static bool xrActive() {
    char b[PATH_MAX];
    ssize_t n = readlink(xrActiveFile().c_str(), b, sizeof b - 1);
    return n > 0 && std::string(b, n) == ourXrJson();
}
static bool ocActive() {
    Json j = Json::parse(readFile(vrpathFile()));
    const Json &r = j["runtime"];
    return r.t == Json::Arr && !r.a.empty() && r.a[0].str() == ocDir();
}

/// OpenXR: active_runtime.json pasa a ser un enlace al de VRmin. El que había (archivo o enlace) se guarda tal cual.
static bool setXr(bool ours) {
    std::string f = xrActiveFile(), saved = dataDir() + "/previous_active_runtime.json";
    if (ours) {
        if (xrActive()) return true;
        mkdirs(f.substr(0, f.rfind('/')));
        if (exists(f) && !exists(saved) && rename(f.c_str(), saved.c_str()) != 0) return false;
        unlink(f.c_str());
        return symlink(ourXrJson().c_str(), f.c_str()) == 0;
    }
    if (!xrActive()) return true;
    unlink(f.c_str());
    return !exists(saved) || rename(saved.c_str(), f.c_str()) == 0;
}

/// OpenVR: OpenComposite primero en la lista de runtimes (los demás quedan después, como en Windows)
static bool setVrpath(bool ours) {
    std::string f = vrpathFile();
    Json j = Json::parse(readFile(f));
    std::string made = dataDir() + "/made_openvrpaths";   // (no había: al cerrar se borra)
    if (!ours && exists(made)) { unlink(made.c_str()); return unlink(f.c_str()) == 0 || errno == ENOENT; }
    if (j.t != Json::Obj) {
        if (!ours) return true;
        writeFile(made, "");
        j = Json::object();
        j.set("config", Json::array().push(configDir() + "/openvr")).set("log", Json::array().push(configDir() + "/openvr"))
         .set("external_drivers", Json()).set("jsonid", "vrpathreg").set("version", 1);
    }
    Json rt = Json::array();
    if (ours) rt.push(ocDir());
    for (auto &x : j["runtime"].a) if (x.str() != ocDir()) rt.push(x);
    j.set("runtime", rt);
    mkdirs(f.substr(0, f.rfind('/')));
    return writeFile(f, j.dump());
}

bool vrSetupActivate() {
    bool ok = true;
    if (!vrRuntimeInstalled()) { logmsg("[setup] VRmin's OpenXR runtime for Linux isn't installed yet: VR games won't use VRmin"); ok = false; }
    else if (!setXr(true)) { logmsg("[setup] Couldn't make VRmin the active OpenXR runtime (%s)", strerror(errno)); ok = false; }
    if (!vrOcInstalled()) { logmsg("[setup] OpenComposite for Linux isn't installed yet: SteamVR games won't use VRmin"); ok = false; }
    else if (!setVrpath(true)) { logmsg("[setup] Couldn't activate OpenComposite"); ok = false; }
    logmsg("[setup] OpenXR: %s · OpenComposite: %s", xrActive() ? "VRmin" : "other", ocActive() ? "active" : "no");
    return ok;
}

void vrSetupOnExit() {
    bool x = xrActive(), o = ocActive();
    if (x) setXr(false);
    if (o) setVrpath(false);
    if (x || o) logmsg("[setup] The VR settings are back as they were");
}
