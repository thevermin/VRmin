// Lo que todavía no está hecho en Linux: el codificador de video, los juegos de Steam y el micrófono del visor.
// Mientras tanto no hacen nada (el Quest se conecta y el juego también, pero no llega video).
#include "host.h"

bool VideoEncoder::encode(const uint8_t *, int, int, int, int, bool, bool, uint64_t) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) logmsg("[video] There's no video encoder on Linux yet: the game's frames arrive but aren't sent to the Quest");
    return false;
}
void VideoEncoder::reset() {}
bool encoderCanHevc(int, int, int, int) { return false; }

std::vector<GameEntry> gamesList() { return {}; }
void gamesLaunch(const std::string &id) { logmsg("[games] Opening games from the Quest isn't available on Linux yet (%s)", id.c_str()); }
void gamesQuit() {}
std::string gamesRunningName() { return ""; }

void micPush(const uint8_t *, size_t) {}
