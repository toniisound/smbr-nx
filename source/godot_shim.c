/* godot_shim.c -- bionic/NDK shims added for libgodot_android.so (Godot 4.6):
 * bionic dirent conversion, rwlocks, AAssetManager over the assets/ dir,
 * and assorted linux-isms newlib lacks. Camera/media NDK stubs live in
 * imports.c as plain ret0/retm1 entries. MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "godot_shim.h"
#include "libc_shim.h"
#include "util.h"

// ---------------------------------------------------------------------------
// bionic dirent: { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[256]; }
// newlib's struct dirent differs, so wrap the directory stream and convert.
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};

#define BIONIC_DT_UNKNOWN 0
#define BIONIC_DT_DIR     4
#define BIONIC_DT_REG     8

typedef struct {
  uint32_t magic; // 'BDIR'
  DIR *dir;
  char path[512];
  struct bionic_dirent ent;
} FakeDir;

#define FAKEDIR_MAGIC 0x42444952

void *opendir_fake(const char *path) {
  if (!path) return NULL;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  DIR *d = opendir(path);
  if (!d) return NULL;
  FakeDir *fd = calloc(1, sizeof(*fd));
  if (!fd) { closedir(d); return NULL; }
  fd->magic = FAKEDIR_MAGIC;
  fd->dir = d;
  strncpy(fd->path, path, sizeof(fd->path) - 1);
  return fd;
}

void *fdopendir_fake(int fd) { (void)fd; return NULL; }

void *readdir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd || fd->magic != FAKEDIR_MAGIC) return NULL;
  struct dirent *e = readdir(fd->dir);
  if (!e) return NULL;
  memset(&fd->ent, 0, sizeof(fd->ent));
  fd->ent.d_ino = 1;
  fd->ent.d_reclen = sizeof(fd->ent);
  strncpy(fd->ent.d_name, e->d_name, sizeof(fd->ent.d_name) - 1);
  // newlib on Switch has no d_type; stat to tell dirs from files (Godot's
  // DirAccessUnix falls back to stat when DT_UNKNOWN, but be explicit).
  char full[768];
  snprintf(full, sizeof(full), "%s/%s", fd->path, e->d_name);
  struct stat st;
  if (stat(full, &st) == 0)
    fd->ent.d_type = S_ISDIR(st.st_mode) ? BIONIC_DT_DIR : BIONIC_DT_REG;
  else
    fd->ent.d_type = BIONIC_DT_UNKNOWN;
  return &fd->ent;
}

int closedir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd || fd->magic != FAKEDIR_MAGIC) return -1;
  closedir(fd->dir);
  fd->magic = 0;
  free(fd);
  return 0;
}

// ---------------------------------------------------------------------------
// pthread rwlock via libnx RwLock, pointer-indirected like the mutex fakes
// (bionic zero-initializes the storage inline; first use allocates).
// ---------------------------------------------------------------------------

typedef struct { RwLock l; } FakeRwLock;

static FakeRwLock *ensure_rwlock(void **lk) {
  if (!*lk) {
    FakeRwLock *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    rwlockInit(&r->l);
    // benign race at worst leaks one small object; engine inits these early
    *lk = r;
  }
  return (FakeRwLock *)*lk;
}

int pthread_rwlock_rdlock_fake(void **lk) {
  FakeRwLock *r = ensure_rwlock(lk);
  if (!r) return -1;
  rwlockReadLock(&r->l);
  return 0;
}
int pthread_rwlock_wrlock_fake(void **lk) {
  FakeRwLock *r = ensure_rwlock(lk);
  if (!r) return -1;
  rwlockWriteLock(&r->l);
  return 0;
}
int pthread_rwlock_unlock_fake(void **lk) {
  FakeRwLock *r = (FakeRwLock *)*lk;
  if (!r) return -1;
  // libnx needs the matching unlock; the write path holds the writer lock
  if (rwlockIsWriteLockHeldByCurrentThread(&r->l))
    rwlockWriteUnlock(&r->l);
  else
    rwlockReadUnlock(&r->l);
  return 0;
}

// ---------------------------------------------------------------------------
// misc bionic/linux
// ---------------------------------------------------------------------------

int gettid_fake2(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

int pthread_gettid_np_fake(void *thread) { (void)thread; return gettid_fake2(); }

unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  if (value) value[0] = 0;
  return 0;
}

struct bionic_rlimit { uint64_t rlim_cur, rlim_max; };
int getrlimit_fake(int res, void *rlim) {
  (void)res;
  struct bionic_rlimit *r = rlim;
  if (r) { r->rlim_cur = r->rlim_max = 8ull * 1024 * 1024; } // plausible stack cap
  return 0;
}

// no symlinks on fatfs: canonicalization is a plain copy
char *realpath_fake(const char *path, char *resolved) {
  if (!path) { errno = EINVAL; return NULL; }
  char *out = resolved ? resolved : malloc(4096);
  if (!out) return NULL;
  strncpy(out, path, 4095);
  out[4095] = 0;
  return out;
}

int mkstemp_fake(char *tmpl) {
  if (!tmpl) { errno = EINVAL; return -1; }
  size_t l = strlen(tmpl);
  if (l < 6) { errno = EINVAL; return -1; }
  static int counter = 0;
  for (int tries = 0; tries < 100; tries++) {
    snprintf(tmpl + l - 6, 7, "%06d", (counter++) % 1000000);
    int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) return fd;
  }
  errno = EEXIST;
  return -1;
}

// sandboxed variants of the direct filesystem mutators
int mkdir_fake(const char *path, int mode) {
  char sb[640];
  return mkdir(sandbox_path(path, sb, sizeof(sb)), (mode_t)mode);
}
int unlink_fake(const char *path) {
  char sb[640];
  return unlink(sandbox_path(path, sb, sizeof(sb)));
}
int rmdir_fake(const char *path) {
  char sb[640];
  return rmdir(sandbox_path(path, sb, sizeof(sb)));
}
int rename_fake(const char *from, const char *to) {
  char s1[640], s2[640];
  return rename(sandbox_path(from, s1, sizeof(s1)), sandbox_path(to, s2, sizeof(s2)));
}
int remove_fake(const char *path) {
  char sb[640];
  return remove(sandbox_path(path, sb, sizeof(sb)));
}

#define BIONIC_AT_FDCWD (-100)

int openat_fake(int dirfd, const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  return open(path, flags, mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) {
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  if (flags) return rmdir_fake(path);
  return unlink_fake(path);
}
int fchmodat_fake(int dirfd, const char *path, int mode, int flags) {
  (void)dirfd; (void)path; (void)mode; (void)flags; return 0;
}
int utimensat_fake(int dirfd, const char *path, const void *times, int flags) {
  (void)dirfd; (void)path; (void)times; (void)flags; return 0;
}

long pathconf_fake(const char *path, int name) { (void)path; (void)name; return 4096; }

int sched_getaffinity_fake(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (mask && setsize >= 1) { memset(mask, 0, setsize); ((uint8_t *)mask)[0] = 0x7; } // 3 cores
  return 0;
}
int sched_setaffinity_fake(int pid, size_t setsize, const void *mask) {
  (void)pid; (void)setsize; (void)mask; return 0;
}

// thread_local destructors: threads live for the process lifetime here, so
// registering the destructors is safely skippable (leaks only at thread exit).
int __cxa_thread_atexit_impl_fake(void (*dtor)(void *), void *obj, void *dso) {
  (void)dtor; (void)obj; (void)dso; return 0;
}

void __FD_SET_chk_fake(int fd, void *set, size_t setsize) {
  if (set && fd >= 0 && (size_t)(fd / 8) < setsize)
    ((uint8_t *)set)[fd / 8] |= 1u << (fd % 8);
}

struct bionic_statvfs {
  uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree, f_favail;
  uint64_t f_fsid;
  uint64_t f_flag, f_namemax;
  uint64_t __spare[6];
};
int statvfs_fake(const char *path, void *buf) {
  (void)path;
  struct bionic_statvfs *s = buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize = s->f_frsize = 0x1000;
  s->f_blocks = (4ull * 1024 * 1024 * 1024) / 0x1000;
  s->f_bfree = s->f_bavail = (2ull * 1024 * 1024 * 1024) / 0x1000;
  s->f_namemax = 255;
  return 0;
}

int truncate_fake(const char *path, int64_t len) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  int rc = ftruncate(fd, (off_t)len);
  close(fd);
  return rc;
}

int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
#if DEBUG_LOG
  char buf[0x800];
  vsnprintf(buf, sizeof(buf), fmt, va);
  if (prio >= 6) // ANDROID_LOG_ERROR / FATAL: write through
    debugPrintf("[%s] %s\n", tag ? tag : "", buf);
  else
    debugPrintfLazy("[%s] %s\n", tag ? tag : "", buf);
#else
  (void)prio; (void)tag; (void)fmt; (void)va;
#endif
  return 0;
}

int android_log_write_fake(int prio, const char *tag, const char *msg) {
#if DEBUG_LOG
  if (prio >= 6)
    debugPrintf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
  else
    debugPrintfLazy("[%s] %s\n", tag ? tag : "", msg ? msg : "");
#else
  (void)prio; (void)tag; (void)msg;
#endif
  return 0;
}

void perror_fake(const char *s) {
#if DEBUG_LOG
  debugPrintf("perror: %s: %s\n", s ? s : "", strerror(errno));
#else
  (void)s;
#endif
}

int isatty_fake(int fd) { (void)fd; return 0; }

// On real Android the app process cwd is "/", and Godot's ProjectSettings
// uses the cwd during project discovery to derive resource_path: any real
// directory reported here leaks into every res:// path the engine builds
// (the empty-character-select bug). Mimic Android: cwd is always "/", and the
// sandbox_path() rebase in libc_shim keeps stray absolute writes ("/saves")
// inside the app's save dir instead of the SD root.
//
// The engine's own chdir() calls are tracked in that same Android view:
// OS_Android::get_user_data_dir() resolves the data dir with
// chdir(dir) + getcwd() (_remove_symlink), and DirAccessUnix::change_dir()
// does chdir(try_dir) ... chdir(prev_dir). With a constant "/" the user data
// dir became "/" and DirAccess on user:// failed (no shader cache). Until the
// engine chdir()s anywhere this still reports "/" for project discovery.
static char s_gd_cwd[1024] = "/";
static Mutex s_gd_cwd_lock;

// collapse "//", "/./" and "/x/.." in an absolute path, in place
static void normalize_abs_path(char *p) {
  char out[1024];
  size_t o = 0;
  const char *s = p;
  out[o++] = '/';
  while (*s) {
    while (*s == '/') s++;
    if (!*s) break;
    const char *e = s;
    while (*e && *e != '/') e++;
    const size_t n = (size_t)(e - s);
    if (n == 1 && s[0] == '.') {
      // skip
    } else if (n == 2 && s[0] == '.' && s[1] == '.') {
      if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }
    } else {
      if (o > 1 && out[o - 1] != '/') out[o++] = '/';
      if (o + n >= sizeof(out) - 1) break;
      memcpy(out + o, s, n);
      o += n;
    }
    s = e;
  }
  if (o > 1 && out[o - 1] == '/') o--;
  out[o] = 0;
  strcpy(p, out);
}

int chdir_fake(const char *path) {
  if (!path || !path[0]) { errno = ENOENT; return -1; }
  char abs[1024];
  mutexLock(&s_gd_cwd_lock);
  if (path[0] == '/')
    snprintf(abs, sizeof(abs), "%s", path);
  else
    snprintf(abs, sizeof(abs), "%s/%s", s_gd_cwd, path);
  mutexUnlock(&s_gd_cwd_lock);
  normalize_abs_path(abs);
  char sb[1100];
  const char *real = sandbox_path(abs, sb, sizeof(sb));
  if (chdir(real) != 0) return -1;
  mutexLock(&s_gd_cwd_lock);
  snprintf(s_gd_cwd, sizeof(s_gd_cwd), "%s", abs);
  mutexUnlock(&s_gd_cwd_lock);
  return 0;
}

char *getcwd_fake(char *buf, size_t size) {
  mutexLock(&s_gd_cwd_lock);
  const size_t l = strlen(s_gd_cwd);
  char *ret = NULL;
  if (!buf) {
    ret = strdup(s_gd_cwd);
  } else if (size < l + 1) {
    errno = ERANGE;
  } else {
    memcpy(buf, s_gd_cwd, l + 1);
    ret = buf;
  }
  mutexUnlock(&s_gd_cwd_lock);
  return ret;
}

// ---------------------------------------------------------------------------
// locale _l variants: single-locale system, forward to the C versions
// ---------------------------------------------------------------------------

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcmp(a, b); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc; return strftime(s, max, fmt, (const struct tm *)tm);
}
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscmp(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }
int towlower_l_fake(int c, void *loc) { (void)loc; return towlower(c); }
int towupper_l_fake(int c, void *loc) { (void)loc; return towupper(c); }
int isdigit_l_fake(int c, void *loc) { (void)loc; return isdigit(c); }
int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
int islower_l_fake(int c, void *loc) { (void)loc; return islower(c); }
int isupper_l_fake(int c, void *loc) { (void)loc; return isupper(c); }
int tolower_l_fake(int c, void *loc) { (void)loc; return tolower(c); }
int toupper_l_fake(int c, void *loc) { (void)loc; return toupper(c); }
long double log10l_fake(long double x) { return (long double)log10((double)x); }

int iswalpha_l_fake(int c, void *loc) { (void)loc; return iswalpha(c); }
int iswblank_l_fake(int c, void *loc) { (void)loc; return iswblank(c); }
int iswcntrl_l_fake(int c, void *loc) { (void)loc; return iswcntrl(c); }
int iswdigit_l_fake(int c, void *loc) { (void)loc; return iswdigit(c); }
int iswlower_l_fake(int c, void *loc) { (void)loc; return iswlower(c); }
int iswprint_l_fake(int c, void *loc) { (void)loc; return iswprint(c); }
int iswpunct_l_fake(int c, void *loc) { (void)loc; return iswpunct(c); }
int iswspace_l_fake(int c, void *loc) { (void)loc; return iswspace(c); }
int iswupper_l_fake(int c, void *loc) { (void)loc; return iswupper(c); }
int iswxdigit_l_fake(int c, void *loc) { (void)loc; return iswxdigit(c); }

// ---------------------------------------------------------------------------
// AAssetManager over <data_root>/assets/: FileAccessAndroid opens every res://
// file through this. Paths arrive relative ("project.binary", "Instances/...").
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t magic; // 'ASET'
  FILE *f;
  int64_t len;
  int pooled; // f belongs to the game.pck handle pool
} FakeAsset;

// The engine opens res://assets.sparsepck (= game.pck) once per resource it
// loads: thousands of times per session. Opening a 56 MB file by path on the
// SD (plus fseek/ftell for its size) every time dominated load times, so idle
// game.pck handles are kept open and reused.
#define PCK_POOL_MAX 8
static FILE *s_pck_pool[PCK_POOL_MAX];
static int s_pck_pool_n;
static int64_t s_pck_len = -1;
static Mutex s_pck_pool_lock;
static int s_pck_logged;

static FILE *pck_acquire(const char *path, int64_t *len) {
  FILE *f = NULL;
  mutexLock(&s_pck_pool_lock);
  if (s_pck_pool_n > 0) f = s_pck_pool[--s_pck_pool_n];
  mutexUnlock(&s_pck_pool_lock);
  if (f) {
    fseek(f, 0, SEEK_SET);
    *len = s_pck_len;
    return f;
  }
  f = fopen(path, "rb");
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  if (s_pck_len < 0) {
    fseek(f, 0, SEEK_END);
    s_pck_len = ftell(f);
    fseek(f, 0, SEEK_SET);
  }
  *len = s_pck_len;
  return f;
}

static void pck_release(FILE *f) {
  mutexLock(&s_pck_pool_lock);
  if (s_pck_pool_n < PCK_POOL_MAX) { s_pck_pool[s_pck_pool_n++] = f; f = NULL; }
  mutexUnlock(&s_pck_pool_lock);
  if (f) fclose(f);
}

#define FAKEASSET_MAGIC 0x41534554

static void *g_fake_assetmgr = (void *)0xA55E7;

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  return g_fake_assetmgr;
}

// res://-relative paths that must always look empty to the engine, regardless
// of whatever the APK's own assets/ actually contains. Currently just
// extension_list.cfg: exported Godot Android projects that ship a GDExtension
// (e.g. SMB1R's res://godotgif/godotgif.gdextension) list it here, and the
// wrapper has no GDExtension-loading support -- letting the engine try to load
// it crashes early in Main::setup2() instead of failing gracefully. Blanking
// this file makes the engine see zero declared extensions, the same as any
// project that doesn't use GDExtensions at all. Faked here (not by editing the
// extracted assets/ tree) so re-extracting a fresh APK doesn't reintroduce it.
static const char *s_blanked_assets[] = {
  ".godot/extension_list.cfg",
};

static int is_blanked_asset(const char *filename) {
  for (unsigned i = 0; i < sizeof(s_blanked_assets) / sizeof(*s_blanked_assets); i++)
    if (!strcmp(filename, s_blanked_assets[i])) return 1;
  return 0;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;
  while (*filename == '/') filename++;

  FakeAsset *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->magic = FAKEASSET_MAGIC;

  if (is_blanked_asset(filename)) {
    debugPrintf("[assets] %s -> blanked (forced empty)\n", filename);
    a->f = NULL;
    a->len = 0;
    return a;
  }

  char path[768];
  if (!strcmp(filename, "assets.sparsepck")) {
    // Godot's only main-pack hook on Android release builds:
    // ProjectSettings::_setup loads res://assets.sparsepck through this asset
    // manager before falling back to loose assets/ files (the exec-path .pck
    // lookup is skipped there because _execpath is still empty at that point,
    // and --main-pack is compiled out). A regular, non-sparse PCK loads fine
    // through it, so serve the payload's game.pck.
    snprintf(path, sizeof(path), "%s/game.pck", config.data_root);
    if (!s_pck_logged) { s_pck_logged = 1; debugPrintf("[assets] assets.sparsepck -> %s\n", path); }
    int64_t len = 0;
    FILE *pf = pck_acquire(path, &len);
    if (!pf) { free(a); return NULL; }
    a->f = pf;
    a->len = len;
    a->pooled = 1;
    return a;
  } else {
    snprintf(path, sizeof(path), "%s/assets/%s", config.data_root, filename);
  }
  FILE *f = fopen(path, "rb");
#if VERBOSE_IO
  debugPrintf("AAssetManager_open(\"%s\") -> %p\n", filename, (void *)f);
#endif
  if (!f) { free(a); return NULL; }
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->len = ftell(f);
  fseek(f, 0, SEEK_SET);
  return a;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  FakeAsset *a = asset;
  if (!a || a->magic != FAKEASSET_MAGIC || !buf) return -1;
  if (!a->f) return 0; // blanked (forced-empty) asset: always EOF
  return (int)fread(buf, 1, count, a->f);
}

int64_t AAsset_seek_fake(void *asset, int64_t offset, int whence) {
  FakeAsset *a = asset;
  if (!a || a->magic != FAKEASSET_MAGIC) return -1;
  if (!a->f) return 0; // blanked (forced-empty) asset: only position 0 exists
  if (fseek(a->f, (long)offset, whence) != 0) return -1;
  return ftell(a->f);
}

int64_t AAsset_getLength_fake(void *asset) {
  FakeAsset *a = asset;
  return (a && a->magic == FAKEASSET_MAGIC) ? a->len : 0;
}
int64_t AAsset_getLength64_fake(void *asset) { return AAsset_getLength_fake(asset); }

void AAsset_close_fake(void *asset) {
  FakeAsset *a = asset;
  if (a && a->magic == FAKEASSET_MAGIC) {
    if (a->f) {
      if (a->pooled) pck_release(a->f);
      else fclose(a->f);
    }
    a->magic = 0;
    free(a);
  }
}

// ---------------------------------------------------------------------------
// data symbols
// ---------------------------------------------------------------------------

unsigned char in6addr_any_fake[16];

extern uint8_t fake_sF[3][0x100];
FILE *stdin_fake  = (FILE *)fake_sF[0];
FILE *stdout_fake = (FILE *)fake_sF[1];
FILE *stderr_fake = (FILE *)fake_sF[2];
