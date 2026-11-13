// Burrow's step 1 test: each check prints «ok <name>» or «FAIL <name>: why». Run natively on Linux and under Burrow,
// both should print the same.
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(const char *name, int ok, const char *why) {
  if (ok) printf("ok %s\n", name);
  else { printf("FAIL %s: %s\n", name, why); fails++; }
}

int main(int argc, char **argv) {
  check("args", argc == 3 && !strcmp(argv[1], "uno") && !strcmp(argv[2], "dos"), "argv");
  check("env", getenv("BURROW_TEST") && !strcmp(getenv("BURROW_TEST"), "madriguera"), "BURROW_TEST");

  size_t big = 200u << 20;                        // 200 MB: comes from mmap
  char *m = malloc(big);
  int ok = m != NULL;
  if (ok) { for (size_t i = 0; i < big; i += 4096) m[i] = (char)i; for (size_t i = 0; i < big; i += 4096) ok &= m[i] == (char)i; }
  check("malloc", ok, "200 MB");
  free(m);
  char *small[1000]; ok = 1;                      // small ones: from brk
  for (int i = 0; i < 1000; i++) { small[i] = malloc(100 + i); if (!small[i]) ok = 0; else memset(small[i], i, 100 + i); }
  for (int i = 0; i < 1000; i++) { ok &= small[i][99 + i] == (char)i; free(small[i]); }
  check("brk", ok, "small mallocs");

  const char *dir = argc > 0 && getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char path[512], sub[512];
  snprintf(sub, sizeof sub, "%s/burrow-prueba-%d", dir, (int)getpid());
  check("mkdir", mkdir(sub, 0755) == 0, sub);
  snprintf(path, sizeof path, "%s/archivo.txt", sub);
  FILE *f = fopen(path, "w");
  check("fopen-w", f != NULL, path);
  if (f) { fprintf(f, "hola desde la madriguera %d\n", 42); fclose(f); }
  char line[128] = "";
  f = fopen(path, "r");
  if (f) { fgets(line, sizeof line, f); fclose(f); }
  check("fopen-r", !strcmp(line, "hola desde la madriguera 42\n"), line);
  struct stat st;
  check("stat", stat(path, &st) == 0 && st.st_size == 28 && S_ISREG(st.st_mode), "size/mode");
  int n = 0; DIR *d = opendir(sub); struct dirent *e;
  if (d) { while ((e = readdir(d))) if (!strcmp(e->d_name, "archivo.txt")) n++; closedir(d); }
  check("readdir", n == 1, "archivo.txt");
  check("open-missing", open("/nonexistent/burrow", O_RDONLY) < 0, "should fail");
  check("unlink", unlink(path) == 0 && rmdir(sub) == 0, "cleanup");

  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  struct timespec w = {0, 50 * 1000000};
  nanosleep(&w, NULL);
  clock_gettime(CLOCK_MONOTONIC, &b);
  long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
  check("time", ms >= 45 && ms < 1000, "nanosleep 50 ms");
  check("realtime", time(NULL) > 1700000000, "time()");

  struct utsname u;
  check("uname", uname(&u) == 0 && !strcmp(u.sysname, "Linux") && !strcmp(u.machine, "aarch64"), u.machine);
  char cwd[512];
  check("getcwd", getcwd(cwd, sizeof cwd) && cwd[0] == '/', "cwd");
  char exe[512]; ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (el > 0) exe[el] = 0;
  check("self-exe", el > 0 && strstr(exe, "prueba"), "readlink");
  printf("%s\n", fails ? "PRUEBA: FALLÓ" : "PRUEBA: TODO BIEN");
  return fails ? 1 : 0;
}
