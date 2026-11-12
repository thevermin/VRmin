// X11 para el escritorio y las ventanas en el visor (desktop.cpp y windows.cpp)
#pragma once
#include "host.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xcomposite.h>
#undef Bool   // (Xlib lo define como macro y choca con Json::Bool: acá se usa int)

static const uint32_t DISPLAY_BIT = 0x40000000;   // los monitores se identifican como en Windows y la Mac: 0x40000000 | número

Display *xOpen();                  // una conexión propia (cada hilo de captura usa la suya); nullptr si no hay X11
Display *xMain();                  // la compartida (monitores, lista de ventanas, mouse y teclado): solo con xMainLock tomado
extern std::mutex xMainLock;
extern thread_local int t_xError;  // el último error de X11 en este hilo (0: ninguno)
std::vector<DisplayInfo> monitorsOf(Display *d);
// Una propiedad de 32 bits (ventanas, números) de una ventana; vacía si no la tiene
std::vector<unsigned long> xLongs(Display *d, Window w, const char *prop, Atom type);
