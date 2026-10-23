// VRmin: arreglo para juegos Unity (Mono) bajo Wine en la Mac.
// El Mono de Unity lee el "último error" de Windows directamente de %gs:0x68. En la Mac,
// %gs no apunta al TEB de Wine sino a los datos del hilo de macOS, y en hilos nuevos ese
// lugar vale 0 ("sin error"). Así, File.Exists de un archivo que no existe da "true".
// Este arreglo hace que cada hilo nuevo tenga ese lugar ocupado (con el búfer propio que
// macOS reserva ahí), de modo que Mono nunca vea un falso "sin error".
#ifdef __x86_64__   // solo Wine (Intel por Rosetta); en programas nativos de la Mac no hace nada
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#define DYLD_INTERPOSE(_r, _o) \
  __attribute__((used)) static struct { const void* r; const void* o; } _interpose_##_o \
  __attribute__((section("__DATA,__interpose"))) = { (const void*)(unsigned long)&_r, (const void*)(unsigned long)&_o };
static unsigned long gs68(void) { unsigned long v; __asm__ volatile("movq %%gs:0x68,%0" : "=r"(v)); return v; }
static void llenar(void) {
  if (gs68()) return;
  time_t t = 0; struct tm* a = gmtime(&t); (void)a;
  if (gs68()) return;
  a = localtime(&t); (void)a;
}
struct arranque { void* (*f)(void*); void* a; };
static void* inicio(void* p) {
  struct arranque w = *(struct arranque*)p; free(p);
  llenar();
  return w.f(w.a);
}
static int vrmin_pthread_create(pthread_t* t, const pthread_attr_t* at, void* (*f)(void*), void* a) {
  struct arranque* p = malloc(sizeof *p);
  if (!p) return pthread_create(t, at, f, a);
  p->f = f; p->a = a;
  int r = pthread_create(t, at, inicio, p);
  if (r) free(p);
  return r;
}
DYLD_INTERPOSE(vrmin_pthread_create, pthread_create)
__attribute__((constructor)) static void al_cargar(void) {
  llenar();
  if (getenv("VRMIN_DIAG")) fprintf(stderr, "[arreglo cargado]\n");
}
#endif
