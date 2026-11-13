// Burrow's threads and signals test: prints «ok <name>» or «FAIL <name>», like prueba.c
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static void check(const char *name, int ok) { printf("%s %s\n", ok ? "ok" : "FAIL", name); if (!ok) fails++; }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter, ready;
static void *solo(void *arg);
static void *worker(void *arg) {
  for (int i = 0; i < 20000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
  pthread_mutex_lock(&mu); ready++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  return arg;
}
static void *solo(void *arg) { return arg; }
static volatile sig_atomic_t got;
static void handler(int s) { got = s; }

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);   // (each line right away: if something hangs, the last one says where)
  pthread_t t[8];
  int ok = 1;
  void *r1 = NULL;                       // first one thread alone, with no waiting: if this hangs, it's creating threads
  ok = pthread_create(&t[0], NULL, solo, (void *)42) == 0 && pthread_join(t[0], &r1) == 0 && r1 == (void *)42;
  check("thread-one", ok);
  for (long i = 0; i < 8; i++) ok &= pthread_create(&t[i], NULL, worker, (void *)i) == 0;
  pthread_mutex_lock(&mu);
  while (ready < 8) pthread_cond_wait(&cv, &mu);
  pthread_mutex_unlock(&mu);
  for (long i = 0; i < 8; i++) { void *r; pthread_join(t[i], &r); ok &= r == (void *)i; }
  check("threads", ok && counter == 160000);

  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = handler;
  sigaction(SIGUSR1, &sa, NULL);
  raise(SIGUSR1);
  check("signal", got == SIGUSR1);
  got = 0; sigaction(SIGALRM, &sa, NULL);
  alarm(1); pause();
  check("alarm", got == SIGALRM);

  pid_t p = fork();
  if (p == 0) _exit(7);
  int st = 0;
  check("fork", p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 7);
  int fd[2]; char buf[8] = "";
  ok = pipe(fd) == 0;
  if (ok && fork() == 0) { write(fd[1], "pipe", 5); _exit(0); }
  read(fd[0], buf, 5); wait(NULL);
  check("pipe", ok && !strcmp(buf, "pipe"));
  printf("%s\n", fails ? "HILOS: FALLÓ" : "HILOS: TODO BIEN");
  return fails != 0;
}
