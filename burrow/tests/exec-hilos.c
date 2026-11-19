// Un hilo que no es el principal abre otros programas, como hace Steam: con posix_spawn (vfork por dentro), con
// fork + execve, y con execve directo (que reemplaza todo el programa). Cada paso dice si anduvo.
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static const char *yo;

static void *con_spawn(void *a) {
  (void) a;
  pid_t p; char *args[] = {(char *) yo, "hijo", "spawn", NULL};
  if (posix_spawn(&p, yo, NULL, NULL, args, environ) != 0) { puts("spawn: no arrancó"); return NULL; }
  int st; waitpid(p, &st, 0);
  printf("spawn: terminó con %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return NULL;
}

static void *con_fork(void *a) {
  (void) a;
  fflush(stdout);
  pid_t p = fork();
  if (p == 0) { char *args[] = {(char *) yo, "hijo", "fork", NULL}; execve(yo, args, environ); _exit(9); }
  int st; waitpid(p, &st, 0);
  printf("fork: terminó con %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return NULL;
}

static void *con_exec(void *a) {
  (void) a;
  fflush(stdout);
  char *args[] = {(char *) yo, "final", NULL};
  execve(yo, args, environ);
  puts("exec: no anduvo");
  return NULL;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc > 2 && !strcmp(argv[1], "hijo")) { printf("hijo de %s\n", argv[2]); return 3; }
  if (argc > 1 && !strcmp(argv[1], "final")) { puts("EXEC-HILOS: TODO BIEN"); return 0; }
  yo = argv[0];
  pthread_t t;
  pthread_create(&t, NULL, con_spawn, NULL); pthread_join(t, NULL);
  pthread_create(&t, NULL, con_fork, NULL); pthread_join(t, NULL);
  pthread_create(&t, NULL, con_exec, NULL); pthread_join(t, NULL);
  puts("exec: el programa siguió (no se reemplazó)");
  return 1;
}
