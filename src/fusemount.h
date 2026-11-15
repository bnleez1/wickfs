#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <thread>

struct fuse;
class Backend;

// FUSE3 virtual filesystem that exposes Google Drive at ~/Drive. Cloud-only
// files report their full logical st_size with 0 allocated disk blocks
// (st_blocks == 0), and opening or reading any file hydrates it on-demand into
// the local cache while recording access frequency in the Backend.
class FuseMount : public QObject {
    Q_OBJECT

public:
    explicit FuseMount(Backend *backend, QObject *parent = nullptr);
    ~FuseMount() override;

    bool mount(const QString &mountPoint, QString *errorOut = nullptr);
    void unmount();

    bool isMounted() const { return m_mounted.load(); }
    bool isKernelFuse() const { return m_kernelFuse.load(); }
    QString mountPoint() const { return m_mountPoint; }
    QString lastError() const { return m_lastError; }
    static bool isExternalFuseMounted(const QString &mountPoint);

    // Direct VFS entry points used by both the libfuse3 callbacks and unit tests.
    int handleGetattr(const char *path, struct stat *stbuf);
    QStringList handleReaddir(const char *path, int *errOut = nullptr);
    int handleOpenAndRead(const char *path, char *buf, size_t size, off_t offset);
    int handleWriteFile(const char *path, const char *buf, size_t size, off_t offset);
    int handleUnlink(const char *path);
    int handleRename(const char *oldPath, const char *newPath, unsigned int flags = 0);

    Backend *backend() const { return m_backend; }

signals:
    void stateChanged();
    void errorOccurred(const QString &message);

private:
    Backend *m_backend = nullptr;
    QString m_mountPoint;
    QString m_lastError;
    struct fuse *m_fuse = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_mounted{false};
    std::atomic<bool> m_kernelFuse{false};
    std::atomic<bool> m_ownsFuse{false};
    mutable std::mutex m_lifecycleMutex;
};
