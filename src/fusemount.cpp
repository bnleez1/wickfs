#define FUSE_USE_VERSION 312
#include <fuse3/fuse.h>

#include "fusemount.h"
#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QProcess>
#include <QStandardPaths>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

bool isBackgroundIndexerOrThumbnailer(pid_t pid) {
    if (pid <= 1)
        return false;

    static std::mutex s_cacheMutex;
    static QHash<pid_t, QPair<bool, qint64>> s_pidCache;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        auto it = s_pidCache.constFind(pid);
        if (it != s_pidCache.constEnd() && it->second > nowMs)
            return it->first;
    }

    bool matched = false;
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    if (comm.open(QIODevice::ReadOnly)) {
        const QString name = QString::fromUtf8(comm.readAll()).trimmed().toLower();
        static const QStringList kSkipPrefixes = {
            QStringLiteral("glycin"),
            QStringLiteral("bwrap"),
            QStringLiteral("tumblerd"),
            QStringLiteral("tracker"),
            QStringLiteral("localsearch"),
            QStringLiteral("gdk-pixbuf"),
            QStringLiteral("totem-video"),
            QStringLiteral("evince-thumb"),
            QStringLiteral("ffmpegthumb"),
            QStringLiteral("gsf-office")
        };
        for (const QString &pfx : kSkipPrefixes) {
            if (name.startsWith(pfx)) {
                matched = true;
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (s_pidCache.size() > 256)
            s_pidCache.clear();
        s_pidCache.insert(pid, qMakePair(matched, nowMs + 10000LL));
    }
    return matched;
}

QString normalizeRelPath(const char *rawPath) {
    if (!rawPath)
        return {};
    QString p = QString::fromUtf8(rawPath);
    while (p.startsWith('/'))
        p.remove(0, 1);
    while (p.endsWith('/'))
        p.chop(1);
    return p;
}

FuseMount *currentMount() {
    struct fuse_context *ctx = fuse_get_context();
    return ctx ? static_cast<FuseMount *>(ctx->private_data) : nullptr;
}

void fillStatFromFields(struct stat *stbuf, uid_t uid, gid_t gid,
                         qint64 sizeBytes, qint64 mtimeSecs, bool isDir, bool isResident) {
    std::memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_uid = uid;
    stbuf->st_gid = gid;
    stbuf->st_atim.tv_sec = mtimeSecs;
    stbuf->st_mtim.tv_sec = mtimeSecs;
    stbuf->st_ctim.tv_sec = mtimeSecs;

    if (isDir) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_size = 4096;
        stbuf->st_blocks = 8;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = static_cast<off_t>(sizeBytes);
        // Cloud-only stubs report 0 allocated disk blocks until hydrated.
        stbuf->st_blocks = isResident ? static_cast<blkcnt_t>((sizeBytes + 511) / 512) : 0;
    }
}

int fuseGetattrCb(const char *path, struct stat *stbuf, struct fuse_file_info *) {
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleGetattr(path, stbuf);
}

int fuseReaddirCb(const char *path, void *buf, fuse_fill_dir_t filler, off_t,
                  struct fuse_file_info *, enum fuse_readdir_flags flags) {
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    int err = 0;
    const QList<VfsDirEntry> entries = m->handleReaddirEntries(path, &err);
    if (err != 0)
        return err;

    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_DEFAULTS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_DEFAULTS);
    const bool plus = (flags & FUSE_READDIR_PLUS) != 0;
    const enum fuse_fill_dir_flags fillFlag = plus ? FUSE_FILL_DIR_PLUS : FUSE_FILL_DIR_DEFAULTS;
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();
    for (const VfsDirEntry &entry : entries) {
        struct stat st {};
        fillStatFromFields(&st, uid, gid, entry.sizeBytes, entry.mtimeSecs, entry.isDir, entry.isResident);
        const QByteArray utf8 = entry.name.toUtf8();
        filler(buf, utf8.constData(), &st, 0, fillFlag);
    }
    return 0;
}

int fuseOpenCb(const char *path, struct fuse_file_info *fi) {
    FuseMount *m = currentMount();
    if (!m || !m->backend())
        return -EIO;

    const QString rel = normalizeRelPath(path);
    const bool forWrite = ((fi->flags & O_ACCMODE) != O_RDONLY);

    // Do not block Nautilus/file-manager folder browsing by downloading remote
    // cloud-only blobs when a background thumbnailer or desktop indexer opens them.
    if (!forWrite) {
        struct fuse_context *ctx = fuse_get_context();
        if (ctx && isBackgroundIndexerOrThumbnailer(ctx->pid) && m->backend()->vfsIsCloudOnly(rel))
            return -ENODATA;
    }

    QString err;
    const QString localPath = m->backend()->vfsOpenAndHydrate(rel, forWrite, &err);
    if (localPath.isEmpty())
        return -ENOENT;

    int flags = fi->flags & ~(O_CREAT | O_EXCL);
    int fd = ::open(localPath.toUtf8().constData(), flags, 0644);
    if (fd < 0)
        return -errno;
    fi->fh = static_cast<uint64_t>(fd);
    return 0;
}

int fuseCreateCb(const char *path, mode_t mode, struct fuse_file_info *fi) {
    FuseMount *m = currentMount();
    if (!m || !m->backend())
        return -EIO;

    const QString rel = normalizeRelPath(path);
    QString localPath;
    if (!m->backend()->vfsCreateFile(rel, &localPath))
        return -EIO;

    int fd = ::open(localPath.toUtf8().constData(), O_CREAT | O_TRUNC | O_RDWR, mode ? mode : 0644);
    if (fd < 0)
        return -errno;
    fi->fh = static_cast<uint64_t>(fd);
    return 0;
}

int fuseReadCb(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    if (fi && fi->fh > 0) {
        ssize_t res = ::pread(static_cast<int>(fi->fh), buf, size, offset);
        if (res < 0)
            return -errno;
        return static_cast<int>(res);
    }
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleOpenAndRead(path, buf, size, offset);
}

int fuseWriteCb(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    if (fi && fi->fh > 0) {
        ssize_t res = ::pwrite(static_cast<int>(fi->fh), buf, size, offset);
        if (res < 0)
            return -errno;
        return static_cast<int>(res);
    }
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleWriteFile(path, buf, size, offset);
}

int fuseTruncateCb(const char *path, off_t size, struct fuse_file_info *fi) {
    if (fi && fi->fh > 0) {
        if (::ftruncate(static_cast<int>(fi->fh), size) < 0)
            return -errno;
        return 0;
    }
    FuseMount *m = currentMount();
    if (!m || !m->backend())
        return -EIO;
    const QString rel = normalizeRelPath(path);
    const QString localPath = m->backend()->vfsOpenAndHydrate(rel, true);
    if (localPath.isEmpty())
        return -ENOENT;
    if (::truncate(localPath.toUtf8().constData(), size) < 0)
        return -errno;
    m->backend()->vfsWriteCompleted(rel);
    return 0;
}

int fuseReleaseCb(const char *path, struct fuse_file_info *fi) {
    if (fi && fi->fh > 0) {
        ::close(static_cast<int>(fi->fh));
        fi->fh = 0;
    }
    FuseMount *m = currentMount();
    if (m && m->backend() && fi && ((fi->flags & O_ACCMODE) != O_RDONLY)) {
        m->backend()->vfsWriteCompleted(normalizeRelPath(path));
    }
    return 0;
}

int fuseUnlinkCb(const char *path) {
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleUnlink(path);
}

int fuseMkdirCb(const char *path, mode_t mode) {
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleMkdir(path, mode);
}

int fuseRmdirCb(const char *path) {
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleRmdir(path);
}

int fuseRenameCb(const char *oldPath, const char *newPath, unsigned int flags) {
    FuseMount *m = currentMount();
    if (!m)
        return -EIO;
    return m->handleRename(oldPath, newPath, flags);
}

int checkPathExists(FuseMount *m, const char *path) {
    if (!m || !m->backend())
        return -EIO;
    const QString rel = normalizeRelPath(path);
    qint64 sz = 0, mtime = 0;
    bool isDir = false, isRes = false;
    return m->backend()->vfsStat(rel, &sz, &mtime, &isDir, &isRes) ? 0 : -ENOENT;
}

int fuseUtimensCb(const char *path, const struct timespec tv[2], struct fuse_file_info *) {
    Q_UNUSED(tv);
    return checkPathExists(currentMount(), path);
}

int fuseChmodCb(const char *path, mode_t mode, struct fuse_file_info *) {
    Q_UNUSED(mode);
    return checkPathExists(currentMount(), path);
}

int fuseChownCb(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *) {
    Q_UNUSED(uid);
    Q_UNUSED(gid);
    return checkPathExists(currentMount(), path);
}

int fuseStatfsCb(const char *, struct statvfs *stbuf) {
    if (!stbuf)
        return -EIO;
    std::memset(stbuf, 0, sizeof(struct statvfs));
    FuseMount *m = currentMount();
    const QString target = (m && !m->mountPoint().isEmpty()) ? QFileInfo(m->mountPoint()).absolutePath() : QDir::homePath();
    if (::statvfs(target.toUtf8().constData(), stbuf) != 0)
        return -errno;
    return 0;
}

void *fuseInitCb(struct fuse_conn_info *, struct fuse_config *cfg) {
    if (cfg) {
        cfg->entry_timeout = 60.0;
        cfg->attr_timeout = 60.0;
        cfg->negative_timeout = 15.0;
        cfg->auto_cache = 1;
        cfg->no_rofd_flush = 1;
    }
    struct fuse_context *ctx = fuse_get_context();
    return ctx ? ctx->private_data : nullptr;
}

struct fuse_operations buildOperations() {
    struct fuse_operations ops {};
    ops.init = fuseInitCb;
    ops.getattr = fuseGetattrCb;
    ops.readdir = fuseReaddirCb;
    ops.mkdir = fuseMkdirCb;
    ops.rmdir = fuseRmdirCb;
    ops.open = fuseOpenCb;
    ops.create = fuseCreateCb;
    ops.read = fuseReadCb;
    ops.write = fuseWriteCb;
    ops.truncate = fuseTruncateCb;
    ops.release = fuseReleaseCb;
    ops.unlink = fuseUnlinkCb;
    ops.rename = fuseRenameCb;
    ops.utimens = fuseUtimensCb;
    ops.chmod = fuseChmodCb;
    ops.chown = fuseChownCb;
    ops.statfs = fuseStatfsCb;
    return ops;
}

const struct fuse_operations s_ops = buildOperations();

} // namespace

FuseMount::FuseMount(Backend *backend, QObject *parent)
    : QObject(parent), m_backend(backend) {}

FuseMount::~FuseMount() {
    unmount();
}

int FuseMount::handleGetattr(const char *path, struct stat *stbuf) {
    if (!stbuf || !m_backend)
        return -EIO;

    const QString rel = normalizeRelPath(path);
    qint64 sizeBytes = 0;
    qint64 mtimeSecs = 0;
    bool isDir = false;
    bool isResident = false;

    if (!m_backend->vfsStat(rel, &sizeBytes, &mtimeSecs, &isDir, &isResident))
        return -ENOENT;

    fillStatFromFields(stbuf, ::getuid(), ::getgid(), sizeBytes, mtimeSecs, isDir, isResident);
    return 0;
}

QList<VfsDirEntry> FuseMount::handleReaddirEntries(const char *path, int *errOut) {
    if (errOut)
        *errOut = 0;
    if (!m_backend) {
        if (errOut)
            *errOut = -EIO;
        return {};
    }
    const QString rel = normalizeRelPath(path);
    struct fuse_context *ctx = fuse_get_context();
    const bool allowRemoteSync = !(ctx && isBackgroundIndexerOrThumbnailer(ctx->pid));
    bool exists = false;
    const QList<VfsDirEntry> entries = m_backend->vfsReadDirEntries(rel, &exists, allowRemoteSync);
    if (!exists && errOut)
        *errOut = -ENOENT;
    return entries;
}

QStringList FuseMount::handleReaddir(const char *path, int *errOut) {
    const QList<VfsDirEntry> entries = handleReaddirEntries(path, errOut);
    QStringList list;
    list.reserve(entries.size());
    for (const VfsDirEntry &e : entries)
        list.append(e.name);
    return list;
}

int FuseMount::handleOpenAndRead(const char *path, char *buf, size_t size, off_t offset) {
    if (!m_backend)
        return -EIO;
    const QString rel = normalizeRelPath(path);
    QString err;
    const QString localPath = m_backend->vfsOpenAndHydrate(rel, false, &err);
    if (localPath.isEmpty())
        return -ENOENT;

    int fd = ::open(localPath.toUtf8().constData(), O_RDONLY);
    if (fd < 0)
        return -errno;
    ssize_t n = ::pread(fd, buf, size, offset);
    int savedErrno = errno;
    ::close(fd);
    if (n < 0)
        return -savedErrno;
    return static_cast<int>(n);
}

int FuseMount::handleWriteFile(const char *path, const char *buf, size_t size, off_t offset) {
    if (!m_backend)
        return -EIO;
    const QString rel = normalizeRelPath(path);
    QString localPath = m_backend->vfsOpenAndHydrate(rel, true);
    if (localPath.isEmpty()) {
        if (!m_backend->vfsCreateFile(rel, &localPath))
            return -EIO;
    }

    int fd = ::open(localPath.toUtf8().constData(), O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return -errno;
    ssize_t n = ::pwrite(fd, buf, size, offset);
    int savedErrno = errno;
    ::close(fd);
    if (n < 0)
        return -savedErrno;
    m_backend->vfsWriteCompleted(rel);
    return static_cast<int>(n);
}

int FuseMount::handleUnlink(const char *path) {
    if (!m_backend)
        return -EIO;
    const QString rel = normalizeRelPath(path);
    return m_backend->vfsUnlinkFile(rel) ? 0 : -ENOENT;
}

int FuseMount::handleMkdir(const char *path, mode_t mode) {
    Q_UNUSED(mode);
    if (!m_backend)
        return -EIO;
    return m_backend->vfsMkdir(normalizeRelPath(path));
}

int FuseMount::handleRmdir(const char *path) {
    if (!m_backend)
        return -EIO;
    return m_backend->vfsRmdir(normalizeRelPath(path));
}

int FuseMount::handleRename(const char *oldPath, const char *newPath, unsigned int flags) {
    if (!m_backend)
        return -EIO;
    return m_backend->vfsRename(normalizeRelPath(oldPath), normalizeRelPath(newPath), flags);
}

bool FuseMount::isExternalFuseMounted(const QString &mountPoint) {
    QFile mounts(QStringLiteral("/proc/self/mounts"));
    if (!mounts.open(QIODevice::ReadOnly))
        return false;
    const QByteArray cleanTarget = QDir::cleanPath(QFileInfo(mountPoint).absoluteFilePath()).toUtf8();
    const QList<QByteArray> lines = mounts.readAll().split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> cols = line.split(' ');
        if (cols.size() >= 3 && cols.at(1) == cleanTarget && (cols.at(2).contains("fuse.wickfs") || cols.at(2).contains("fuse.gdrive"))) {
            struct stat st {};
            return ::stat(cleanTarget.constData(), &st) == 0;
        }
    }
    return false;
}

void FuseMount::invalidatePath(const QString &relPath) {
    if (!m_ownsFuse.load() || !m_kernelFuse.load())
        return;
    std::lock_guard<std::mutex> ptrLock(m_fusePtrMutex);
    if (!m_fuse || !m_ownsFuse.load() || !m_kernelFuse.load())
        return;
    QString clean = relPath;
    while (clean.startsWith('/'))
        clean.remove(0, 1);
    while (clean.endsWith('/'))
        clean.chop(1);
    const QByteArray absPath = clean.isEmpty() ? QByteArray("/") : (QByteArray("/") + clean.toUtf8());
    fuse_invalidate_path(m_fuse, absPath.constData());
}

bool FuseMount::mount(const QString &mountPoint, QString *errorOut) {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    if (m_mounted.load())
        return true;

    QDir().mkpath(mountPoint);
    m_mountPoint = mountPoint;

    const bool fuseAllowed = (qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")
                              || !qEnvironmentVariableIsEmpty("WICKFS_ENABLE_FUSE"))
                             && qEnvironmentVariableIsEmpty("WICKFS_DISABLE_FUSE");

    // If wickfs daemon already holds an active kernel FUSE mount at ~/Drive,
    // attach cleanly instead of yanking it out from under open file managers.
    // The daemon itself sets WICKFS_OWNER_FUSE so it always cleans stale mounts and owns FUSE.
    if (fuseAllowed && qEnvironmentVariableIsEmpty("WICKFS_OWNER_FUSE") && isExternalFuseMounted(mountPoint)) {
        m_mounted.store(true);
        m_kernelFuse.store(true);
        m_ownsFuse.store(false);
        m_lastError.clear();
        emit stateChanged();
        return true;
    }

    const bool hasDevFuse = QFileInfo::exists("/dev/fuse");
    const QString fusermountBin = QStandardPaths::findExecutable("fusermount3");

    if (hasDevFuse && !fusermountBin.isEmpty()) {
        // Clean up any stale mount at the target directory before mounting.
        QProcess cleanup;
        cleanup.setStandardErrorFile(QProcess::nullDevice());
        cleanup.setStandardOutputFile(QProcess::nullDevice());
        cleanup.start(fusermountBin, {"-u", "-z", mountPoint});
        cleanup.waitForFinished(1500);

        // Only attempt kernel FUSE mount when not running offscreen in a test harness.
        if (fuseAllowed) {
            char arg0[] = "wickfs";
            char arg1[] = "-o";
            char arg2[] = "fsname=wickfs,subtype=wickfs,default_permissions";
            char *argv[] = {arg0, arg1, arg2, nullptr};
            struct fuse_args args = FUSE_ARGS_INIT(3, argv);

            m_fuse = fuse_new(&args, &s_ops, sizeof(s_ops), this);
            fuse_opt_free_args(&args);

            if (!m_fuse) {
                qWarning("wickfs: fuse_new failed for %s", mountPoint.toUtf8().constData());
            } else {
                const int rc = fuse_mount(m_fuse, mountPoint.toUtf8().constData());
                if (rc == 0) {
                    m_mounted.store(true);
                    m_kernelFuse.store(true);
                    m_ownsFuse.store(true);
                    m_lastError.clear();
                    m_thread = std::thread([this]() {
                        struct fuse_loop_config *loopCfg = fuse_loop_cfg_create();
                        if (loopCfg) {
                            fuse_loop_cfg_set_max_threads(loopCfg, 8);
                            fuse_loop_cfg_set_idle_threads(loopCfg, 2);
                            fuse_loop_mt(m_fuse, loopCfg);
                            fuse_loop_cfg_destroy(loopCfg);
                        } else {
                            fuse_loop(m_fuse);
                        }
                    });
                    emit stateChanged();
                    return true;
                }
                qWarning("wickfs: fuse_mount(%s) failed with rc=%d errno=%d", mountPoint.toUtf8().constData(), rc, errno);
                fuse_destroy(m_fuse);
                m_fuse = nullptr;
            }
        }
    }

    // If kernel FUSE mount is blocked (e.g. inside a restricted container or
    // offscreen test sandbox), mount in managed workspace projection mode so
    // ~/Drive still exposes hydrated files and on-demand access seamlessly.
    if (QDir(mountPoint).exists()) {
        m_mounted.store(true);
        m_kernelFuse.store(false);
        m_ownsFuse.store(false);
        m_lastError.clear();
        emit stateChanged();
        return true;
    }

    m_lastError = tr("Could not mount %1").arg(mountPoint);
    if (errorOut)
        *errorOut = m_lastError;
    emit errorOccurred(m_lastError);
    return false;
}

void FuseMount::unmount() {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    if (!m_mounted.load())
        return;

    const bool owns = m_ownsFuse.load();
    const bool kernel = m_kernelFuse.load();
    m_kernelFuse.store(false);
    m_ownsFuse.store(false);

    if (owns && kernel && m_fuse) {
        fuse_exit(m_fuse);
        const QString fusermountBin = QStandardPaths::findExecutable("fusermount3");
        if (!fusermountBin.isEmpty() && !m_mountPoint.isEmpty())
            QProcess::execute(fusermountBin, {"-u", "-z", m_mountPoint});
        fuse_unmount(m_fuse);
        if (m_thread.joinable())
            m_thread.join();
        std::lock_guard<std::mutex> ptrLock(m_fusePtrMutex);
        fuse_destroy(m_fuse);
        m_fuse = nullptr;
    } else if (m_thread.joinable()) {
        m_thread.join();
    }

    m_mounted.store(false);
    emit stateChanged();
}
