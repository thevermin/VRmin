// VRmin Host para Linux (por ahora sin ventana: el estado y el código para emparejar salen en la terminal)
#include "host.h"
#include <clocale>
#include <csignal>
#include <thread>
#include <cstdlib>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

void vrSetupOnExit();

int main() {
    signal(SIGPIPE, SIG_IGN);
    // (al cerrar con Ctrl+C o al apagar: la VR de Linux queda como estaba)
    sigset_t quit;
    sigemptyset(&quit);
    sigaddset(&quit, SIGINT);
    sigaddset(&quit, SIGTERM);
    sigaddset(&quit, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &quit, nullptr);
    setlocale(LC_ALL, "");
    const char *lang = getenv("LANG");
    IDIOMA_ES = lang && strncmp(lang, "es", 2) == 0;
    // (uno solo a la vez: los puertos del Quest y del juego son los mismos)
    int lock = open((dataDir() + "/host.lock").c_str(), O_RDWR | O_CREAT, 0600);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "%s\n", tr("VRmin Host ya está abierto.", "VRmin Host is already open."));
        return 1;
    }
    logmsg("VRmin Host for Linux %s", VRMIN_LINUX_VERSION);
    loadSettings();
    applySettings();
    questStart();
    gameStart();
    lobbyStart();
    logmsg("%s", tr("Esperando al Quest: abrí VRmin en el visor (misma red Wi-Fi)", "Waiting for the Quest: open VRmin on the headset (same Wi-Fi)"));
    int sig = 0;
    sigwait(&quit, &sig);
    vrSetupOnExit();
    logmsg("VRmin Host closed");
    return 0;
}
