// VRmin Host para Linux: con ventana si hay pantalla (ui.cpp); si no, sin ventana (el estado y el código para emparejar
// salen en la terminal)
#include "host.h"
#include <clocale>
#include <csignal>
#include <thread>
#include <cstdlib>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

void vrSetupOnExit();
bool uiRun(const sigset_t &quit);

/// Al salir (Ctrl+C, apagar o cerrar la ventana): lo que haya que dejar como estaba va acá, en un solo lugar
static void onExit() {
    vrSetupOnExit();
}

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
    deskStart();
    logmsg("%s", tr("Esperando al Quest: abrí VRmin en el visor (misma red Wi-Fi)", "Waiting for the Quest: open VRmin on the headset (same Wi-Fi)"));
    if (!uiRun(quit)) {   // (vuelve al cerrar la ventana; sin pantalla, enseguida)
        int sig = 0;
        sigwait(&quit, &sig);
    }
    onExit();
    logmsg("VRmin Host closed");
    // (sin los destructores de lo global: los hilos siguen corriendo, y destruir una condition_variable que un hilo
    // espera traba la salida para siempre)
    _exit(0);
}
