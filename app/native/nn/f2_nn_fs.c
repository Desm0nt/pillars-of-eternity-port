/* F2: nn.fs -- save data (and the few other mounts) mapped to POSIX folders.
 *
 * Every save-data mount name ("save", whatever the game picks) is the SAME file system on the Switch (the user's
 * save data), so every mount resolves to f2_save_dir().  Layout inside it is the game's own, identical to what the
 * nx2android port keeps (e.g. "<guid><name>.savegame/saveinfo.xml", "Player.prefs/data"), so a save exported from
 * the working port can be dropped into this folder.
 * Paths: "<mount>:/<rel>"; '\\' is accepted as a separator; ".." components are rejected. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include "f2_nn.h"

#define MAX_MOUNTS 16
#define NN_PATH_MAX 0x301           /* nn::fs::EntryNameLengthMax + 1 */

enum { MOUNT_SAVE = 1, MOUNT_ROM = 2 };
typedef struct { char name[32]; int kind; int readonly; } mount_t;
static mount_t s_mounts[MAX_MOUNTS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { int fd; int mode; } nn_file_t;
typedef struct { char name[NN_PATH_MAX]; int8_t type; int64_t size; } dent_t;
typedef struct { dent_t *ents; int64_t count, pos; } nn_dir_t;

/* native nn::fs::DirectoryEntry as the generated marshalling code reads it (0x310 bytes) */
typedef struct {
    char name[NN_PATH_MAX];
    char pad0[3];
    int8_t type;                    /* +0x304: 0 = directory, 1 = file */
    char pad1[3];
    int64_t size;                   /* +0x308 */
} nn_dirent_t;
_Static_assert(sizeof(nn_dirent_t) == 0x310, "nn::fs::DirectoryEntry");
_Static_assert(offsetof(nn_dirent_t, type) == 0x304, "nn::fs::DirectoryEntry.type");

/* never NULL: a missing boot-layer path must not crash the game's save code */
static const char *save_root(void)
{
    const char *d = f2_save_dir();
    return d && *d ? d : "/f2-save-dir-not-set";
}

static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0770); *p = '/'; }
    if (mkdir(tmp, 0770) != 0 && errno != EEXIST) return -1;
    return 0;
}

void f2_nn_fs_init(void)
{
    const char *d = f2_save_dir();
    if (d && mkdir_p(d) != 0) f2_log("F2: nn fs: cannot create save dir %s: %s", d, strerror(errno));
    else f2_log("F2: nn fs: save data -> %s", d ? d : "(null)");
}

static mount_t *find_mount(const char *name, size_t n)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (s_mounts[i].kind && strlen(s_mounts[i].name) == n && !strncmp(s_mounts[i].name, name, n))
            return &s_mounts[i];
    return NULL;
}

static nn_result_t add_mount(const char *name, int kind, int readonly)
{
    if (!name || !*name || strlen(name) >= sizeof(s_mounts[0].name)) return NN_FS_INVALID_ARGUMENT;
    pthread_mutex_lock(&s_lock);
    nn_result_t r = NN_OK;
    if (find_mount(name, strlen(name))) r = NN_FS_MOUNT_NAME_EXISTS;
    else {
        r = NN_FS_INVALID_ARGUMENT;
        for (int i = 0; i < MAX_MOUNTS; i++)
            if (!s_mounts[i].kind) {
                strcpy(s_mounts[i].name, name); s_mounts[i].kind = kind; s_mounts[i].readonly = readonly;
                r = NN_OK; break;
            }
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}

/* "<mount>:/<rel>" -> host path.  0 = ok */
static int resolve(const char *path, char *out, size_t n)
{
    if (!path) return -1;
    const char *colon = strchr(path, ':');
    if (!colon) { f2_log("F2: nn fs: path without mount name: %s", path); return -1; }
    int kind = MOUNT_SAVE;
    pthread_mutex_lock(&s_lock);
    mount_t *m = find_mount(path, (size_t)(colon - path));
    if (m) kind = m->kind;
    pthread_mutex_unlock(&s_lock);
    if (!m) F2_ONCE("F2: nn fs: path on a mount name that was never mounted, using the save data folder: %s", path);
    const char *root = kind == MOUNT_ROM ? f2_data_dir() : save_root();
    if (!root) return -1;
    const char *rel = colon + 1;
    while (*rel == '/' || *rel == '\\') rel++;
    int len = snprintf(out, n, "%s/%s", root, rel);
    if (len < 0 || (size_t)len >= n) return -1;
    for (char *p = out + strlen(root); *p; p++) if (*p == '\\') *p = '/';
    /* no escaping from the mount */
    for (const char *p = out + strlen(root); (p = strstr(p, "/..")) != NULL; p += 3)
        if (p[3] == '/' || p[3] == 0) { f2_log("F2: nn fs: rejected path %s", path); return -1; }
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    return 0;
}

static nn_result_t from_errno(void)
{
    switch (errno) {
    case ENOENT: case ENOTDIR: return NN_FS_PATH_NOT_FOUND;
    case EEXIST: return NN_FS_PATH_ALREADY_EXISTS;
    case ENOTEMPTY: return NN_FS_DIRECTORY_NOT_EMPTY;
    case ENOSPC: case EDQUOT: return NN_FS_USABLE_SPACE_NOT_ENOUGH;
    case EBUSY: case ETXTBSY: return NN_FS_TARGET_LOCKED;
    default: return NN_FS_INVALID_ARGUMENT;
    }
}

static nn_result_t fail(const char *op, const char *path)
{
    nn_result_t r = from_errno();
    if (r != NN_FS_PATH_NOT_FOUND && r != NN_FS_PATH_ALREADY_EXISTS)
        f2_log("F2: nn fs.%s(%s) failed: %s -> 2%03u-%04u", op, path ? path : "?", strerror(errno), r & 0x1ff, (r >> 9) & 0x1fff);
    return r;
}

/* ---------------- mounts ---------------- */
nn_result_t nn_fs_EnsureSaveData(uint64_t d0, uint64_t d1)
{
    (void)d0; (void)d1;
    F2_ONCE("F2: nn fs.SaveData.Ensure -> %s", save_root());
    if (mkdir_p(save_root()) != 0) return fail("EnsureSaveData", save_root());
    return NN_OK;
}

nn_result_t nn_fs_MountSaveData(const char *name, uint64_t d0, uint64_t d1)
{
    (void)d0; (void)d1;
    nn_result_t r = add_mount(name, MOUNT_SAVE, 0);
    f2_log("F2: nn fs.SaveData.Mount(\"%s\") -> %s%s", name ? name : "?", save_root(), r ? "  [name already mounted]" : "");
    if (!r) mkdir_p(save_root());
    return r;
}

nn_result_t nn_fs_MountSaveDataReadOnly(const char *name, uint64_t app_id, uint64_t d0, uint64_t d1)
{
    (void)app_id; (void)d0; (void)d1;
    nn_result_t r = add_mount(name, MOUNT_SAVE, 1);
    f2_log("F2: nn fs.SaveData.MountSaveDataReadOnly(\"%s\") -> %s", name ? name : "?", save_root());
    return r;
}

/* both overloads (Uid) / (applicationId, Uid) */
uint32_t nn_fs_IsSaveDataExisting(void)
{
    struct stat st;
    int yes = stat(save_root(), &st) == 0 && S_ISDIR(st.st_mode);
    F2_ONCE("F2: nn fs.SaveData.IsExisting -> %d", yes);
    return (uint32_t)yes;
}

nn_result_t nn_fs_QueryMountRomCacheSize(uint64_t *out)
{
    F2_ONCE("F2: nn fs.Rom.QueryMountRomCacheSize");
    if (out) *out = 0x1000;
    return NN_OK;
}

nn_result_t nn_fs_MountRom(const char *name, void *cache, uint64_t size)
{
    (void)cache; (void)size;
    nn_result_t r = add_mount(name, MOUNT_ROM, 1);
    f2_log("F2: nn fs.Rom.MountRom(\"%s\") -> %s", name ? name : "?", f2_data_dir() ? f2_data_dir() : "(null)");
    return r;
}

nn_result_t nn_fs_QueryMountAddOnContentCacheSize(uint64_t *out, int32_t index)
{
    F2_ONCE("F2: nn fs.AddOnContent.QueryMountCacheSize(%d) -> not found", index);
    if (out) *out = 0;
    return NN_FS_TARGET_NOT_FOUND;
}

nn_result_t nn_fs_MountAddOnContent(const char *name, int32_t index, void *cache, uint64_t size)
{
    (void)cache; (void)size;
    f2_log("F2: nn fs.AddOnContent.Mount(\"%s\", %d) -> not found (no add-on content in this build)", name ? name : "?", index);
    return NN_FS_TARGET_NOT_FOUND;
}

void nn_fs_Unmount(const char *name)
{
    if (!name) return;
    pthread_mutex_lock(&s_lock);
    mount_t *m = find_mount(name, strlen(name));
    if (m) memset(m, 0, sizeof *m);
    pthread_mutex_unlock(&s_lock);
    f2_log("F2: nn fs.FileSystem.Unmount(\"%s\")%s", name, m ? "" : "  [was not mounted]");
}

nn_result_t nn_fs_Commit(const char *name)
{
    F2_ONCE("F2: nn fs.FileSystem.Commit(\"%s\") (files are written through; nothing to commit)", name ? name : "?");
    return NN_OK;
}

/* ---------------- entries ---------------- */
nn_result_t nn_fs_GetEntryType(int32_t *out, const char *path)
{
    char p[1024]; struct stat st;
    F2_ONCE("F2: nn fs.FileSystem.GetEntryType (first: %s)", path ? path : "?");
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    if (stat(p, &st) != 0) return fail("GetEntryType", path);
    if (out) *out = S_ISDIR(st.st_mode) ? 0 : 1;
    return NN_OK;
}

nn_result_t nn_fs_CreateFile(const char *path, int64_t size)
{
    char p[1024];
    F2_ONCE("F2: nn fs.File.Create (first: %s, %lld bytes)", path ? path : "?", (long long)size);
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
    if (fd < 0) return fail("CreateFile", path);
    nn_result_t r = NN_OK;
    if (size > 0 && ftruncate(fd, size) != 0) r = fail("CreateFile/size", path);
    close(fd);
    if (r) unlink(p);
    return r;
}

nn_result_t nn_fs_DeleteFile(const char *path)
{
    char p[1024];
    F2_ONCE("F2: nn fs.File.Delete (first: %s)", path ? path : "?");
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    return unlink(p) == 0 ? NN_OK : fail("DeleteFile", path);
}

nn_result_t nn_fs_CreateDirectory(const char *path)
{
    char p[1024];
    F2_ONCE("F2: nn fs.Directory.Create (first: %s)", path ? path : "?");
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    return mkdir(p, 0770) == 0 ? NN_OK : fail("CreateDirectory", path);
}

nn_result_t nn_fs_DeleteDirectory(const char *path)
{
    char p[1024];
    F2_ONCE("F2: nn fs.Directory.Delete (first: %s)", path ? path : "?");
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    return rmdir(p) == 0 ? NN_OK : fail("DeleteDirectory", path);
}

static int remove_tree(const char *p, int remove_self, int depth)
{
    if (depth > 32) { errno = ELOOP; return -1; }
    DIR *d = opendir(p);
    if (!d) return -1;
    struct dirent *e; int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1280]; struct stat st;
        if (snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= (int)sizeof c) { rc = -1; errno = ENAMETOOLONG; break; }
        if (lstat(c, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) { if (remove_tree(c, 1, depth + 1) != 0) { rc = -1; break; } }
        else if (unlink(c) != 0) { rc = -1; break; }
    }
    int e2 = errno;
    closedir(d);
    errno = e2;
    if (rc == 0 && remove_self) rc = rmdir(p);
    return rc;
}

nn_result_t nn_fs_DeleteDirectoryRecursively(const char *path)
{
    char p[1024];
    F2_ONCE("F2: nn fs.Directory.DeleteRecursively (first: %s)", path ? path : "?");
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    if (!strcmp(p, save_root())) { f2_log("F2: nn fs: refusing to delete the save root itself; cleaning it instead"); return remove_tree(p, 0, 0) == 0 ? NN_OK : fail("CleanDirectoryRecursively", path); }
    return remove_tree(p, 1, 0) == 0 ? NN_OK : fail("DeleteDirectoryRecursively", path);
}

nn_result_t nn_fs_CleanDirectoryRecursively(const char *path)
{
    char p[1024];
    F2_ONCE("F2: nn fs.Directory.CleanRecursively (first: %s)", path ? path : "?");
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    return remove_tree(p, 0, 0) == 0 ? NN_OK : fail("CleanDirectoryRecursively", path);
}

/* RenameFile / RenameDirectory: the destination must not exist */
nn_result_t nn_fs_Rename(const char *from, const char *to)
{
    char a[1024], b[1024]; struct stat st;
    F2_ONCE("F2: nn fs.File/Directory.Rename (first: %s -> %s)", from ? from : "?", to ? to : "?");
    if (resolve(from, a, sizeof a) || resolve(to, b, sizeof b)) return NN_FS_PATH_NOT_FOUND;
    if (lstat(b, &st) == 0) return NN_FS_PATH_ALREADY_EXISTS;
    return rename(a, b) == 0 ? NN_OK : fail("Rename", from);
}

/* ---------------- files ---------------- */
nn_result_t nn_fs_OpenFile(nn_file_t **out, const char *path, int32_t mode)
{
    char p[1024];
    F2_ONCE("F2: nn fs.File.Open (first: %s, mode %d)", path ? path : "?", mode);
    if (out) *out = NULL;
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    int fd = open(p, ((mode & 2) ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return fail("OpenFile", path);
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); return NN_FS_PATH_NOT_FOUND; }
    nn_file_t *f = (nn_file_t *)calloc(1, sizeof *f);
    f->fd = fd; f->mode = mode;
    if (out) *out = f;
    return NN_OK;
}

static nn_result_t read_at(nn_file_t *f, int64_t off, void *buf, uint64_t size, uint64_t *got_out)
{
    if (!f) return NN_FS_INVALID_ARGUMENT;
    uint64_t got = 0;
    while (got < size) {
        ssize_t r = pread(f->fd, (char *)buf + got, size - got, off + (int64_t)got);
        if (r < 0) { if (errno == EINTR) continue; return fail("ReadFile", NULL); }
        if (r == 0) break;
        got += (uint64_t)r;
    }
    if (got_out) { *got_out = got; return NN_OK; }
    if (got != size) { f2_log("F2: nn fs.File.Read: short read %llu of %llu at %lld", (unsigned long long)got, (unsigned long long)size, (long long)off); return NN_FS_OUT_OF_RANGE; }
    return NN_OK;
}

/* (handle, offset, buffer, size[, option]) -- must read exactly size */
nn_result_t nn_fs_ReadFile(nn_file_t *f, int64_t off, void *buf, uint64_t size)
{
    F2_ONCE("F2: nn fs.File.Read");
    return read_at(f, off, buf, size, NULL);
}

/* (outSize, handle, offset, buffer, size[, option]) -- reads what is there */
nn_result_t nn_fs_ReadFileOut(uint64_t *out, nn_file_t *f, int64_t off, void *buf, uint64_t size)
{
    F2_ONCE("F2: nn fs.File.Read (with out size)");
    uint64_t got = 0;
    nn_result_t r = read_at(f, off, buf, size, &got);
    if (out) *out = got;
    return r;
}

nn_result_t nn_fs_WriteFile(nn_file_t *f, int64_t off, const void *buf, uint64_t size, int32_t option)
{
    F2_ONCE("F2: nn fs.File.Write");
    if (!f) return NN_FS_INVALID_ARGUMENT;
    uint64_t put = 0;
    while (put < size) {
        ssize_t r = pwrite(f->fd, (const char *)buf + put, size - put, off + (int64_t)put);
        if (r < 0) { if (errno == EINTR) continue; return fail("WriteFile", NULL); }
        put += (uint64_t)r;
    }
    if (option & 1) fdatasync(f->fd);
    return NN_OK;
}

nn_result_t nn_fs_FlushFile(nn_file_t *f)
{
    F2_ONCE("F2: nn fs.File.Flush");
    if (!f) return NN_FS_INVALID_ARGUMENT;
    fdatasync(f->fd);
    return NN_OK;
}

nn_result_t nn_fs_SetFileSize(nn_file_t *f, int64_t size)
{
    F2_ONCE("F2: nn fs.File.SetSize");
    if (!f) return NN_FS_INVALID_ARGUMENT;
    return ftruncate(f->fd, size) == 0 ? NN_OK : fail("SetFileSize", NULL);
}

nn_result_t nn_fs_GetFileSize(int64_t *out, nn_file_t *f)
{
    struct stat st;
    F2_ONCE("F2: nn fs.File.GetSize");
    if (!f) return NN_FS_INVALID_ARGUMENT;
    if (fstat(f->fd, &st) != 0) return fail("GetFileSize", NULL);
    if (out) *out = st.st_size;
    return NN_OK;
}

int32_t nn_fs_GetFileOpenMode(nn_file_t *f) { F2_ONCE("F2: nn fs.File.GetOpenMode"); return f ? f->mode : 0; }

void nn_fs_CloseFile(nn_file_t *f)
{
    F2_ONCE("F2: nn fs.File.Close");
    if (!f) return;
    close(f->fd);
    free(f);
}

/* ---------------- directories ---------------- */
static int dent_cmp(const void *a, const void *b) { return strcmp(((const dent_t *)a)->name, ((const dent_t *)b)->name); }

nn_result_t nn_fs_OpenDirectory(nn_dir_t **out, const char *path, int32_t mode)
{
    char p[1024];
    F2_ONCE("F2: nn fs.Directory.Open (first: %s, mode %d)", path ? path : "?", mode);
    if (out) *out = NULL;
    if (resolve(path, p, sizeof p)) return NN_FS_PATH_NOT_FOUND;
    DIR *d = opendir(p);
    if (!d) return fail("OpenDirectory", path);
    nn_dir_t *h = (nn_dir_t *)calloc(1, sizeof *h);
    int64_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1280]; struct stat st;
        if (snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= (int)sizeof c || stat(c, &st) != 0) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!(mode & (isdir ? 1 : 2))) continue;
        if (strlen(e->d_name) >= NN_PATH_MAX) continue;
        if (h->count == cap) { cap = cap ? cap * 2 : 32; h->ents = (dent_t *)realloc(h->ents, (size_t)cap * sizeof(dent_t)); }
        dent_t *t = &h->ents[h->count++];
        memset(t, 0, sizeof *t);
        strcpy(t->name, e->d_name);
        t->type = isdir ? 0 : 1;
        t->size = isdir ? 0 : st.st_size;
    }
    closedir(d);
    if (h->count > 1) qsort(h->ents, (size_t)h->count, sizeof(dent_t), dent_cmp);
    if (out) *out = h;
    return NN_OK;
}

nn_result_t nn_fs_ReadDirectory(int64_t *out_count, nn_dirent_t *buf, nn_dir_t *h, int64_t count)
{
    F2_ONCE("F2: nn fs.Directory.Read");
    int64_t n = 0;
    if (!h) return NN_FS_INVALID_ARGUMENT;
    while (buf && n < count && h->pos < h->count) {
        const dent_t *t = &h->ents[h->pos++];
        memset(&buf[n], 0, sizeof buf[n]);
        strcpy(buf[n].name, t->name);
        buf[n].type = t->type;
        buf[n].size = t->size;
        n++;
    }
    if (out_count) *out_count = n;
    return NN_OK;
}

nn_result_t nn_fs_GetDirectoryEntryCount(int64_t *out, nn_dir_t *h)
{
    F2_ONCE("F2: nn fs.Directory.GetEntryCount");
    if (!h) return NN_FS_INVALID_ARGUMENT;
    if (out) *out = h->count;
    return NN_OK;
}

void nn_fs_CloseDirectory(nn_dir_t *h)
{
    F2_ONCE("F2: nn fs.Directory.Close");
    if (!h) return;
    free(h->ents);
    free(h);
}
