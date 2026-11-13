// Burrow: runs Linux ARM64 programs on Apple Silicon Macs (see README.md).
//
// Memory: one big block of Mac memory is the program's whole address space. Program address A is at mem + A on the Mac
// and at guest physical address A (identity), so any pointer the program passes is just mem + pointer here.
// Layout (36-bit, 64 GB):
//   0 .. 2 MB          not mapped (null pointers fault)
//   0x00400000 ..      non-PIE programs, where they ask to be
//   0x10000000         PIE programs
//   0x30000000         the dynamic loader (ld-linux)
//   after the program  brk (up to 1 GB)
//   0x200000000 ..     mmap
//   .. 0xF00000000     stack (grows down)
//   last GB            Burrow's own: page tables and the exception vectors (only EL1 can touch it)
// The program runs at EL0. A system call (svc) goes to our EL1 vectors, which do «hvc #0» and come back here; we read
// the registers, answer the call with macOS and return to EL0 after the svc.
#include <Hypervisor/Hypervisor.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define GB (1ull << 30)
#define MEM_SIZE (64 * GB)
#define EL1_BASE (63 * GB)         // page tables + vectors
#define PIE_BASE 0x10000000ull
#define INTERP_BASE 0x30000000ull
#define MMAP_BASE 0x200000000ull
#define STACK_TOP 0xF00000000ull
#define STACK_SIZE (64ull << 20)
#define MMAP_END (STACK_TOP - STACK_SIZE)

static uint8_t *mem;
static uint64_t brk_start, brk_cur, mmap_next = MMAP_BASE;
static const char *root;           // -r: the Linux system's folder, seen as / by the program
static char exe_path[1024];        // what /proc/self/exe points to (as the program sees it)
static int logcalls;

static void die(const char *fmt, ...) {
  va_list a; va_start(a, fmt);
  fprintf(stderr, "burrow: "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n");
  va_end(a); exit(127);
}
#define HV(x) do { hv_return_t r_ = (x); if (r_ != HV_SUCCESS) die("%s: 0x%x", #x, r_); } while (0)

// A program pointer → Mac pointer (NULL if it's outside the program's memory)
static void *g2h(uint64_t a, uint64_t len) {
  if (a < (2ull << 20) || a >= EL1_BASE || len > EL1_BASE - a) return NULL;
  return mem + a;
}

// ---- Linux constants (arm64) ----
enum {
  L_EPERM = 1, L_ENOENT = 2, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12, L_EFAULT = 14, L_EINVAL = 22, L_ENOTTY = 25,
  L_ERANGE = 34, L_ENOSYS = 38, L_ENAMETOOLONG = 36, L_ENOTEMPTY = 39, L_ELOOP = 40, L_EOVERFLOW = 75,
  L_ENOTSUP = 95, L_ETIMEDOUT = 110,
};
// macOS errno → Linux errno (1-34 are the same, except 11)
static int64_t lerr(int e) {
  switch (e) {
  case EDEADLK: return -35; case EAGAIN: return -L_EAGAIN; case EINPROGRESS: return -115; case EALREADY: return -114;
  case ENOTSOCK: return -88; case EDESTADDRREQ: return -89; case EMSGSIZE: return -90; case EPROTOTYPE: return -91;
  case ENOPROTOOPT: return -92; case EPROTONOSUPPORT: return -93; case ENOTSUP: return -L_ENOTSUP;
  case EAFNOSUPPORT: return -97; case EADDRINUSE: return -98; case EADDRNOTAVAIL: return -99; case ENETDOWN: return -100;
  case ENETUNREACH: return -101; case ECONNABORTED: return -103; case ECONNRESET: return -104; case ENOBUFS: return -105;
  case EISCONN: return -106; case ENOTCONN: return -107; case ETIMEDOUT: return -L_ETIMEDOUT;
  case ECONNREFUSED: return -111; case ELOOP: return -L_ELOOP; case ENAMETOOLONG: return -L_ENAMETOOLONG;
  case EHOSTUNREACH: return -113; case ENOTEMPTY: return -L_ENOTEMPTY; case ENOSYS: return -L_ENOSYS;
  case EOVERFLOW: return -L_EOVERFLOW; case ECANCELED: return -125; case EILSEQ: return -84;
  default: return e > 0 && e <= 34 ? -e : -L_EINVAL;
  }
}
#define R(x) ({ long r_ = (long)(x); r_ < 0 ? lerr(errno) : r_; })

#define L_AT_FDCWD -100
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_REMOVEDIR 0x200
#define L_AT_EMPTY_PATH 0x1000

static int oflags(uint64_t f) {
  int o = f & 3;   // O_RDONLY/O_WRONLY/O_RDWR are the same
  if (f & 0100) o |= O_CREAT;
  if (f & 0200) o |= O_EXCL;
  if (f & 0400) o |= O_NOCTTY;
  if (f & 01000) o |= O_TRUNC;
  if (f & 02000) o |= O_APPEND;
  if (f & 04000) o |= O_NONBLOCK;
  if (f & 040000) o |= O_DIRECTORY;
  if (f & 0100000) o |= O_NOFOLLOW;
  if (f & 02000000) o |= O_CLOEXEC;
  return o;
}

// Program path → Mac path (with -r, absolute paths are inside the root)
static int hpath(uint64_t gp, char *out, size_t n) {
  const char *p = g2h(gp, 1);
  if (!p) return -L_EFAULT;
  size_t len = strnlen(p, EL1_BASE - gp);
  if (len >= 4096) return -L_ENAMETOOLONG;
  if (root && p[0] == '/') {
    if (snprintf(out, n, "%s%s", root, p) >= (int)n) return -L_ENAMETOOLONG;
  } else if (snprintf(out, n, "%s", p) >= (int)n) return -L_ENAMETOOLONG;
  return 0;
}
static int hdirfd(int64_t fd) { return fd == L_AT_FDCWD ? AT_FDCWD : (int)fd; }

// struct stat (arm64 Linux)
struct lstat {
  uint64_t dev, ino; uint32_t mode, nlink, uid, gid; uint64_t rdev, pad1; int64_t size; int32_t blksize, pad2;
  int64_t blocks, atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; uint32_t unused[2];
};
static void tostat(const struct stat *s, struct lstat *l) {
  memset(l, 0, sizeof *l);
  l->dev = (uint64_t)s->st_dev; l->ino = s->st_ino; l->mode = s->st_mode; l->nlink = s->st_nlink;
  l->uid = s->st_uid; l->gid = s->st_gid; l->rdev = (uint64_t)s->st_rdev; l->size = s->st_size;
  l->blksize = s->st_blksize; l->blocks = s->st_blocks;
  l->atime = s->st_atimespec.tv_sec; l->atime_ns = s->st_atimespec.tv_nsec;
  l->mtime = s->st_mtimespec.tv_sec; l->mtime_ns = s->st_mtimespec.tv_nsec;
  l->ctime = s->st_ctimespec.tv_sec; l->ctime_ns = s->st_ctimespec.tv_nsec;
}

static clockid_t hclock(int64_t c) {
  switch (c) {
  case 0: case 5: return CLOCK_REALTIME;                   // REALTIME, REALTIME_COARSE
  case 1: case 6: case 7: return CLOCK_MONOTONIC;          // MONOTONIC, MONOTONIC_COARSE, BOOTTIME
  case 4: return CLOCK_MONOTONIC_RAW;
  case 2: return CLOCK_PROCESS_CPUTIME_ID;
  case 3: return CLOCK_THREAD_CPUTIME_ID;
  default: return (clockid_t)-1;
  }
}

// ---- memory ----
static uint64_t page_up(uint64_t x) { return (x + 0xfff) & ~0xfffull; }

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
  (void)prot;
  if (!len || (off & 0xfff)) return -L_EINVAL;
  len = page_up(len);
  uint64_t a;
  if (flags & (0x10 | 0x100000)) {          // MAP_FIXED, MAP_FIXED_NOREPLACE
    if ((addr & 0xfff) || !g2h(addr, len)) return -L_ENOMEM;
    a = addr;
    memset(mem + a, 0, len);                // (it may be memory the program used before)
  } else {
    a = (mmap_next + 0x3fff) & ~0x3fffull;  // fresh memory: already zero
    if (a + len > MMAP_END) return -L_ENOMEM;
    mmap_next = a + len;
  }
  if (!(flags & 0x20) && fd >= 0) {         // a file: copy it in (MAP_SHARED writes don't go back to the file yet)
    uint64_t done = 0;
    while (done < len) {
      ssize_t n = pread((int)fd, mem + a + done, len - done, (off_t)(off + done));
      if (n < 0) return lerr(errno);
      if (n == 0) break;
      done += n;
    }
  }
  return (int64_t)a;
}

static int64_t sys_brk(uint64_t want) {
  if (want >= brk_start && want <= brk_start + GB) {
    if (want > brk_cur) memset(mem + brk_cur, 0, want - brk_cur);   // (it may have shrunk before)
    brk_cur = want;
  }
  return (int64_t)brk_cur;
}

// ---- system calls ----
static int64_t sys_writev(int fd, uint64_t iov, int cnt, int wr, int64_t off) {
  if (cnt < 0 || cnt > 1024) return -L_EINVAL;
  uint64_t *gv = g2h(iov, (uint64_t)cnt * 16);
  if (!gv && cnt) return -L_EFAULT;
  struct iovec v[1024];
  for (int i = 0; i < cnt; i++) {
    v[i].iov_len = gv[i * 2 + 1];
    v[i].iov_base = v[i].iov_len ? g2h(gv[i * 2], gv[i * 2 + 1]) : mem;
    if (!v[i].iov_base) return -L_EFAULT;
  }
  if (off >= 0) return R(wr ? pwritev(fd, v, cnt, off) : preadv(fd, v, cnt, off));
  return R(wr ? writev(fd, v, cnt) : readv(fd, v, cnt));
}

static int64_t sys_getdents64(int fd, uint64_t buf, uint64_t len) {
  // macOS has no getdents: read the folder with its own DIR, remembering where we were by the file's offset
  uint8_t *out = g2h(buf, len);
  if (!out) return -L_EFAULT;
  int d2 = dup(fd);
  if (d2 < 0) return lerr(errno);
  off_t pos = lseek(fd, 0, SEEK_CUR);   // our position: how many entries were already given
  DIR *d = fdopendir(d2);
  if (!d) { close(d2); return lerr(errno); }
  rewinddir(d);
  struct dirent *e;
  off_t i = 0;
  uint64_t used = 0;
  while ((e = readdir(d))) {
    if (i++ < pos) continue;
    size_t nl = strlen(e->d_name), rl = (19 + nl + 1 + 7) & ~7ul;
    if (used + rl > len) { i--; break; }
    uint8_t *r = out + used;
    memset(r, 0, rl);
    memcpy(r, &e->d_ino, 8);
    int64_t next = i; memcpy(r + 8, &next, 8);
    uint16_t rl16 = (uint16_t)rl; memcpy(r + 16, &rl16, 2);
    r[18] = e->d_type;                   // DT_* are the same
    memcpy(r + 19, e->d_name, nl);
    used += rl;
  }
  closedir(d);
  if (used == 0 && e) return -L_EINVAL;  // buffer too small for one entry
  lseek(fd, i, SEEK_SET);
  return (int64_t)used;
}

static int64_t syscall_(uint64_t nr, uint64_t *x) {
  char p1[4096], p2[4096];
  int r;
  switch (nr) {
  case 63: { void *b = g2h(x[1], x[2]); return b || !x[2] ? R(read((int)x[0], b ? b : mem, x[2])) : -L_EFAULT; }
  case 64: { void *b = g2h(x[1], x[2]); return b || !x[2] ? R(write((int)x[0], b ? b : mem, x[2])) : -L_EFAULT; }
  case 65: return sys_writev((int)x[0], x[1], (int)x[2], 0, -1);
  case 66: return sys_writev((int)x[0], x[1], (int)x[2], 1, -1);
  case 67: { void *b = g2h(x[1], x[2]); return b ? R(pread((int)x[0], b, x[2], (off_t)x[3])) : -L_EFAULT; }
  case 68: { void *b = g2h(x[1], x[2]); return b ? R(pwrite((int)x[0], b, x[2], (off_t)x[3])) : -L_EFAULT; }
  case 69: return sys_writev((int)x[0], x[1], (int)x[2], 0, (int64_t)x[3]);
  case 70: return sys_writev((int)x[0], x[1], (int)x[2], 1, (int64_t)x[3]);
  case 56:                                                                   // openat
    if ((r = hpath(x[1], p1, sizeof p1))) return r;
    return R(openat(hdirfd((int64_t)x[0]), p1, oflags(x[2]), (int)x[3]));
  case 57: return x[0] <= 2 ? 0 : R(close((int)x[0]));                       // (stdin/out/err stay ours)
  case 62: return R(lseek((int)x[0], (off_t)x[1], (int)x[2]));
  case 61: return sys_getdents64((int)x[0], x[1], x[2]);
  case 79: case 80: {                                                         // newfstatat, fstat
    struct stat s;
    struct lstat *l = g2h(nr == 80 ? x[1] : x[2], sizeof *l);
    if (!l) return -L_EFAULT;
    if (nr == 80) r = fstat((int)x[0], &s);
    else {
      const char *gp = g2h(x[1], 1);
      if (gp && !gp[0] && (x[3] & L_AT_EMPTY_PATH)) r = x[0] == (uint64_t)L_AT_FDCWD ? stat(".", &s) : fstat((int)x[0], &s);
      else {
        int e = hpath(x[1], p1, sizeof p1);
        if (e) return e;
        r = fstatat(hdirfd((int64_t)x[0]), p1, &s, (x[3] & L_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0);
      }
    }
    if (r < 0) return lerr(errno);
    tostat(&s, l);
    return 0;
  }
  case 291: return -L_ENOSYS;                                                // statx: glibc falls back to newfstatat
  case 78: {                                                                 // readlinkat
    char *b = g2h(x[2], x[3]);
    if (!b) return -L_EFAULT;
    const char *gp = g2h(x[1], 1);
    if (gp && !strcmp(gp, "/proc/self/exe")) {
      size_t n = strlen(exe_path) < x[3] ? strlen(exe_path) : x[3];
      memcpy(b, exe_path, n);
      return (int64_t)n;
    }
    if ((r = hpath(x[1], p1, sizeof p1))) return r;
    return R(readlinkat(hdirfd((int64_t)x[0]), p1, b, x[3]));
  }
  case 48: case 439:                                                         // faccessat, faccessat2
    if ((r = hpath(x[1], p1, sizeof p1))) return r;
    return R(faccessat(hdirfd((int64_t)x[0]), p1, (int)x[2], 0));
  case 17: {                                                                 // getcwd
    char *b = g2h(x[0], x[1]);
    if (!b) return -L_EFAULT;
    if (!getcwd(p1, sizeof p1)) return lerr(errno);
    const char *c = p1;
    if (root && !strncmp(p1, root, strlen(root))) c = p1[strlen(root)] ? p1 + strlen(root) : "/";
    if (strlen(c) + 1 > x[1]) return -L_ERANGE;
    strcpy(b, c);
    return (int64_t)strlen(c) + 1;
  }
  case 49:                                                                   // chdir
    if ((r = hpath(x[0], p1, sizeof p1))) return r;
    return R(chdir(p1));
  case 34:                                                                   // mkdirat
    if ((r = hpath(x[1], p1, sizeof p1))) return r;
    return R(mkdirat(hdirfd((int64_t)x[0]), p1, (mode_t)x[2]));
  case 35:                                                                   // unlinkat
    if ((r = hpath(x[1], p1, sizeof p1))) return r;
    return R(unlinkat(hdirfd((int64_t)x[0]), p1, (x[2] & L_AT_REMOVEDIR) ? AT_REMOVEDIR : 0));
  case 38: case 276:                                                         // renameat, renameat2 (no flags)
    if (nr == 276 && x[4]) return -L_EINVAL;
    if ((r = hpath(x[1], p1, sizeof p1)) || (r = hpath(x[3], p2, sizeof p2))) return r;
    return R(renameat(hdirfd((int64_t)x[0]), p1, hdirfd((int64_t)x[2]), p2));
  case 23: return R(dup((int)x[0]));
  case 24: return R(dup2((int)x[0], (int)x[1]));                            // dup3 (O_CLOEXEC ignored)
  case 25:                                                                   // fcntl
    switch (x[1]) {
    case 0: case 1030: return R(fcntl((int)x[0], x[1] ? F_DUPFD_CLOEXEC : F_DUPFD, (int)x[2]));
    case 1: return R(fcntl((int)x[0], F_GETFD));
    case 2: return R(fcntl((int)x[0], F_SETFD, (int)x[2]));
    case 3: { int f = fcntl((int)x[0], F_GETFL); if (f < 0) return lerr(errno);
              int l = f & 3; if (f & O_APPEND) l |= 02000; if (f & O_NONBLOCK) l |= 04000; return l; }
    case 4: { int f = (x[2] & 02000 ? O_APPEND : 0) | (x[2] & 04000 ? O_NONBLOCK : 0); return R(fcntl((int)x[0], F_SETFL, f)); }
    default: return -L_EINVAL;
    }
  case 29: return -L_ENOTTY;                                                 // ioctl (no terminals yet)
  case 214: return sys_brk(x[0]);
  case 222: return sys_mmap(x[0], x[1], x[2], x[3], (int64_t)x[4], x[5]);
  case 215: case 226: case 233: return 0;                                    // munmap, mprotect, madvise
  case 216: return -L_ENOMEM;                                                // mremap: callers fall back to a new mmap
  case 160: {                                                                // uname
    char *u = g2h(x[0], 65 * 6);
    if (!u) return -L_EFAULT;
    memset(u, 0, 65 * 6);
    strcpy(u, "Linux"); strcpy(u + 65, "burrow"); strcpy(u + 130, "6.8.0-burrow");
    strcpy(u + 195, "#1 SMP"); strcpy(u + 260, "aarch64"); strcpy(u + 325, "(none)");
    return 0;
  }
  case 172: case 178: return getpid();                                       // getpid, gettid (one thread)
  case 173: return getppid();
  case 174: return getuid(); case 175: return geteuid();
  case 176: return getgid(); case 177: return getegid();
  case 96: return getpid();                                                  // set_tid_address
  case 99: return 0;                                                         // set_robust_list
  case 293: return -L_ENOSYS;                                                // rseq
  case 98: return (x[1] & 0x7f) == 1 ? 0 : -L_EAGAIN;                        // futex: one thread, nobody waits
  case 113: {                                                                // clock_gettime
    struct timespec *t = g2h(x[1], 16);
    clockid_t c = hclock((int64_t)x[0]);
    if (!t) return -L_EFAULT;
    if (c == (clockid_t)-1) return -L_EINVAL;
    return R(clock_gettime(c, t));
  }
  case 114: {                                                                // clock_getres
    struct timespec *t = x[1] ? g2h(x[1], 16) : NULL;
    if (t) { t->tv_sec = 0; t->tv_nsec = 1; }
    return 0;
  }
  case 169: {                                                                // gettimeofday
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    int64_t *tv = x[0] ? g2h(x[0], 16) : NULL;
    if (tv) { tv[0] = t.tv_sec; tv[1] = t.tv_nsec / 1000; }
    return 0;
  }
  case 101: case 115: {                                                      // nanosleep, clock_nanosleep
    struct timespec *t = g2h(nr == 101 ? x[0] : x[2], 16);
    if (!t) return -L_EFAULT;
    if (nr == 115 && (x[1] & 1)) {                                           // TIMER_ABSTIME
      struct timespec now; clock_gettime(hclock((int64_t)x[0]), &now);
      int64_t ns = (t->tv_sec - now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
      if (ns <= 0) return 0;
      struct timespec d = {ns / 1000000000ll, ns % 1000000000ll};
      return R(nanosleep(&d, NULL));
    }
    return R(nanosleep(t, NULL));
  }
  case 278: {                                                                // getrandom
    uint8_t *b = g2h(x[0], x[1]);
    if (!b) return -L_EFAULT;
    for (uint64_t i = 0; i < x[1]; i += 256) getentropy(b + i, x[1] - i < 256 ? x[1] - i : 256);
    return (int64_t)x[1];
  }
  case 261: {                                                                // prlimit64 (only reading)
    uint64_t *old = x[3] ? g2h(x[3], 16) : NULL;
    if (old) {
      if (x[1] == 3) { old[0] = 8 << 20; old[1] = STACK_SIZE; }              // RLIMIT_STACK
      else if (x[1] == 7) { old[0] = 1024; old[1] = 4096; }                  // RLIMIT_NOFILE
      else old[0] = old[1] = ~0ull;
    }
    return 0;
  }
  case 134: case 135: return 0;                                              // rt_sigaction, rt_sigprocmask (no signals yet)
  case 122: {                                                                // sched_getaffinity: one CPU
    uint8_t *m = g2h(x[2], x[1]);
    if (!m || x[1] < 8) return -L_EINVAL;
    memset(m, 0, x[1]); m[0] = 1;
    return 8;
  }
  case 124: return 0;                                                        // sched_yield
  case 167: return -L_EINVAL;                                                // prctl
  case 166: return 022;                                                      // umask
  default:
    fprintf(stderr, "burrow: system call %llu not done yet (returns ENOSYS)\n", (unsigned long long)nr);
    return -L_ENOSYS;
  }
}

// ---- loading the program ----
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;

typedef struct { uint64_t base, entry, phdr, end; uint16_t phnum; char interp[512]; } Loaded;

static void load_elf(const char *path, uint64_t pie_base, Loaded *L) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("can't open %s: %s", path, strerror(errno));
  Ehdr e;
  if (pread(fd, &e, sizeof e, 0) != sizeof e || memcmp(e.ident, "\177ELF", 4) || e.ident[4] != 2)
    die("%s isn't a 64-bit Linux program", path);
  if (e.machine != 183) die("%s isn't for ARM64 (machine %u)", path, e.machine);
  if (e.type != 2 && e.type != 3) die("%s isn't a program (ELF type %u)", path, e.type);
  if (e.phentsize != sizeof(Phdr) || e.phnum > 64) die("%s: unexpected program headers", path);
  Phdr ph[64];
  if (pread(fd, ph, e.phnum * sizeof(Phdr), (off_t)e.phoff) != (ssize_t)(e.phnum * sizeof(Phdr))) die("%s: short read", path);
  uint64_t base = e.type == 3 ? pie_base : 0;
  memset(L, 0, sizeof *L);
  L->base = base; L->entry = base + e.entry; L->phnum = e.phnum;
  for (int i = 0; i < e.phnum; i++) {
    Phdr *p = &ph[i];
    if (p->type == 3) {                                                      // PT_INTERP
      if (p->filesz >= sizeof L->interp) die("%s: interpreter path too long", path);
      if (pread(fd, L->interp, p->filesz, (off_t)p->offset) != (ssize_t)p->filesz) die("%s: short read", path);
      L->interp[p->filesz] = 0;
    }
    if (p->type == 6) L->phdr = base + p->vaddr;                             // PT_PHDR
    if (p->type != 1) continue;                                              // PT_LOAD
    uint64_t va = base + p->vaddr;
    if (!g2h(va, p->memsz)) die("%s: segment at 0x%llx is outside Burrow's memory", path, (unsigned long long)va);
    if (p->filesz && pread(fd, mem + va, p->filesz, (off_t)p->offset) != (ssize_t)p->filesz) die("%s: short read", path);
    memset(mem + va + p->filesz, 0, p->memsz - p->filesz);
    if (!L->phdr && p->offset == 0) L->phdr = va + e.phoff;
    if (va + p->memsz > L->end) L->end = va + p->memsz;
  }
  close(fd);
}

static uint64_t push(uint64_t *sp, const void *data, size_t n) {
  *sp -= n;
  memcpy(mem + *sp, data, n);
  return *sp;
}

// argc, argv, envp and auxv on the stack, the way Linux leaves them
static uint64_t make_stack(int argc, char **argv, Loaded *prog, Loaded *interp) {
  uint64_t sp = STACK_TOP;
  extern char **environ;
  uint64_t av[256], ev[512];
  int ne = 0;
  if (argc > 255) die("too many arguments");
  for (int i = argc - 1; i >= 0; i--) av[i] = push(&sp, argv[i], strlen(argv[i]) + 1);
  for (char **e = environ; *e && ne < 511; e++) {
    if (!strncmp(*e, "DYLD_", 5) || !strncmp(*e, "BURROW_", 7)) continue;
    ev[ne++] = push(&sp, *e, strlen(*e) + 1);
  }
  uint64_t execfn = push(&sp, exe_path, strlen(exe_path) + 1);
  uint64_t platform = push(&sp, "aarch64", 8);
  uint8_t rnd[16]; getentropy(rnd, 16);
  uint64_t random = push(&sp, rnd, 16);
  // HWCAP: FP ASIMD AES PMULL SHA1 SHA2 CRC32 ATOMICS FPHP ASIMDHP ASIMDRDM JSCVT FCMA LRCPC DCPOP ASIMDDP
  uint64_t hwcap = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) |
                   (1 << 10) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15) | (1 << 16) | (1 << 20);
  uint64_t aux[] = {
    3, prog->phdr, 4, sizeof(Phdr), 5, prog->phnum, 6, 4096, 7, interp ? interp->base : 0, 8, 0, 9, prog->entry,
    11, getuid(), 12, geteuid(), 13, getgid(), 14, getegid(), 16, hwcap, 26, 0, 17, 100, 23, 0,
    25, random, 31, execfn, 15, platform, 0, 0,
  };
  size_t words = 1 + argc + 1 + ne + 1 + sizeof aux / 8;
  sp = (sp - words * 8) & ~15ull;
  uint64_t *s = (uint64_t *)(mem + sp);
  *s++ = argc;
  for (int i = 0; i < argc; i++) *s++ = av[i];
  *s++ = 0;
  for (int i = 0; i < ne; i++) *s++ = ev[i];
  *s++ = 0;
  memcpy(s, aux, sizeof aux);
  return sp;
}

// ---- the virtual CPU ----
static void setup_el1(void) {
  uint64_t *l0 = (uint64_t *)(mem + EL1_BASE), *l1 = l0 + 512, *l2 = l1 + 512;
  memset(l0, 0, 3 * 4096);
  const uint64_t user = 1 | (1 << 6) | (3 << 8) | (1 << 10) | (1ull << 53);  // block, EL0 RW, inner shareable, AF, PXN
  const uint64_t el1 = 1 | (3 << 8) | (1 << 10) | (1ull << 54);              // block, EL1 only, AF, UXN
  l0[0] = (EL1_BASE + 4096) | 3;
  l1[0] = (EL1_BASE + 8192) | 3;
  for (int i = 1; i < 512; i++) l2[i] = ((uint64_t)i << 21) | user;          // first GB in 2 MB pieces (0..2 MB missing)
  for (int i = 1; i < 63; i++) l1[i] = ((uint64_t)i << 30) | user;
  l1[63] = EL1_BASE | el1;
  // Exception vectors: all 16 do «hvc #0» (back to Burrow)
  uint32_t *v = (uint32_t *)(mem + EL1_BASE + 0x10000);
  for (int i = 0; i < 16; i++) v[i * 32] = 0xd4000002;
}

static void setup_vcpu(hv_vcpu_t c, uint64_t pc, uint64_t sp) {
  uint64_t sctlr = 0x30d00800ull                    // reserved ones
                 | 1 | (1 << 2) | (1 << 3) | (1 << 4)  // MMU, data cache, SP alignment checks (EL1, EL0)
                 | (1 << 12) | (1 << 14) | (1 << 15)   // instruction cache, DC ZVA and CTR_EL0 at EL0
                 | (1 << 16) | (1 << 18) | (1 << 26);  // WFI, WFE and cache maintenance at EL0 don't trap
  uint64_t tcr = 16 | (1 << 8) | (1 << 10) | (3 << 12) | (1 << 23) | (1ull << 32) | (1ull << 37);
  //             T0SZ=16, write-back, inner shareable, 4 KB pages, no TTBR1, 36-bit addresses, top byte ignored
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_MAIR_EL1, 0xff));
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_TCR_EL1, tcr));
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_TTBR0_EL1, EL1_BASE));
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_VBAR_EL1, EL1_BASE + 0x10000));
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_CPACR_EL1, 3 << 20));                // FP/SIMD
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_CNTKCTL_EL1, 3));                    // EL0 reads the counters
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_SCTLR_EL1, sctlr));
  HV(hv_vcpu_set_sys_reg(c, HV_SYS_REG_SP_EL0, sp));
  HV(hv_vcpu_set_reg(c, HV_REG_PC, pc));
  HV(hv_vcpu_set_reg(c, HV_REG_CPSR, 0));                                   // EL0t
  for (int i = 0; i < 31; i++) HV(hv_vcpu_set_reg(c, HV_REG_X0 + i, 0));
}

static const char *ec_name(uint64_t ec) {
  switch (ec) {
  case 0x00: return "unknown instruction"; case 0x07: return "FP/SIMD access"; case 0x0e: return "illegal state";
  case 0x18: return "system register access"; case 0x20: return "instruction fetch fault"; case 0x22: return "misaligned PC";
  case 0x24: return "memory access fault"; case 0x26: return "misaligned stack"; case 0x2c: return "floating point exception";
  case 0x3c: return "breakpoint (brk)"; default: return "exception";
  }
}

static void run(hv_vcpu_t c, hv_vcpu_exit_t *ex) {
  for (;;) {
    HV(hv_vcpu_run(c));
    if (ex->reason != HV_EXIT_REASON_EXCEPTION) {
      if (ex->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) { hv_vcpu_set_vtimer_mask(c, false); continue; }
      die("unexpected exit from the virtual CPU (reason %u)", ex->reason);
    }
    uint64_t ec2 = ex->exception.syndrome >> 26;
    if (ec2 != 0x16) die("the program reached Burrow in an unexpected way (EC 0x%llx at 0x%llx)",
                         (unsigned long long)ec2, (unsigned long long)ex->exception.virtual_address);
    uint64_t esr, elr, spsr, far;
    HV(hv_vcpu_get_sys_reg(c, HV_SYS_REG_ESR_EL1, &esr));
    HV(hv_vcpu_get_sys_reg(c, HV_SYS_REG_ELR_EL1, &elr));
    HV(hv_vcpu_get_sys_reg(c, HV_SYS_REG_SPSR_EL1, &spsr));
    uint64_t ec = esr >> 26;
    if (ec == 0x15) {                                                        // svc from the program
      uint64_t x[6], nr;
      for (int i = 0; i < 6; i++) HV(hv_vcpu_get_reg(c, HV_REG_X0 + i, &x[i]));
      HV(hv_vcpu_get_reg(c, HV_REG_X8, &nr));
      if (nr == 93 || nr == 94) exit((int)(x[0] & 0xff));                    // exit, exit_group
      int64_t r = syscall_(nr, x);
      if (logcalls) fprintf(stderr, "[burrow] %llu(0x%llx, 0x%llx, 0x%llx) = %lld\n", (unsigned long long)nr,
                            (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2], (long long)r);
      HV(hv_vcpu_set_reg(c, HV_REG_X0, (uint64_t)r));
      HV(hv_vcpu_set_reg(c, HV_REG_PC, elr));                                // (ELR already points after the svc)
      HV(hv_vcpu_set_reg(c, HV_REG_CPSR, spsr));
      continue;
    }
    HV(hv_vcpu_get_sys_reg(c, HV_SYS_REG_FAR_EL1, &far));
    fprintf(stderr, "burrow: the program crashed: %s at 0x%llx (address 0x%llx, ESR 0x%llx)\n", ec_name(ec),
            (unsigned long long)elr, (unsigned long long)far, (unsigned long long)esr);
    exit(139);
  }
}

int main(int argc, char **argv) {
  int a = 1;
  if (a + 1 < argc && !strcmp(argv[a], "-r")) { root = argv[a + 1]; a += 2; }
  if (a >= argc) { fprintf(stderr, "Usage: burrow [-r <root>] <program> [args…]\n"); return 2; }
  logcalls = getenv("BURROW_LOG") != NULL;
  const char *prog = argv[a];
  char hprog[4096];
  if (root && prog[0] == '/') snprintf(hprog, sizeof hprog, "%s%s", root, prog);
  else snprintf(hprog, sizeof hprog, "%s", prog);
  if (prog[0] == '/') snprintf(exe_path, sizeof exe_path, "%s", prog);
  else if (realpath(hprog, exe_path)) {
    if (root && !strncmp(exe_path, root, strlen(root))) memmove(exe_path, exe_path + strlen(root), strlen(exe_path + strlen(root)) + 1);
  } else snprintf(exe_path, sizeof exe_path, "%s", prog);

  mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mem == MAP_FAILED) die("can't reserve the program's memory: %s", strerror(errno));
  HV(hv_vm_create(NULL));
  HV(hv_vm_map(mem, 0, MEM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
  setup_el1();

  Loaded P, I;
  load_elf(hprog, PIE_BASE, &P);
  brk_start = brk_cur = (P.end + 0xffff) & ~0xffffull;
  uint64_t entry = P.entry;
  Loaded *interp = NULL;
  if (P.interp[0]) {
    char ip[4096];
    if (root) snprintf(ip, sizeof ip, "%s%s", root, P.interp);
    else snprintf(ip, sizeof ip, "%s", P.interp);
    load_elf(ip, INTERP_BASE, &I);
    interp = &I;
    entry = I.entry;
  }
  uint64_t sp = make_stack(argc - a, argv + a, &P, interp);

  hv_vcpu_t c;
  hv_vcpu_exit_t *ex;
  HV(hv_vcpu_create(&c, &ex, NULL));
  setup_vcpu(c, entry, sp);
  run(c, ex);
  return 0;
}
