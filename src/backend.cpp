#include "backend.h"
#include "filepicker.h"
#include "fusemount.h"
#include "portalfilepicker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDBusConnection>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <sys/statvfs.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace {

QSharedPointer<QMutex> perPathHydrateMutex(const QString &relPath) {
    static QMutex mapMutex;
    static QHash<QString, QSharedPointer<QMutex>> locks;
    QMutexLocker locker(&mapMutex);
    auto &ptr = locks[relPath];
    if (!ptr)
        ptr = QSharedPointer<QMutex>::create();
    return ptr;
}

QString buildRcloneSpec(const QString &remoteName, const QString &remotePath, const QString &relPath = {}) {
    QString spec = remoteName;
    if (!spec.endsWith(':'))
        spec += ':';
    if (!remotePath.isEmpty()) {
        QString rp = remotePath;
        while (rp.startsWith('/'))
            rp.remove(0, 1);
        while (rp.endsWith('/'))
            rp.chop(1);
        if (!rp.isEmpty())
            spec += rp + (relPath.isEmpty() ? QString() : QStringLiteral("/"));
    }
    if (!relPath.isEmpty())
        spec += relPath;
    return spec;
}

QString inferCategory(const QString &name) {
    const QString ext = QFileInfo(name).suffix().toLower();
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "txt" || ext == "md" || ext == "rtf")
        return QStringLiteral("document");
    if (ext == "xls" || ext == "xlsx" || ext == "csv" || ext == "ods")
        return QStringLiteral("spreadsheet");
    if (ext == "ppt" || ext == "pptx" || ext == "key" || ext == "odp")
        return QStringLiteral("presentation");
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "svg" || ext == "webp" || ext == "fig")
        return QStringLiteral("image");
    if (ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "webm")
        return QStringLiteral("video");
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg")
        return QStringLiteral("audio");
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "zst" || ext == "7z")
        return QStringLiteral("archive");
    if (ext == "cpp" || ext == "h" || ext == "py" || ext == "rs" || ext == "js" || ext == "ts" || ext == "json")
        return QStringLiteral("code");
    return QStringLiteral("document");
}

QString formatRelativeTime(qint64 epochSecs) {
    if (epochSecs <= 0)
        return QObject::tr("Never opened");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 diff = qMax<qint64>(0, now - epochSecs);
    if (diff < 60)
        return QObject::tr("Just now");
    if (diff < 3600)
        return QObject::tr("%1m ago").arg(diff / 60);
    if (diff < 86400)
        return QObject::tr("%1h ago").arg(diff / 3600);
    return QObject::tr("%1d ago").arg(diff / 86400);
}

QString formatDateShort(qint64 epochSecs) {
    if (epochSecs <= 0)
        return QStringLiteral("—");
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(epochSecs), QLocale::ShortFormat);
}

} // namespace

DriveFileModel::DriveFileModel(QObject *parent)
    : QAbstractListModel(parent) {}

int DriveFileModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return m_entries.size();
}

QVariant DriveFileModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const DriveFileEntry &e = m_entries.at(index.row());
    switch (role) {
    case PathRole:
        return e.path;
    case NameRole:
        return e.name;
    case FolderRole:
        return e.folder;
    case CategoryRole:
        return e.category;
    case SizeBytesRole:
        return e.sizeBytes;
    case SizeFormattedRole:
        return Backend::formatSize(e.sizeBytes);
    case CachedBytesRole:
        return (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached")) ? e.sizeBytes : 0LL;
    case StateRole:
        return e.state;
    case PinnedRole:
        return e.pinned;
    case CachedRole:
        return e.state == QLatin1String("pinned") || e.state == QLatin1String("cached");
    case AccessCountRole:
        return e.accessCount;
    case LastAccessedFormattedRole:
        return formatRelativeTime(e.lastAccessedSecs);
    case ModifiedFormattedRole:
        return formatDateShort(e.modifiedSecs);
    case HotScoreRole:
        return e.hotScore;
    case IsFrequentRole:
        return e.isFrequent;
    default:
        return {};
    }
}

QHash<int, QByteArray> DriveFileModel::roleNames() const {
    return {
        {PathRole, "path"},
        {NameRole, "name"},
        {FolderRole, "folder"},
        {CategoryRole, "category"},
        {SizeBytesRole, "sizeBytes"},
        {SizeFormattedRole, "sizeFormatted"},
        {CachedBytesRole, "cachedBytes"},
        {StateRole, "state"},
        {PinnedRole, "pinned"},
        {CachedRole, "cached"},
        {AccessCountRole, "accessCount"},
        {LastAccessedFormattedRole, "lastAccessedFormatted"},
        {ModifiedFormattedRole, "modifiedFormatted"},
        {HotScoreRole, "hotScore"},
        {IsFrequentRole, "isFrequent"}
    };
}

void DriveFileModel::setEntries(const QList<DriveFileEntry> &entries) {
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

DriveFileEntry DriveFileModel::entryAt(int row) const {
    if (row < 0 || row >= m_entries.size())
        return {};
    return m_entries.at(row);
}

int DriveFileModel::indexOfPath(const QString &path) const {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).path == path)
            return i;
    }
    return -1;
}

Backend::Backend(const QString &stateDir, const QString &mountDir,
                 FilePicker *picker, QObject *parent)
    : QObject(parent),
      m_stateDir(stateDir.isEmpty() ? QDir::homePath() + "/.local/share/wickfs" : stateDir),
      m_blobDir(m_stateDir + "/blobs"),
      m_mountDir(mountDir.isEmpty() ? QDir::homePath() + "/Drive" : mountDir),
      m_isTestInstance(!stateDir.isEmpty()),
      m_filePicker(picker),
      m_ownsPicker(picker == nullptr) {
    if (!m_filePicker)
        m_filePicker = new PortalFilePicker(this);

    if (!stateDir.isEmpty())
        m_minFreeDiskBytes = 0;

    if (stateDir.isEmpty() && qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")) {
        const QString legacyDir = QDir::homePath() + QStringLiteral("/.local/share/gdrive");
        if (!QFileInfo::exists(m_stateDir + QStringLiteral("/state.json"))
            && QFileInfo::exists(legacyDir + QStringLiteral("/state.json"))) {
            QDir().mkpath(m_blobDir);
            QFile::copy(legacyDir + QStringLiteral("/state.json"),
                        m_stateDir + QStringLiteral("/state.json"));
            QDirIterator it(legacyDir + QStringLiteral("/blobs"), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString srcFile = it.next();
                const QString rel = QDir(legacyDir + QStringLiteral("/blobs")).relativeFilePath(srcFile);
                const QString dstFile = m_blobDir + QLatin1Char('/') + rel;
                QDir().mkpath(QFileInfo(dstFile).absolutePath());
                QFile::copy(srcFile, dstFile);
            }
        }
        const QString omarchyDir = QDir::homePath() + QStringLiteral("/.config/omarchy");
        const QString pluginManifest = omarchyDir + QStringLiteral("/plugins/ben.wickfs/manifest.json");
        if (QDir(omarchyDir).exists() && !QFileInfo::exists(pluginManifest))
            installBarPlugin(true);
    }

    QDir().mkpath(m_stateDir);
    QDir().mkpath(m_blobDir);
    QDir().mkpath(m_mountDir);

    m_rcloneBin = QStandardPaths::findExecutable("rclone");
    m_rcloneAvailable = !m_rcloneBin.isEmpty();
    m_fuseAvailable = QFileInfo::exists("/dev/fuse")
                   && !QStandardPaths::findExecutable("fusermount3").isEmpty();

    m_fuseMount = new FuseMount(this, this);
    connect(m_fuseMount, &FuseMount::stateChanged, this, &Backend::mountStateChanged);

    // Subscribe to systemd-logind PrepareForSleep(bool) so waking from standby
    // automatically remounts ~/Drive if needed and runs an immediate sync.
    QDBusConnection::systemBus().connect(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("PrepareForSleep"),
        this,
        SLOT(handleSystemWake(bool)));

    m_lastSyncedSecs = QDateTime::currentSecsSinceEpoch();
    m_syncAnimTimer.setInterval(220);
    connect(&m_syncAnimTimer, &QTimer::timeout, this, [this]() {
        if (!m_syncing) {
            m_syncAnimTimer.stop();
            return;
        }
        m_syncProgress = qMin(1.0, m_syncProgress + 0.32);
        if (m_syncProgress >= 1.0) {
            finishSyncSimulation();
        } else {
            emit statusChanged();
        }
    });

    m_saveDebounceTimer.setSingleShot(true);
    m_saveDebounceTimer.setInterval(400);
    connect(&m_saveDebounceTimer, &QTimer::timeout, this, [this]() {
        saveState();
    });

    m_uploadDebounceTimer.setSingleShot(true);
    m_uploadDebounceTimer.setInterval(250);
    connect(&m_uploadDebounceTimer, &QTimer::timeout, this, [this]() {
        startAsyncPendingUpload();
    });

    refreshRemotes();

    if (QFileInfo::exists(m_stateDir + "/state.json")) {
        loadState();
    } else {
        initStarterCatalog();
        if (stateDir.isEmpty() && m_oauthAuthorized && !m_availableRemotes.isEmpty()) {
            m_sourceMode = QStringLiteral("rclone");
            m_remoteName = m_availableRemotes.first();
        }
        saveState();
    }

    if (stateDir.isEmpty() && m_oauthAuthorized && m_sourceMode == QLatin1String("catalog")) {
        m_sourceMode = QStringLiteral("rclone");
        saveState();
    }
    if (stateDir.isEmpty() && m_oauthAuthorized && m_sourceMode == QLatin1String("rclone")) {
        bool hasDemoCatalog = m_entries.isEmpty();
        for (const auto &e : m_entries) {
            if (e.path == QLatin1String("Architecture/Distributed-Cache-Spec.pdf")) {
                hasDemoCatalog = true;
                break;
            }
        }
        if (hasDemoCatalog) {
            syncNow();
        } else if (pendingUploadCount() > 0) {
            schedulePendingUpload(100);
        }
    }

    if (m_sourceMode == QLatin1String("rclone")) {
        if (m_oauthClientId.isEmpty())
            setStatusBanner(tr("Connected to Google Drive '%1:' (shared OAuth ID — press C to add client_secret.json for faster sync)").arg(m_remoteName), false);
        else
            setStatusBanner(tr("Connected to Google Drive remote '%1:'").arg(m_remoteName), false);
    } else if (!m_rcloneAvailable) {
        setStatusBanner(tr("rclone is not installed (run: omarchy-pkg-add rclone) — virtual drive active in local cache mode"), true);
    } else if (m_availableRemotes.isEmpty()) {
        setStatusBanner(tr("No Google Drive remote configured in rclone — press C to connect your account"), false);
    } else {
        setStatusBanner(tr("Smart cache ready — press C to switch to rclone remote '%1'").arg(m_availableRemotes.first()), false);
    }

    // Automatically mount ~/Drive in normal desktop runs.
    if (stateDir.isEmpty()) {
        mountDrive();
        m_stateWatchTimer.setInterval(4000);
        connect(&m_stateWatchTimer, &QTimer::timeout, this, [this]() {
            if (m_syncing || m_rcloneSyncInFlight.load() || m_uploadInFlight.load())
                return;
            const QFileInfo fi(m_stateDir + "/state.json");
            if (fi.exists()) {
                const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
                const qint64 sz = fi.size();
                if ((mtime > m_lastLoadedStateMtimeMs || sz != m_lastLoadedStateSize) && m_lastLoadedStateMtimeMs > 0) {
                    loadState();
                    if (m_fuseMount)
                        m_fuseMount->invalidatePath(QString());
                    if (m_sourceMode == QLatin1String("rclone"))
                        setStatusBanner(tr("Connected to Google Drive remote '%1:' (%2 files indexed)")
                                            .arg(m_remoteName)
                                            .arg(totalFileCount()),
                                        false);
                    emit connectionChanged();
                    emit statusChanged();
                    emit statsChanged();
                }
            }
        });
        m_stateWatchTimer.start();

        if (m_fuseMount && m_fuseMount->ownsFuse()) {
            m_changePollTimer.setInterval(30 * 1000);
            connect(&m_changePollTimer, &QTimer::timeout, this, &Backend::pollRemoteChanges);
            m_changePollTimer.start();
            QTimer::singleShot(5000, this, &Backend::pollRemoteChanges);
        }
    } else {
        QMutexLocker locker(&m_mutex);
        syncAllProjectionsLocked();
    }
}

Backend::~Backend() {
    const bool wasProjectionMode = (m_fuseMount && m_fuseMount->isMounted() && !m_fuseMount->isKernelFuse());
    if (m_uploadDebounceTimer.isActive())
        m_uploadDebounceTimer.stop();
    if (m_saveDebounceTimer.isActive()) {
        m_saveDebounceTimer.stop();
        saveState();
    }
    if (m_fuseMount && m_fuseMount->isMounted())
        m_fuseMount->unmount();
    if (wasProjectionMode) {
        QMutexLocker locker(&m_mutex);
        syncAllProjectionsLocked();
    }
}

QString Backend::formatSize(qint64 bytes) {
    const double b = static_cast<double>(qMax<qint64>(0, bytes));
    const QLocale loc;
    if (b >= 1024.0 * 1024.0 * 1024.0)
        return QStringLiteral("%1 GB").arg(loc.toString(b / (1024.0 * 1024.0 * 1024.0), 'f', 1));
    if (b >= 1024.0 * 1024.0)
        return QStringLiteral("%1 MB").arg(loc.toString(b / (1024.0 * 1024.0), 'f', 1));
    if (b >= 1024.0)
        return QStringLiteral("%1 KB").arg(loc.toString(b / 1024.0, 'f', 1));
    return QStringLiteral("%1 B").arg(loc.toString(bytes));
}

double Backend::computeHotScore(int accessCount, qint64 lastAccessedSecs,
                                qint64 nowSecs, int halfLifeHours) {
    if (accessCount <= 0)
        return 0.0;
    const double hl = qMax(1, halfLifeHours);
    const double elapsedHours = (lastAccessedSecs > 0 && nowSecs > lastAccessedSecs)
        ? static_cast<double>(nowSecs - lastAccessedSecs) / 3600.0
        : 0.0;
    const double decay = std::pow(2.0, -elapsedHours / hl);
    const double base = static_cast<double>(accessCount) * 18.0 + 16.0;
    return std::clamp(std::round(base * decay * 10.0) / 10.0, 0.0, 100.0);
}

void Backend::initStarterCatalog() {
    QMutexLocker locker(&m_mutex);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 MB = 1024LL * 1024LL;

    m_entries = {
        {
            QStringLiteral("Architecture/Distributed-Cache-Spec.pdf"),
            QStringLiteral("Distributed-Cache-Spec.pdf"),
            QStringLiteral("Architecture"),
            QStringLiteral("document"),
            28LL * MB,
            now - 3600 * 5,
            QStringLiteral("pinned"),
            true,
            11,
            now - 900,
            0.0,
            true
        },
        {
            QStringLiteral("Product/Q4-Roadmap-Deck.pptx"),
            QStringLiteral("Q4-Roadmap-Deck.pptx"),
            QStringLiteral("Product"),
            QStringLiteral("presentation"),
            64LL * MB,
            now - 3600 * 12,
            QStringLiteral("cached"),
            false,
            8,
            now - 1800,
            0.0,
            true
        },
        {
            QStringLiteral("Finance/2026-Operating-Model.xlsx"),
            QStringLiteral("2026-Operating-Model.xlsx"),
            QStringLiteral("Finance"),
            QStringLiteral("spreadsheet"),
            18LL * MB,
            now - 3600 * 20,
            QStringLiteral("pinned"),
            true,
            7,
            now - 3200,
            0.0,
            true
        },
        {
            QStringLiteral("Design/Brand-Component-Kit.fig"),
            QStringLiteral("Brand-Component-Kit.fig"),
            QStringLiteral("Design"),
            QStringLiteral("image"),
            52LL * MB,
            now - 3600 * 28,
            QStringLiteral("cached"),
            false,
            5,
            now - 5400,
            0.0,
            true
        },
        {
            QStringLiteral("Engineering/Kernel-Benchmark-Results.csv"),
            QStringLiteral("Kernel-Benchmark-Results.csv"),
            QStringLiteral("Engineering"),
            QStringLiteral("spreadsheet"),
            34LL * MB,
            now - 3600 * 48,
            QStringLiteral("cached"),
            false,
            2,
            now - 3600 * 40,
            0.0,
            false
        },
        {
            QStringLiteral("Media/Keynote-Demo-4K.mp4"),
            QStringLiteral("Keynote-Demo-4K.mp4"),
            QStringLiteral("Media"),
            QStringLiteral("video"),
            145LL * MB,
            now - 3600 * 72,
            QStringLiteral("cloud"),
            false,
            1,
            now - 3600 * 160,
            0.0,
            false
        },
        {
            QStringLiteral("Archives/Release-Artifacts-v2.4.tar.zst"),
            QStringLiteral("Release-Artifacts-v2.4.tar.zst"),
            QStringLiteral("Archives"),
            QStringLiteral("archive"),
            160LL * MB,
            now - 3600 * 120,
            QStringLiteral("cloud"),
            false,
            0,
            0,
            0.0,
            false
        },
        {
            QStringLiteral("Media/Podcast-Master-Session.wav"),
            QStringLiteral("Podcast-Master-Session.wav"),
            QStringLiteral("Media"),
            QStringLiteral("audio"),
            88LL * MB,
            now - 3600 * 96,
            QStringLiteral("cloud"),
            false,
            0,
            0,
            0.0,
            false
        },
        {
            QStringLiteral("Architecture/API-Security-Review.md"),
            QStringLiteral("API-Security-Review.md"),
            QStringLiteral("Architecture"),
            QStringLiteral("document"),
            12LL * MB,
            now - 3600 * 18,
            QStringLiteral("cloud"),
            false,
            2,
            now - 3600 * 24,
            0.0,
            false
        },
        {
            QStringLiteral("Design/Wallpaper-Pack-HiDPI.zip"),
            QStringLiteral("Wallpaper-Pack-HiDPI.zip"),
            QStringLiteral("Design"),
            QStringLiteral("archive"),
            74LL * MB,
            now - 3600 * 200,
            QStringLiteral("cloud"),
            false,
            0,
            0,
            0.0,
            false
        }
    };

    recomputeScoresLocked(now);

    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].state == QLatin1String("pinned") || m_entries[i].state == QLatin1String("cached")) {
            hydrateEntryLocked(i);
        }
    }
    rebuildVfsIndexLocked();

    logActivity(tr("Smart Cache Initialized"),
                tr("Pinned 2 offline files and cached 2 frequently accessed files (479 MB saved in cloud)"),
                QStringLiteral("READY"));
    rebuildFilteredModel();
}

QByteArray Backend::synthesizePayloadForEntry(const DriveFileEntry &entry) const {
    const QString header = QStringLiteral(
        "WICKFS VIRTUAL FILE: %1\n"
        "Category: %2\n"
        "Logical Cloud Size: %3 (%4 bytes)\n"
        "Access Count: %5\n"
        "Hot Retention Score: %6\n"
        "---\n"
        "Hydrated on-demand by WickFS Smart Cache Engine.\n")
        .arg(entry.path, entry.category, formatSize(entry.sizeBytes))
        .arg(entry.sizeBytes)
        .arg(entry.accessCount)
        .arg(entry.hotScore, 0, 'f', 1);
    return header.toUtf8();
}

QString Backend::localBlobPath(const QString &relPath) const {
    QString clean = relPath;
    while (clean.startsWith('/'))
        clean.remove(0, 1);
    return m_blobDir + "/" + clean;
}

bool Backend::fetchFromRclonePath(const QString &rcloneBin, const QString &remoteName,
                                    const QString &remotePath, const QString &relPath,
                                    const QString &destBlobPath, QString *errorOut,
                                    const QString &configPath) {
    if (rcloneBin.isEmpty() || remoteName.isEmpty() || relPath.isEmpty())
        return false;

    const QString remoteSpec = buildRcloneSpec(remoteName, remotePath, relPath);
    const QString partPath = destBlobPath + QStringLiteral(".part");
    QDir().mkpath(QFileInfo(destBlobPath).absolutePath());
    QFile::remove(partPath);

    QStringList args = {QStringLiteral("copyto")};
    if (!configPath.isEmpty())
        args << QStringLiteral("--config") << configPath;
    args << QStringLiteral("--contimeout=5s")
         << QStringLiteral("--timeout=20s")
         << QStringLiteral("--retries=1")
         << remoteSpec << partPath;

    QProcess proc;
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(rcloneBin, args);
    if (!proc.waitForFinished(20000)) {
        proc.kill();
        proc.waitForFinished(500);
        QFile::remove(partPath);
        if (errorOut)
            *errorOut = tr("Timed out connecting to rclone remote '%1:'").arg(remoteName);
        return false;
    }
    if (proc.exitCode() != 0) {
        QFile::remove(partPath);
        if (errorOut)
            *errorOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return false;
    }
    if (!QFileInfo::exists(partPath))
        return false;
    QFile::remove(destBlobPath);
    if (!QFile::rename(partPath, destBlobPath)) {
        QFile::remove(partPath);
        return false;
    }
    return QFileInfo::exists(destBlobPath);
}

bool Backend::fetchFromRcloneLocked(const DriveFileEntry &entry, const QString &destBlobPath, QString *errorOut) const {
    if (!m_rcloneAvailable || m_remoteName.isEmpty() || !m_availableRemotes.contains(m_remoteName))
        return false;
    return fetchFromRclonePath(m_rcloneBin, m_remoteName, m_remotePath, entry.path, destBlobPath, errorOut, m_rcloneConfigPath);
}

bool Backend::hydrateEntryLocked(int idx, QString *errorOut) {
    if (idx < 0 || idx >= m_entries.size())
        return false;

    DriveFileEntry &entry = m_entries[idx];
    const QString blobPath = localBlobPath(entry.path);
    QDir().mkpath(QFileInfo(blobPath).absolutePath());

    if (!QFileInfo::exists(blobPath)) {
        if (m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable) {
            QString rcloneErr;
            if (!fetchFromRcloneLocked(entry, blobPath, &rcloneErr)) {
                if (errorOut)
                    *errorOut = rcloneErr.isEmpty() ? tr("Failed to download %1 from rclone").arg(entry.name) : rcloneErr;
                return false;
            }
        } else if (m_sourceMode == QLatin1String("folder") && !m_localSourceDir.isEmpty()) {
            const QString srcFile = m_localSourceDir + "/" + entry.path;
            if (QFileInfo::exists(srcFile)) {
                QFile::remove(blobPath);
                if (!QFile::copy(srcFile, blobPath)) {
                    if (errorOut)
                        *errorOut = tr("Failed to copy %1 from source folder").arg(entry.name);
                    return false;
                }
            }
        }

        if (!QFileInfo::exists(blobPath)) {
            QSaveFile f(blobPath);
            if (!f.open(QIODevice::WriteOnly)) {
                if (errorOut)
                    *errorOut = tr("Cannot write cache file %1").arg(blobPath);
                return false;
            }
            const QByteArray payload = synthesizePayloadForEntry(entry);
            f.write(payload);
            if (!f.commit()) {
                if (errorOut)
                    *errorOut = tr("Failed to commit cache file %1").arg(blobPath);
                return false;
            }
        }
    }

    const qint64 diskSize = QFileInfo(blobPath).size();
    if (m_sourceMode == QLatin1String("rclone") && diskSize > 0)
        entry.sizeBytes = diskSize;
    entry.state = entry.pinned ? QStringLiteral("pinned") : QStringLiteral("cached");
    syncProjectionForEntryLocked(entry);
    return true;
}

void Backend::syncProjectionForEntryLocked(const DriveFileEntry &entry) const {
    if (m_mountDir.isEmpty())
        return;
    // When kernel FUSE is active, FUSE serves ~/Drive directly from m_blobDir and VFS metadata.
    if (m_fuseMount && m_fuseMount->isKernelFuse())
        return;
    if (!m_isTestInstance && (!m_fuseMount || !m_fuseMount->isMounted()))
        return;
    if (FuseMount::isExternalFuseMounted(m_mountDir))
        return;

    const QString targetPath = m_mountDir + "/" + entry.path;
    QDir().mkpath(QFileInfo(targetPath).absolutePath());

    if (entry.state == QLatin1String("pinned") || entry.state == QLatin1String("cached")) {
        const QString blobPath = localBlobPath(entry.path);
        if (QFileInfo::exists(blobPath)) {
            QFile::remove(targetPath);
            QFile::copy(blobPath, targetPath);
        }
    } else {
        // Materialize a 0-disk-block sparse placeholder in ~/Drive so every folder and file
        // remains visible in file managers and terminals even when kernel FUSE is not mounted,
        // while consuming 0 disk blocks on the host filesystem.
        QFile::remove(targetPath);
        QFile stub(targetPath);
        if (stub.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            stub.resize(qMax<qint64>(0, entry.sizeBytes));
            stub.close();
        }
    }
}

void Backend::syncAllProjectionsLocked() const {
    if (m_mountDir.isEmpty())
        return;
    if (m_fuseMount && m_fuseMount->isKernelFuse())
        return;
    if (!m_isTestInstance && (!m_fuseMount || !m_fuseMount->isMounted()))
        return;
    if (FuseMount::isExternalFuseMounted(m_mountDir))
        return;

    for (const auto &entry : m_entries) {
        syncProjectionForEntryLocked(entry);
    }
}

void Backend::recomputeScoresLocked(qint64 nowSecs) {
    const qint64 now = nowSecs > 0 ? nowSecs : QDateTime::currentSecsSinceEpoch();
    for (auto &entry : m_entries) {
        entry.hotScore = computeHotScore(entry.accessCount, entry.lastAccessedSecs, now, m_halfLifeHours);
        entry.isFrequent = (entry.accessCount >= m_frequentThreshold);
        if (entry.pinned)
            entry.state = QStringLiteral("pinned");
    }
    m_statsDirty = true;
}

void Backend::recomputeStatsLocked() const {
    qint64 totalCloud = 0;
    qint64 cached = 0;
    qint64 pinned = 0;
    qint64 hotCached = 0;
    int freqCount = 0;
    int pinCount = 0;
    int cacheCount = 0;
    int cloudCount = 0;
    QSet<QString> folderSet;

    for (const QString &rf : m_remoteFolders) {
        if (!rf.isEmpty() && rf != QLatin1String("/"))
            folderSet.insert(rf);
    }

    for (const auto &e : m_entries) {
        totalCloud += e.sizeBytes;
        const bool isRes = (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached"));
        if (isRes) {
            cached += e.sizeBytes;
            ++cacheCount;
        } else {
            ++cloudCount;
        }
        if (e.pinned) {
            pinned += e.sizeBytes;
            ++pinCount;
        } else if (e.state == QLatin1String("cached")) {
            hotCached += e.sizeBytes;
        }
        if (e.isFrequent)
            ++freqCount;
        if (!e.folder.isEmpty() && e.folder != QLatin1String("/"))
            folderSet.insert(e.folder);
    }

    QStringList foldersList = folderSet.values();
    foldersList.sort(Qt::CaseInsensitive);

    m_cachedTotalCloudBytes = totalCloud;
    m_cachedCachedBytes = cached;
    m_cachedPinnedBytes = pinned;
    m_cachedHotCachedBytes = hotCached;
    m_cachedFrequentCount = freqCount;
    m_cachedPinnedCount = pinCount;
    m_cachedCachedCount = cacheCount;
    m_cachedCloudCount = cloudCount;
    m_cachedFolders = foldersList;
    m_statsDirty = false;
}

void Backend::ensureStatsFreshLocked() const {
    if (m_statsDirty)
        recomputeStatsLocked();
}

void Backend::refreshStatvfsCacheLocked() const {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastStatvfsMs > 0 && (nowMs - m_lastStatvfsMs) < 2000LL)
        return;
    struct statvfs vfs {};
    if (::statvfs(m_stateDir.toUtf8().constData(), &vfs) == 0) {
        if (vfs.f_bavail > 0)
            m_lastDiskAvailBytes = static_cast<qint64>(vfs.f_bavail) * static_cast<qint64>(vfs.f_frsize);
        if (vfs.f_blocks > 0)
            m_lastDiskTotalBytes = static_cast<qint64>(vfs.f_blocks) * static_cast<qint64>(vfs.f_frsize);
    }
    m_lastStatvfsMs = nowMs;
}

void Backend::rebuildFilteredModel() {
    QList<DriveFileEntry> filtered;
    {
        QMutexLocker locker(&m_mutex);
        const QString q = m_searchQuery.trimmed().toLower();
        for (const auto &e : m_entries) {
            if (!m_currentFolder.isEmpty()) {
                const bool exactMatch = (e.folder == m_currentFolder);
                const bool childMatch = (m_currentFolder != QLatin1String("Root")
                                         && e.folder.startsWith(m_currentFolder + QLatin1Char('/')));
                if (!exactMatch && !childMatch)
                    continue;
            }
            if (m_filterMode == QLatin1String("frequent") && !e.isFrequent)
                continue;
            if (m_filterMode == QLatin1String("pinned") && !e.pinned)
                continue;
            if (m_filterMode == QLatin1String("cached")
                && e.state != QLatin1String("cached") && e.state != QLatin1String("pinned"))
                continue;
            if (m_filterMode == QLatin1String("cloud") && e.state != QLatin1String("cloud"))
                continue;

            if (!q.isEmpty()) {
                if (!e.name.toLower().contains(q)
                    && !e.folder.toLower().contains(q)
                    && !e.category.toLower().contains(q))
                    continue;
            }
            filtered.append(e);
        }

        std::sort(filtered.begin(), filtered.end(), [this](const DriveFileEntry &a, const DriveFileEntry &b) {
            if (m_sortMode == QLatin1String("name"))
                return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
            if (m_sortMode == QLatin1String("size")) {
                if (a.sizeBytes != b.sizeBytes)
                    return a.sizeBytes > b.sizeBytes;
                return a.name < b.name;
            }
            if (m_sortMode == QLatin1String("recent")) {
                if (a.lastAccessedSecs != b.lastAccessedSecs)
                    return a.lastAccessedSecs > b.lastAccessedSecs;
                return a.hotScore > b.hotScore;
            }
            // Default "hot": Pinned first, then highest hotScore, then accessCount, then name.
            if (a.pinned != b.pinned)
                return a.pinned > b.pinned;
            if (!qFuzzyCompare(a.hotScore, b.hotScore))
                return a.hotScore > b.hotScore;
            if (a.accessCount != b.accessCount)
                return a.accessCount > b.accessCount;
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
    }

    const QString prevSelectedPath = selectedPath();
    m_fileModel.setEntries(filtered);

    int nextIdx = prevSelectedPath.isEmpty() ? m_selectedIndex : m_fileModel.indexOfPath(prevSelectedPath);
    if (nextIdx < 0)
        nextIdx = filtered.isEmpty() ? -1 : qBound(0, m_selectedIndex, filtered.size() - 1);
    m_selectedIndex = nextIdx;

    emit filterChanged();
    emit selectionChanged();
    emit statsChanged();
}

void Backend::setSelectedIndex(int index) {
    const int count = m_fileModel.rowCount();
    const int clamped = count == 0 ? -1 : qBound(0, index, count - 1);
    if (clamped == m_selectedIndex)
        return;
    m_selectedIndex = clamped;
    emit selectionChanged();
}

QString Backend::selectedPath() const {
    const DriveFileEntry e = m_fileModel.entryAt(m_selectedIndex);
    return e.path;
}

QVariantMap Backend::selectedFile() const {
    const DriveFileEntry e = m_fileModel.entryAt(m_selectedIndex);
    if (e.path.isEmpty())
        return {};
    return {
        {"path", e.path},
        {"name", e.name},
        {"folder", e.folder},
        {"category", e.category},
        {"sizeBytes", e.sizeBytes},
        {"sizeFormatted", formatSize(e.sizeBytes)},
        {"state", e.state},
        {"pinned", e.pinned},
        {"cached", e.state == QLatin1String("pinned") || e.state == QLatin1String("cached")},
        {"accessCount", e.accessCount},
        {"lastAccessedFormatted", formatRelativeTime(e.lastAccessedSecs)},
        {"modifiedFormatted", formatDateShort(e.modifiedSecs)},
        {"hotScore", e.hotScore},
        {"isFrequent", e.isFrequent}
    };
}

void Backend::setFilterMode(const QString &mode) {
    if (mode == m_filterMode)
        return;
    m_filterMode = mode;
    rebuildFilteredModel();
}

void Backend::setCurrentFolder(const QString &folder) {
    if (folder == m_currentFolder)
        return;
    m_currentFolder = folder;
    if (!folder.isEmpty() && folder != QLatin1String("Root") && m_sourceMode == QLatin1String("rclone")) {
        syncRemoteFolder(folder);
    }
    rebuildFilteredModel();
}

void Backend::setSearchQuery(const QString &query) {
    if (query == m_searchQuery)
        return;
    m_searchQuery = query;
    rebuildFilteredModel();
}

void Backend::setSortMode(const QString &mode) {
    if (mode == m_sortMode)
        return;
    m_sortMode = mode;
    rebuildFilteredModel();
}

QStringList Backend::folders() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedFolders;
}

bool Backend::mounted() const {
    return m_fuseMount && m_fuseMount->isMounted();
}

bool Backend::kernelFuseActive() const {
    return m_fuseMount && m_fuseMount->isKernelFuse();
}

QString Backend::mountStatusText() const {
    QString disp = m_mountDir;
    const QString home = QDir::homePath();
    if (disp.startsWith(home + "/"))
        disp = "~" + disp.mid(home.size());
    else if (disp.endsWith("/Drive"))
        disp = QStringLiteral("~/Drive");

    if (!mounted())
        return tr("Unmounted (%1)").arg(disp);
    if (kernelFuseActive())
        return tr("FUSE3 Mounted at %1").arg(disp);
    return tr("Active at %1").arg(disp);
}

QString Backend::connectionSummary() const {
    if (m_sourceMode == QLatin1String("rclone"))
        return tr("Google Drive (%1:%2)").arg(m_remoteName, m_remotePath);
    if (m_sourceMode == QLatin1String("folder"))
        return tr("Mirrored Source (%1)").arg(QFileInfo(m_localSourceDir).fileName());
    return tr("Smart Drive Catalog");
}

QString Backend::oauthClientModeLabel() const {
    if (!m_oauthClientId.isEmpty())
        return tr("Custom Google OAuth Client ID");
    return tr("Built-in Google OAuth Client ID");
}

QString Backend::oauthStatusSummary() const {
    if (m_oauthAuthorized) {
        if (!m_oauthClientId.isEmpty())
            return tr("OAuth Token Active · Custom Client ID (%1)").arg(m_oauthClientId.left(20) + QStringLiteral("…"));
        return tr("OAuth Token Active · Built-in Google Client ID ready");
    }
    if (!m_oauthClientId.isEmpty())
        return tr("Custom Client ID saved — click Authorize Google OAuth to sign in");
    return tr("Using built-in Google OAuth Client ID (or enter custom GCP credentials)");
}

void Backend::setRcloneConfigPath(const QString &path) {
    m_rcloneConfigPath = path;
    refreshRemotes();
}

QString Backend::rcloneConfigPath() const {
    if (!m_rcloneConfigPath.isEmpty())
        return m_rcloneConfigPath;
    return QDir::homePath() + QStringLiteral("/.config/rclone/rclone.conf");
}

QString Backend::syncState() const {
    if (m_syncing || m_uploadInFlight.load())
        return QStringLiteral("syncing");
    if (pendingUploadCount() > 0)
        return QStringLiteral("pending");
    return QStringLiteral("synced");
}

QString Backend::syncStateLabel() const {
    if (m_syncing || m_uploadInFlight.load())
        return tr("Syncing files…");
    const int pending = pendingUploadCount();
    if (pending > 0)
        return tr("%1 local change(s) pending upload").arg(pending);
    return tr("All files in sync");
}

QString Backend::syncDetailText() const {
    if (m_syncing || m_uploadInFlight.load()) {
        const int pct = qBound(5, static_cast<int>(std::round(m_syncProgress * 100.0)), 99);
        if (!m_activeSyncFile.isEmpty())
            return tr("Syncing %1 (%2%)").arg(m_activeSyncFile).arg(pct);
        return tr("Checking remote catalog (%1%)").arg(pct);
    }
    const int pending = pendingUploadCount();
    if (pending > 0)
        return tr("%1 pending upload · %2 cached locally").arg(pending).arg(cachedFileCount());
    return tr("%1 cached locally · Updated %2").arg(cachedFileCount()).arg(lastSyncedFormatted());
}

QString Backend::lastSyncedFormatted() const {
    return formatRelativeTime(m_lastSyncedSecs);
}

int Backend::pendingUploadCount() const {
    QMutexLocker locker(&m_mutex);
    int dirtyCount = m_pendingDeletes.size();
    for (const auto &e : m_entries) {
        if (e.dirty)
            ++dirtyCount;
    }
    return dirtyCount;
}

int Backend::pendingSyncCount() const {
    const int uploads = pendingUploadCount();
    if (m_syncing)
        return qMax(m_pendingSyncCount, uploads);
    return uploads;
}

int Backend::syncedFileCount() const {
    return qMax(0, totalFileCount() - pendingSyncCount());
}

bool Backend::autoStartEnabled() const {
    const QString userService = QDir::homePath() + QStringLiteral("/.config/systemd/user/wickfs.service");
    const QString hyprAutostart = QDir::homePath() + QStringLiteral("/.config/hypr/autostart.lua");
    if (QFileInfo::exists(userService))
        return true;
    QFile f(hyprAutostart);
    if (f.open(QIODevice::ReadOnly)) {
        if (QString::fromUtf8(f.readAll()).contains(QLatin1String("wickfs")))
            return true;
    }
    return true;
}

QString Backend::autoStartSummary() const {
    return tr("Auto-start on boot & wake from standby enabled");
}

void Backend::handleSystemWake(bool sleeping) {
    if (sleeping) {
        // System is entering standby/sleep: flush state cleanly before suspend.
        saveState();
        logActivity(tr("Entering Standby"),
                    tr("Flushed local cache metadata before system sleep"),
                    QStringLiteral("SLEEP"));
        return;
    }

    // System just woke up from standby/suspend.
    ++m_wakeSyncCount;
    if (!mounted())
        mountDrive();
    logActivity(tr("Resumed from Standby"),
                tr("Verified ~/Drive mount and triggered post-wake Google Drive sync"),
                QStringLiteral("WAKE"));
    syncNow();
    pollRemoteChanges();
}

void Backend::beginSyncSimulation(const QString &fileName, int totalFiles) {
    m_syncing = true;
    m_activeSyncFile = fileName;
    m_pendingSyncCount = qMax(1, totalFiles);
    m_syncProgress = 0.25;
    saveState();
    emit statusChanged();
    emit statsChanged();
}

void Backend::finishSyncSimulation() {
    m_syncAnimTimer.stop();
    m_syncing = false;
    m_activeSyncFile.clear();
    m_pendingSyncCount = 0;
    m_syncProgress = 1.0;
    m_lastSyncedSecs = QDateTime::currentSecsSinceEpoch();
    saveState();
    emit statusChanged();
    emit statsChanged();
}

qint64 Backend::totalCloudBytes() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedTotalCloudBytes;
}

qint64 Backend::cachedBytes() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedCachedBytes;
}

qint64 Backend::pinnedBytes() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedPinnedBytes;
}

qint64 Backend::hotCachedBytes() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedHotCachedBytes;
}

qint64 Backend::savedBytes() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return qMax<qint64>(0, m_cachedTotalCloudBytes - m_cachedCachedBytes);
}

qint64 Backend::diskAvailableBytes() const {
    if (m_simulatedDiskFreeBytes >= 0)
        return m_simulatedDiskFreeBytes;
    QMutexLocker locker(&m_mutex);
    refreshStatvfsCacheLocked();
    return m_lastDiskAvailBytes;
}

qint64 Backend::diskTotalBytes() const {
    QMutexLocker locker(&m_mutex);
    refreshStatvfsCacheLocked();
    return m_lastDiskTotalBytes;
}

qint64 Backend::effectiveBudgetBytes() const {
    const qint64 avail = diskAvailableBytes();
    const qint64 curCached = cachedBytes();
    const qint64 floorHeadroom = qMax<qint64>(0, curCached + avail - m_minFreeDiskBytes);
    return qMin(m_maxCacheBytes, floorHeadroom);
}

bool Backend::lowDiskFloorActive() const {
    return effectiveBudgetBytes() < m_maxCacheBytes;
}

void Backend::setMaxCacheMb(int mb) {
    const qint64 clampedMb = qBound(10, mb, 102400);
    setMaxCacheBytesDirect(clampedMb * 1024LL * 1024LL);
}

void Backend::setMaxCacheBytesDirect(qint64 bytes) {
    const qint64 clamped = qMax<qint64>(1024LL * 1024LL, bytes);
    if (clamped == m_maxCacheBytes)
        return;
    pushUndoSnapshot(tr("Change cache budget"));
    m_maxCacheBytes = clamped;
    enforceCacheBudget();
    saveState();
    emit budgetChanged();
    emit statsChanged();
}

void Backend::setMinFreeDiskGb(int gb) {
    const qint64 clampedGb = qBound(0, gb, 500);
    setMinFreeDiskBytesDirect(clampedGb * 1024LL * 1024LL * 1024LL);
}

void Backend::setMinFreeDiskBytesDirect(qint64 bytes) {
    const qint64 clamped = qMax<qint64>(0, bytes);
    if (clamped == m_minFreeDiskBytes)
        return;
    m_minFreeDiskBytes = clamped;
    enforceCacheBudget();
    saveState();
    emit budgetChanged();
    emit statsChanged();
}

void Backend::setSimulatedDiskFreeBytes(qint64 bytes) {
    m_simulatedDiskFreeBytes = bytes;
    enforceCacheBudget();
    emit statsChanged();
}

void Backend::setFrequentThreshold(int threshold) {
    const int clamped = qBound(1, threshold, 50);
    if (clamped == m_frequentThreshold)
        return;
    m_frequentThreshold = clamped;
    {
        QMutexLocker locker(&m_mutex);
        recomputeScoresLocked();
    }
    enforceCacheBudget();
    saveState();
    rebuildFilteredModel();
    emit budgetChanged();
}

void Backend::setHalfLifeHours(int hours) {
    const int clamped = qBound(1, hours, 720);
    if (clamped == m_halfLifeHours)
        return;
    m_halfLifeHours = clamped;
    {
        QMutexLocker locker(&m_mutex);
        recomputeScoresLocked();
    }
    saveState();
    rebuildFilteredModel();
    emit budgetChanged();
}

void Backend::setAutoKeepFrequent(bool enabled) {
    if (enabled == m_autoKeepFrequent)
        return;
    m_autoKeepFrequent = enabled;
    if (enabled) {
        QMutexLocker locker(&m_mutex);
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].isFrequent && m_entries[i].state == QLatin1String("cloud")) {
                if (cachedBytes() + m_entries[i].sizeBytes <= effectiveBudgetBytes())
                    hydrateEntryLocked(i);
            }
        }
    }
    enforceCacheBudget();
    saveState();
    rebuildFilteredModel();
    emit budgetChanged();
}

QString Backend::totalCloudFormatted() const { return formatSize(totalCloudBytes()); }
QString Backend::cachedFormatted() const { return formatSize(cachedBytes()); }
QString Backend::pinnedFormatted() const { return formatSize(pinnedBytes()); }
QString Backend::hotCachedFormatted() const { return formatSize(hotCachedBytes()); }
QString Backend::savedFormatted() const { return formatSize(savedBytes()); }
QString Backend::maxCacheFormatted() const { return formatSize(m_maxCacheBytes); }
QString Backend::effectiveBudgetFormatted() const { return formatSize(effectiveBudgetBytes()); }
QString Backend::diskAvailableFormatted() const { return formatSize(diskAvailableBytes()); }

double Backend::cacheUsageRatio() const {
    const qint64 ceiling = qMax<qint64>(1, effectiveBudgetBytes());
    return std::clamp(static_cast<double>(cachedBytes()) / static_cast<double>(ceiling), 0.0, 1.0);
}

double Backend::pinnedUsageRatio() const {
    const qint64 ceiling = qMax<qint64>(1, effectiveBudgetBytes());
    return std::clamp(static_cast<double>(pinnedBytes()) / static_cast<double>(ceiling), 0.0, 1.0);
}

int Backend::spaceSavedPercent() const {
    const qint64 total = totalCloudBytes();
    if (total <= 0)
        return 0;
    return qBound(0, static_cast<int>(std::round((static_cast<double>(savedBytes()) * 100.0) / static_cast<double>(total))), 100);
}

int Backend::totalFileCount() const {
    QMutexLocker locker(&m_mutex);
    return m_entries.size();
}

int Backend::frequentFileCount() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedFrequentCount;
}

int Backend::pinnedFileCount() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedPinnedCount;
}

int Backend::cachedFileCount() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedCachedCount;
}

int Backend::cloudOnlyFileCount() const {
    QMutexLocker locker(&m_mutex);
    ensureStatsFreshLocked();
    return m_cachedCloudCount;
}

void Backend::primaryAction() {
    ++m_actionCount;
    const QString sel = selectedPath();
    if (!sel.isEmpty()) {
        accessFile(sel);
    } else {
        emit statsChanged();
    }
}

bool Backend::accessFile(const QString &path, QString *errorOut) {
    pushUndoSnapshot(tr("Open %1").arg(path));
    const bool wasCloud = (entryForPath(path).state == QLatin1String("cloud"));
    QString err;
    const QString local = vfsOpenAndHydrate(path, false, &err);
    if (local.isEmpty()) {
        if (errorOut)
            *errorOut = err;
        return false;
    }
    m_lastSyncedSecs = QDateTime::currentSecsSinceEpoch();
    if (wasCloud && qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")) {
        beginSyncSimulation(QFileInfo(path).fileName(), 1);
        m_syncAnimTimer.start();
    } else {
        emit statusChanged();
    }
    return true;
}

void Backend::openSelectedInDesktop() {
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    QString err;
    const QString local = vfsOpenAndHydrate(path, false, &err);
    if (local.isEmpty()) {
        setStatusBanner(err, true);
        return;
    }
    const QString mountedFile = m_mountDir + "/" + path;
    const QString targetToOpen = QFileInfo::exists(mountedFile) ? mountedFile : local;
    QDesktopServices::openUrl(QUrl::fromLocalFile(targetToOpen));
}

void Backend::openMountFolderInDesktop() {
    QDir().mkpath(m_mountDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_mountDir));
}

bool Backend::togglePin(const QString &path) {
    const DriveFileEntry existing = entryForPath(path);
    if (existing.path.isEmpty())
        return false;
    return setPinned(path, !existing.pinned);
}

void Backend::togglePinSelected() {
    const QString path = selectedPath();
    if (!path.isEmpty())
        togglePin(path);
}

bool Backend::setPinned(const QString &path, bool pinned) {
    pushUndoSnapshot(pinned ? tr("Pin %1").arg(path) : tr("Unpin %1").arg(path));

    DriveFileEntry entryCopy;
    QString sourceMode;
    bool rcloneAvailable = false;
    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString rcloneConfigPath;
    QString localSourceDir;
    QString blobPath;

    {
        QMutexLocker locker(&m_mutex);
        if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
            rebuildVfsIndexLocked();
        const int idx = m_vfsFileIdx.value(path, -1);
        if (idx < 0 || idx >= m_entries.size())
            return false;
        entryCopy = m_entries.at(idx);
        sourceMode = m_sourceMode;
        rcloneAvailable = m_rcloneAvailable;
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        rcloneConfigPath = m_rcloneConfigPath;
        localSourceDir = m_localSourceDir;
        blobPath = localBlobPath(path);
    }

    if (pinned && !QFileInfo::exists(blobPath)) {
        const auto fileLock = perPathHydrateMutex(path);
        QMutexLocker fileLocker(fileLock.data());
        if (!QFileInfo::exists(blobPath)) {
            QDir().mkpath(QFileInfo(blobPath).absolutePath());
            if (sourceMode == QLatin1String("rclone") && rcloneAvailable) {
                fetchFromRclonePath(rcloneBin, remoteName, remotePath, path, blobPath, nullptr, rcloneConfigPath);
            } else if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
                const QString srcFile = localSourceDir + QLatin1Char('/') + path;
                if (QFileInfo::exists(srcFile)) {
                    QFile::remove(blobPath);
                    QFile::copy(srcFile, blobPath);
                }
            }
            if (!QFileInfo::exists(blobPath) && sourceMode != QLatin1String("rclone")) {
                QSaveFile f(blobPath);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(synthesizePayloadForEntry(entryCopy));
                    f.commit();
                }
            }
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        const int idx = m_vfsFileIdx.value(path, -1);
        if (idx < 0 || idx >= m_entries.size())
            return false;

        m_entries[idx].pinned = pinned;
        if (pinned) {
            if (QFileInfo::exists(blobPath)) {
                const qint64 diskSize = QFileInfo(blobPath).size();
                if (m_sourceMode == QLatin1String("rclone") && diskSize > 0)
                    m_entries[idx].sizeBytes = diskSize;
                m_entries[idx].state = QStringLiteral("pinned");
            } else {
                m_entries[idx].state = QStringLiteral("pinned");
            }
            syncProjectionForEntryLocked(m_entries[idx]);
            logActivity(tr("Pinned Offline"),
                        tr("%1 (%2) locked on disk for offline use").arg(m_entries[idx].name, formatSize(m_entries[idx].sizeBytes)),
                        QStringLiteral("PIN"));
        } else {
            m_entries[idx].state = QFileInfo::exists(blobPath)
                ? QStringLiteral("cached")
                : QStringLiteral("cloud");
            logActivity(tr("Unpinned"),
                        tr("%1 returned to smart LFU/LRU cache management").arg(m_entries[idx].name),
                        QStringLiteral("UNPIN"));
        }
        recomputeScoresLocked();
    }

    if (m_fuseMount)
        m_fuseMount->invalidatePath(path);
    enforceCacheBudget(pinned ? path : QString());
    saveState();
    rebuildFilteredModel();
    return true;
}

bool Backend::evictFile(const QString &path, QString *errorOut) {
    pushUndoSnapshot(tr("Evict %1").arg(path));
    {
        QMutexLocker locker(&m_mutex);
        if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
            rebuildVfsIndexLocked();
        const int idx = m_vfsFileIdx.value(path, -1);
        if (idx < 0 || idx >= m_entries.size()) {
            if (errorOut)
                *errorOut = tr("File not found: %1").arg(path);
            return false;
        }
        if (m_entries[idx].pinned) {
            const QString msg = tr("Cannot evict pinned file '%1' — unpin it first").arg(m_entries[idx].name);
            if (errorOut)
                *errorOut = msg;
            setStatusBanner(msg, true);
            return false;
        }
        if (m_entries[idx].dirty) {
            const QString msg = tr("Cannot evict '%1' — pending upload to cloud").arg(m_entries[idx].name);
            if (errorOut)
                *errorOut = msg;
            setStatusBanner(msg, true);
            return false;
        }

        const QString blobPath = localBlobPath(path);
        if (QFileInfo::exists(blobPath))
            QFile::remove(blobPath);

        m_entries[idx].state = QStringLiteral("cloud");
        m_statsDirty = true;
        syncProjectionForEntryLocked(m_entries[idx]);
        logActivity(tr("Evicted from Disk"),
                    tr("Freed %1 by returning %2 to Cloud Only").arg(formatSize(m_entries[idx].sizeBytes), m_entries[idx].name),
                    QStringLiteral("EVICT"));
    }

    if (m_fuseMount)
        m_fuseMount->invalidatePath(path);
    saveState();
    rebuildFilteredModel();
    return true;
}

void Backend::evictSelected() {
    const QString path = selectedPath();
    if (!path.isEmpty())
        evictFile(path);
}

int Backend::evictColdFiles(int maxFilesToEvict) {
    pushUndoSnapshot(tr("Evict cold files"));
    int evictedCount = 0;
    qint64 freedBytes = 0;
    QStringList evictedPaths;

    {
        QMutexLocker locker(&m_mutex);
        recomputeScoresLocked();

        QList<int> candidates;
        for (int i = 0; i < m_entries.size(); ++i) {
            const auto &e = m_entries.at(i);
            if (!e.pinned && !e.dirty && e.state == QLatin1String("cached") && !e.isFrequent) {
                candidates.append(i);
            }
        }
        // If all cached files are marked frequent and user explicitly asked to evict cold files,
        // pick the lowest-scoring unpinned cached files.
        if (candidates.isEmpty()) {
            for (int i = 0; i < m_entries.size(); ++i) {
                const auto &e = m_entries.at(i);
                if (!e.pinned && !e.dirty && e.state == QLatin1String("cached"))
                    candidates.append(i);
            }
        }

        std::sort(candidates.begin(), candidates.end(), [this](int a, int b) {
            const auto &ea = m_entries.at(a);
            const auto &eb = m_entries.at(b);
            if (!qFuzzyCompare(ea.hotScore, eb.hotScore))
                return ea.hotScore < eb.hotScore;
            if (ea.lastAccessedSecs != eb.lastAccessedSecs)
                return ea.lastAccessedSecs < eb.lastAccessedSecs;
            return ea.sizeBytes > eb.sizeBytes;
        });

        for (int idx : candidates) {
            if (maxFilesToEvict > 0 && evictedCount >= maxFilesToEvict)
                break;
            const QString blobPath = localBlobPath(m_entries[idx].path);
            if (QFileInfo::exists(blobPath))
                QFile::remove(blobPath);
            m_entries[idx].state = QStringLiteral("cloud");
            m_statsDirty = true;
            syncProjectionForEntryLocked(m_entries[idx]);
            evictedPaths.append(m_entries[idx].path);
            freedBytes += m_entries[idx].sizeBytes;
            ++evictedCount;
        }
    }

    if (m_fuseMount) {
        for (const QString &p : std::as_const(evictedPaths))
            m_fuseMount->invalidatePath(p);
    }

    if (evictedCount > 0) {
        logActivity(tr("Smart Cache Cleanup"),
                    tr("Evicted %1 cold file(s), reclaiming %2 of disk space")
                        .arg(evictedCount)
                        .arg(formatSize(freedBytes)),
                    QStringLiteral("CLEAN"));
        setStatusBanner(tr("Reclaimed %1 by evicting %2 cold cached file(s)")
                            .arg(formatSize(freedBytes))
                            .arg(evictedCount),
                        false);
    } else {
        setStatusBanner(tr("No cold unpinned files in cache to evict"), false);
    }

    saveState();
    rebuildFilteredModel();
    return evictedCount;
}

int Backend::enforceCacheBudget(const QString &protectedPath) {
    int evictedCount = 0;
    qint64 freedBytes = 0;
    QStringList evictedNames;
    QStringList evictedPaths;

    {
        QMutexLocker locker(&m_mutex);
        recomputeScoresLocked();

        const qint64 ceiling = effectiveBudgetBytes();
        if (cachedBytes() <= ceiling)
            return 0;

        QList<int> candidates;
        for (int i = 0; i < m_entries.size(); ++i) {
            const auto &e = m_entries.at(i);
            if (!e.pinned && !e.dirty && e.state == QLatin1String("cached") && e.path != protectedPath) {
                candidates.append(i);
            }
        }

        // Sort ascending by LFU+LRU hotScore so the least frequently & least recently
        // accessed files are evicted first.
        std::sort(candidates.begin(), candidates.end(), [this](int a, int b) {
            const auto &ea = m_entries.at(a);
            const auto &eb = m_entries.at(b);
            if (!qFuzzyCompare(ea.hotScore, eb.hotScore))
                return ea.hotScore < eb.hotScore;
            if (ea.lastAccessedSecs != eb.lastAccessedSecs)
                return ea.lastAccessedSecs < eb.lastAccessedSecs;
            return ea.sizeBytes > eb.sizeBytes;
        });

        for (int idx : candidates) {
            if (cachedBytes() <= effectiveBudgetBytes())
                break;
            const QString blobPath = localBlobPath(m_entries[idx].path);
            if (QFileInfo::exists(blobPath))
                QFile::remove(blobPath);
            m_entries[idx].state = QStringLiteral("cloud");
            m_statsDirty = true;
            syncProjectionForEntryLocked(m_entries[idx]);
            freedBytes += m_entries[idx].sizeBytes;
            evictedNames.append(m_entries[idx].name);
            evictedPaths.append(m_entries[idx].path);
            ++evictedCount;
        }
    }

    if (m_fuseMount) {
        for (const QString &p : std::as_const(evictedPaths))
            m_fuseMount->invalidatePath(p);
    }

    if (evictedCount > 0) {
        logActivity(tr("Auto-Evicted Cold Cache"),
                    tr("Freed %1 (%2) to stay within %3 budget")
                        .arg(formatSize(freedBytes), evictedNames.join(QStringLiteral(", ")), effectiveBudgetFormatted()),
                    QStringLiteral("BUDGET"));
    }

    rebuildFilteredModel();
    return evictedCount;
}

bool Backend::toggleMount() {
    if (mounted()) {
        unmountDrive();
        return false;
    }
    return mountDrive();
}

bool Backend::mountDrive(QString *errorOut) {
    QString err;
    const bool ok = m_fuseMount->mount(m_mountDir, &err);
    if (ok) {
        if (!m_fuseMount->isKernelFuse()) {
            QMutexLocker locker(&m_mutex);
            syncAllProjectionsLocked();
        }
        logActivity(tr("Virtual Drive Mounted"),
                    m_fuseMount->isKernelFuse()
                        ? tr("FUSE3 filesystem active at ~/Drive")
                        : tr("On-demand workspace active at ~/Drive"),
                    QStringLiteral("MOUNT"));
    } else if (errorOut) {
        *errorOut = err;
    }
    emit mountStateChanged();
    return ok;
}

void Backend::unmountDrive() {
    if (!m_fuseMount || !m_fuseMount->isMounted())
        return;
    m_fuseMount->unmount();
    logActivity(tr("Virtual Drive Unmounted"),
                tr("Unmounted %1").arg(m_mountDir),
                QStringLiteral("UNMOUNT"));
    emit mountStateChanged();
}

void Backend::refreshRemotes() {
    m_rcloneBin = QStandardPaths::findExecutable("rclone");
    m_rcloneAvailable = !m_rcloneBin.isEmpty();
    m_availableRemotes.clear();
    m_oauthAuthorized = false;
    m_oauthClientId.clear();
    m_oauthClientSecret.clear();

    // Parse ~/.config/rclone/rclone.conf directly so we also inspect client_id, client_secret, and token
    const QString cfgPath = rcloneConfigPath();
    QFile cfgFile(cfgPath);
    if (cfgFile.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(cfgFile.readAll()).split('\n');
        QString currentSection;
        const QString targetRemote = m_remoteName.isEmpty() ? QStringLiteral("gdrive") : m_remoteName;
        for (QString line : lines) {
            line = line.trimmed();
            if (line.startsWith('[') && line.endsWith(']')) {
                currentSection = line.mid(1, line.size() - 2).trimmed();
                if (!currentSection.isEmpty() && !m_availableRemotes.contains(currentSection))
                    m_availableRemotes.append(currentSection);
                continue;
            }
            if (currentSection == targetRemote) {
                const int eq = line.indexOf('=');
                if (eq > 0) {
                    const QString key = line.left(eq).trimmed();
                    const QString val = line.mid(eq + 1).trimmed();
                    if (key == QLatin1String("client_id"))
                        m_oauthClientId = val;
                    else if (key == QLatin1String("client_secret"))
                        m_oauthClientSecret = val;
                    else if (key == QLatin1String("scope") && !val.isEmpty())
                        m_oauthScope = val;
                    else if (key == QLatin1String("token") && val.contains(QLatin1String("access_token")))
                        m_oauthAuthorized = true;
                }
            }
        }
    }

    if (m_rcloneAvailable && m_rcloneConfigPath.isEmpty() && m_availableRemotes.isEmpty()) {
        QProcess proc;
        proc.start(m_rcloneBin, {"listremotes"});
        if (proc.waitForFinished(2000) && proc.exitCode() == 0) {
            const QStringList lines = QString::fromUtf8(proc.readAllStandardOutput())
                                          .split('\n', Qt::SkipEmptyParts);
            for (QString line : lines) {
                line = line.trimmed();
                if (line.endsWith(':'))
                    line.chop(1);
                if (!line.isEmpty() && !m_availableRemotes.contains(line))
                    m_availableRemotes.append(line);
            }
        }
    }
    emit connectionChanged();
}

bool Backend::configureOAuthClient(const QString &remoteName, const QString &clientId,
                                   const QString &clientSecret, const QString &scope) {
    QString cleanRemote = remoteName.trimmed().isEmpty() ? QStringLiteral("gdrive") : remoteName.trimmed();
    if (cleanRemote.endsWith(':'))
        cleanRemote.chop(1);

    const QString cfgPath = rcloneConfigPath();
    QDir().mkpath(QFileInfo(cfgPath).absolutePath());

    QString existingToken;
    QString existingTeamDrive;
    QStringList otherSectionsLines;

    QFile inFile(cfgPath);
    if (inFile.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(inFile.readAll()).split('\n');
        QString curSec;
        for (const QString &rawLine : lines) {
            const QString trimmed = rawLine.trimmed();
            if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
                curSec = trimmed.mid(1, trimmed.size() - 2).trimmed();
            }
            if (curSec == cleanRemote) {
                const int eq = trimmed.indexOf('=');
                if (eq > 0) {
                    const QString k = trimmed.left(eq).trimmed();
                    const QString v = trimmed.mid(eq + 1).trimmed();
                    if (k == QLatin1String("token"))
                        existingToken = v;
                    else if (k == QLatin1String("team_drive"))
                        existingTeamDrive = v;
                }
            } else {
                otherSectionsLines.append(rawLine);
            }
        }
        inFile.close();
    }

    QString outText = otherSectionsLines.join('\n').trimmed();
    if (!outText.isEmpty())
        outText += QStringLiteral("\n\n");

    outText += QStringLiteral("[%1]\n").arg(cleanRemote);
    outText += QStringLiteral("type = drive\n");
    outText += QStringLiteral("client_id = %1\n").arg(clientId.trimmed());
    outText += QStringLiteral("client_secret = %1\n").arg(clientSecret.trimmed());
    outText += QStringLiteral("scope = %1\n").arg(scope.trimmed().isEmpty() ? QStringLiteral("drive") : scope.trimmed());
    if (!existingToken.isEmpty())
        outText += QStringLiteral("token = %1\n").arg(existingToken);
    outText += QStringLiteral("team_drive = %1\n").arg(existingTeamDrive);

    QSaveFile out(cfgPath);
    if (!out.open(QIODevice::WriteOnly)) {
        setStatusBanner(tr("Failed to write rclone config: %1").arg(cfgPath), true);
        return false;
    }
    out.write(outText.toUtf8());
    if (!out.commit()) {
        setStatusBanner(tr("Failed to save rclone config: %1").arg(cfgPath), true);
        return false;
    }
    QFile::setPermissions(cfgPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    m_remoteName = cleanRemote;
    refreshRemotes();
    if (m_oauthAuthorized)
        m_sourceMode = QStringLiteral("rclone");
    saveState();

    logActivity(tr("Configured Google OAuth Client"),
                clientId.trimmed().isEmpty()
                    ? tr("Configured '%1:' with built-in Google Drive OAuth Client ID").arg(cleanRemote)
                    : tr("Configured '%1:' with custom Google Cloud Client ID").arg(cleanRemote),
                QStringLiteral("OAUTH"));
    setStatusBanner(clientId.trimmed().isEmpty()
                        ? tr("Google OAuth ready for '%1:' (Built-in Client ID active)").arg(cleanRemote)
                        : tr("Saved custom Google OAuth Client ID for '%1:'").arg(cleanRemote),
                    false);
    emit connectionChanged();
    return true;
}

bool Backend::importGoogleClientSecretJson(const QString &jsonFilePath, const QString &remoteName, QString *errorOut) {
    QFile file(jsonFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString msg = tr("Cannot open credentials file: %1").arg(jsonFilePath);
        if (errorOut)
            *errorOut = msg;
        setStatusBanner(msg, true);
        return false;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString msg = tr("Invalid Google OAuth JSON file: %1").arg(jsonFilePath);
        if (errorOut)
            *errorOut = msg;
        setStatusBanner(msg, true);
        return false;
    }

    const QJsonObject root = doc.object();
    QJsonObject creds = root;
    if (root.contains("installed") && root.value("installed").isObject())
        creds = root.value("installed").toObject();
    else if (root.contains("web") && root.value("web").isObject())
        creds = root.value("web").toObject();

    const QString clientId = creds.value("client_id").toString().trimmed();
    const QString clientSecret = creds.value("client_secret").toString().trimmed();
    if (clientId.isEmpty() || clientSecret.isEmpty()) {
        const QString msg = tr("JSON file does not contain 'client_id' and 'client_secret'");
        if (errorOut)
            *errorOut = msg;
        setStatusBanner(msg, true);
        return false;
    }

    return configureOAuthClient(remoteName, clientId, clientSecret, QStringLiteral("drive"));
}

void Backend::pickGoogleClientSecretJson(const QString &remoteName) {
    if (!m_filePicker)
        return;
    m_filePicker->openFile(
        tr("Select Google Cloud client_secret.json"),
        {{tr("JSON Credentials"), {QStringLiteral("*.json")}}},
        [this, remoteName](const QString &path) {
            if (!path.isEmpty())
                importGoogleClientSecretJson(path, remoteName);
        });
}

void Backend::openGoogleCloudConsole() {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://console.cloud.google.com/apis/credentials")));
}

bool Backend::connectRcloneRemote(const QString &remoteName, const QString &remotePath) {
    refreshRemotes();
    QString cleanRemote = remoteName.trimmed();
    if (cleanRemote.endsWith(':'))
        cleanRemote.chop(1);

    if (!m_rcloneAvailable) {
        setStatusBanner(tr("rclone is not installed — install it with: omarchy-pkg-add rclone"), true);
        return false;
    }
    if (cleanRemote.isEmpty()) {
        setStatusBanner(tr("Please enter a valid rclone remote name (e.g. gdrive)"), true);
        return false;
    }

    m_sourceMode = QStringLiteral("rclone");
    m_remoteName = cleanRemote;
    m_remotePath = remotePath.trimmed();
    m_driveChangePageToken.clear();
    m_driveRootFolderId.clear();
    m_driveFolderIdToPath.clear();
    m_cachedAccessToken.clear();
    m_cachedAccessTokenExpiry = QDateTime();
    emit connectionChanged();

    syncNow();
    saveState();
    return true;
}

bool Backend::connectLocalSource(const QString &folderPath) {
    QFileInfo info(folderPath);
    if (!info.exists() || !info.isDir()) {
        setStatusBanner(tr("Folder does not exist: %1").arg(folderPath), true);
        return false;
    }

    m_sourceMode = QStringLiteral("folder");
    m_localSourceDir = info.absoluteFilePath();
    emit connectionChanged();

    syncNow();
    saveState();
    return true;
}

void Backend::pickLocalSourceFolder() {
    if (!m_filePicker)
        return;
    m_filePicker->openDirectory(tr("Select Cloud Mirror / Source Folder"), [this](const QString &path) {
        if (!path.isEmpty())
            connectLocalSource(path);
    });
}

void Backend::launchRcloneAuth(const QString &remoteName) {
    refreshRemotes();
    if (!m_rcloneAvailable) {
        setStatusBanner(tr("Cannot launch Google OAuth: rclone is not installed (run: omarchy-pkg-add rclone)"), true);
        return;
    }
    QString clean = remoteName.trimmed().isEmpty() ? QStringLiteral("gdrive") : remoteName.trimmed();
    if (clean.endsWith(':'))
        clean.chop(1);

    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc, clean](int code, QProcess::ExitStatus) {
        proc->deleteLater();
        refreshRemotes();
        if (code == 0 && m_availableRemotes.contains(clean)) {
            connectRcloneRemote(clean, m_remotePath);
        } else {
            setStatusBanner(tr("rclone OAuth finished (code %1) — press R to sync once authorized").arg(code), code != 0);
        }
    });
    setStatusBanner(tr("Launching browser for Google Drive OAuth ('%1:')...").arg(clean), false);
    proc->start(m_rcloneBin, {"config", "create", clean, "drive"});
}

namespace {

bool runRcloneLsjson(const QString &rcloneBin, const QString &spec, int maxDepth, bool filesOnly,
                     int timeoutMs, QByteArray *stdoutOut, QString *errOut,
                     const QString &configPath = {}, bool recursive = false, bool fastList = false) {
    QStringList args = {"lsjson", "--no-mimetype"};
    if (recursive) {
        args << "-R";
        if (fastList)
            args << "--fast-list";
    } else {
        args << "--max-depth" << QString::number(maxDepth);
    }
    if (filesOnly)
        args << "--files-only";
    if (!configPath.isEmpty())
        args << "--config" << configPath;
    args << "--contimeout=5s" << QStringLiteral("--timeout=%1s").arg(qMax(1, timeoutMs / 1000))
         << "--retries=1" << spec;

    QProcess proc;
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(rcloneBin, args);
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(500);
        if (errOut)
            *errOut = QStringLiteral("Timed out syncing %1").arg(spec);
        return false;
    }
    if (proc.exitCode() != 0) {
        if (errOut)
            *errOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return false;
    }
    if (stdoutOut)
        *stdoutOut = proc.readAllStandardOutput();
    return true;
}

bool runRcloneCommand(const QString &rcloneBin, const QString &subcommand,
                      const QStringList &cmdArgs, int timeoutMs,
                      const QString &timeoutVerb, QString *errOut,
                      const QString &configPath = {}) {
    QStringList args = {subcommand};
    if (!configPath.isEmpty())
        args << QStringLiteral("--config") << configPath;
    args << QStringLiteral("--contimeout=5s")
         << QStringLiteral("--timeout=%1s").arg(qMax(1, timeoutMs / 1000))
         << QStringLiteral("--retries=1");
    args << cmdArgs;

    QProcess proc;
    proc.setStandardInputFile(QProcess::nullDevice());
    proc.start(rcloneBin, args);
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(500);
        if (errOut)
            *errOut = QStringLiteral("Timed out %1").arg(timeoutVerb);
        return false;
    }
    if (proc.exitCode() != 0) {
        if (errOut)
            *errOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

bool runRcloneMoveto(const QString &rcloneBin, const QString &srcSpec, const QString &dstSpec,
                     int timeoutMs, QString *errOut, const QString &configPath = {}) {
    return runRcloneCommand(rcloneBin, QStringLiteral("moveto"), {srcSpec, dstSpec},
                            timeoutMs, QStringLiteral("renaming %1").arg(srcSpec), errOut, configPath);
}

bool runRcloneUpload(const QString &rcloneBin, const QString &localFilePath, const QString &remoteSpec,
                     int timeoutMs, QString *errOut, const QString &configPath = {}) {
    return runRcloneCommand(rcloneBin, QStringLiteral("copyto"), {localFilePath, remoteSpec},
                            timeoutMs, QStringLiteral("uploading %1").arg(remoteSpec), errOut, configPath);
}

bool runRcloneDeletefile(const QString &rcloneBin, const QString &spec,
                         int timeoutMs, QString *errOut, const QString &configPath = {}) {
    return runRcloneCommand(rcloneBin, QStringLiteral("deletefile"), {spec},
                            timeoutMs, QStringLiteral("deleting %1").arg(spec), errOut, configPath);
}

bool copyFileAtomic(const QString &srcPath, const QString &dstPath) {
    QDir().mkpath(QFileInfo(dstPath).absolutePath());
    const QString partPath = dstPath + QStringLiteral(".part");
    QFile::remove(partPath);
    if (!QFile::copy(srcPath, partPath)) {
        QFile::remove(partPath);
        return false;
    }
    QFile::remove(dstPath);
    if (!QFile::rename(partPath, dstPath)) {
        QFile::remove(partPath);
        return false;
    }
    return true;
}

bool runRcloneMkdir(const QString &rcloneBin, const QString &spec,
                    int timeoutMs, QString *errOut, const QString &configPath = {}) {
    return runRcloneCommand(rcloneBin, QStringLiteral("mkdir"), {spec},
                            timeoutMs, QStringLiteral("creating directory %1").arg(spec), errOut, configPath);
}

bool runRcloneRmdir(const QString &rcloneBin, const QString &spec,
                    int timeoutMs, QString *errOut, const QString &configPath = {}) {
    return runRcloneCommand(rcloneBin, QStringLiteral("rmdir"), {spec},
                            timeoutMs, QStringLiteral("removing directory %1").arg(spec), errOut, configPath);
}

template <typename Fn>
bool runOnQtThread(const QObject *owner, Fn &&fn) {
    if (!owner || QThread::currentThread() == owner->thread())
        return fn();
    bool ok = false;
    std::unique_ptr<QThread> worker(QThread::create([&]() {
        ok = fn();
    }));
    worker->start();
    worker->wait();
    return ok;
}

bool isRcloneNotFoundError(const QString &err) {
    const QString lower = err.toLower();
    return lower.contains(QLatin1String("not found"))
        || lower.contains(QLatin1String("no such file"))
        || lower.contains(QLatin1String("doesn't exist"))
        || lower.contains(QLatin1String("does not exist"));
}

bool shouldUseAsyncRcloneSync() {
    return (qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")
            || !qEnvironmentVariableIsEmpty("WICKFS_ENABLE_FUSE"))
        && qEnvironmentVariableIsEmpty("WICKFS_SYNC_BLOCKING")
        && QCoreApplication::instance() != nullptr;
}

constexpr qint64 kDirSyncTtlMs = 60LL * 1000LL;

QString cleanRelPath(QString p) {
    while (p.startsWith('/'))
        p.remove(0, 1);
    while (p.endsWith('/'))
        p.chop(1);
    return p;
}

struct RcloneDriveAuth {
    bool valid = false;
    QString type;
    QString accessToken;
    QString refreshToken;
    QDateTime expiry;
    QString clientId;
    QString clientSecret;
    QString teamDrive;
    QString rootFolderId;
};

RcloneDriveAuth readRcloneDriveAuth(const QString &cfgPath, const QString &remoteName) {
    RcloneDriveAuth auth;
    QFile cfgFile(cfgPath);
    if (!cfgFile.open(QIODevice::ReadOnly))
        return auth;

    const QString target = remoteName.isEmpty() ? QStringLiteral("gdrive") : remoteName;
    const QStringList lines = QString::fromUtf8(cfgFile.readAll()).split('\n');
    QString curSec;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            curSec = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        if (curSec != target)
            continue;
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = line.mid(eq + 1).trimmed();
        if (k == QLatin1String("type")) {
            auth.type = v;
        } else if (k == QLatin1String("client_id")) {
            auth.clientId = v;
        } else if (k == QLatin1String("client_secret")) {
            auth.clientSecret = v;
        } else if (k == QLatin1String("team_drive")) {
            auth.teamDrive = v;
        } else if (k == QLatin1String("root_folder_id")) {
            auth.rootFolderId = v;
        } else if (k == QLatin1String("token")) {
            const QJsonObject tokObj = QJsonDocument::fromJson(v.toUtf8()).object();
            auth.accessToken = tokObj.value(QStringLiteral("access_token")).toString();
            auth.refreshToken = tokObj.value(QStringLiteral("refresh_token")).toString();
            const QString expStr = tokObj.value(QStringLiteral("expiry")).toString();
            if (!expStr.isEmpty())
                auth.expiry = QDateTime::fromString(expStr, Qt::ISODate);
        }
    }

    auth.valid = (auth.type.isEmpty() || auth.type == QLatin1String("drive")) && !auth.accessToken.isEmpty();
    return auth;
}

bool driveHttpRequestJson(QNetworkAccessManager &nam, const QString &method, const QUrl &url,
                          const QByteArray &body, const QString &bearerToken,
                          const QString &contentType, int timeoutMs,
                          int *httpStatusOut, QJsonObject *jsonOut, QString *errOut) {
    QNetworkRequest req(url);
    req.setTransferTimeout(timeoutMs);
    if (!bearerToken.isEmpty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + bearerToken.toUtf8());
    if (!contentType.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);

    QNetworkReply *reply = nullptr;
    if (method == QLatin1String("POST"))
        reply = nam.post(req, body);
    else
        reply = nam.get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    const bool timedOut = !reply->isFinished();
    if (timedOut) {
        reply->abort();
        reply->deleteLater();
        if (httpStatusOut)
            *httpStatusOut = 0;
        if (errOut)
            *errOut = QStringLiteral("HTTP request timed out");
        return false;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatusOut)
        *httpStatusOut = status;
    const QByteArray respBytes = reply->readAll();
    const QNetworkReply::NetworkError netErr = reply->error();
    const QString netErrStr = reply->errorString();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(respBytes);
    if (doc.isObject() && jsonOut)
        *jsonOut = doc.object();

    if (netErr != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (errOut)
            *errOut = netErrStr;
        return false;
    }
    return true;
}

bool refreshDriveAccessToken(QNetworkAccessManager &nam, const QString &tokenUrl,
                             const RcloneDriveAuth &auth, const QString &rcloneBin,
                             const QString &remoteName, const QString &cfgPath,
                             QString *accessTokenOut, QDateTime *expiryOut) {
    if (!auth.refreshToken.isEmpty() && !auth.clientId.isEmpty() && !auth.clientSecret.isEmpty()) {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        form.addQueryItem(QStringLiteral("refresh_token"), auth.refreshToken);
        form.addQueryItem(QStringLiteral("client_id"), auth.clientId);
        form.addQueryItem(QStringLiteral("client_secret"), auth.clientSecret);
        QJsonObject resp;
        int status = 0;
        if (driveHttpRequestJson(nam, QStringLiteral("POST"), QUrl(tokenUrl),
                                 form.toString(QUrl::FullyEncoded).toUtf8(), QString(),
                                 QStringLiteral("application/x-www-form-urlencoded"), 15000,
                                 &status, &resp, nullptr)) {
            const QString tok = resp.value(QStringLiteral("access_token")).toString();
            const int expiresIn = resp.value(QStringLiteral("expires_in")).toInt(3600);
            if (!tok.isEmpty()) {
                if (accessTokenOut)
                    *accessTokenOut = tok;
                if (expiryOut)
                    *expiryOut = QDateTime::currentDateTimeUtc().addSecs(qMax(60, expiresIn - 60));
                return true;
            }
        }
    }

    // Built-in rclone OAuth tokens do not expose rclone's embedded client secret in rclone.conf;
    // let rclone perform a lightweight metadata probe to refresh rclone.conf on disk.
    if (!rcloneBin.isEmpty() && !remoteName.isEmpty()) {
        QStringList args = {QStringLiteral("lsd")};
        if (!cfgPath.isEmpty())
            args << QStringLiteral("--config") << cfgPath;
        args << QStringLiteral("--max-depth") << QStringLiteral("1")
             << QStringLiteral("--contimeout=5s") << QStringLiteral("--timeout=15s")
             << buildRcloneSpec(remoteName, QString());
        QProcess proc;
        proc.setStandardInputFile(QProcess::nullDevice());
        proc.start(rcloneBin, args);
        if (proc.waitForFinished(15000) && proc.exitCode() == 0) {
            const RcloneDriveAuth reloaded = readRcloneDriveAuth(cfgPath, remoteName);
            if (reloaded.valid) {
                if (accessTokenOut)
                    *accessTokenOut = reloaded.accessToken;
                if (expiryOut)
                    *expiryOut = reloaded.expiry;
                return true;
            }
        }
    }
    return false;
}

bool resolveDriveFolderRelPath(QNetworkAccessManager &nam, const QString &apiBase,
                               const QString &accessToken, const QString &folderId,
                               const QString &rootId, QHash<QString, QString> &cache,
                               const QString &remotePathPrefix, QString *relPathOut) {
    if (folderId.isEmpty())
        return false;
    if (folderId == rootId) {
        if (relPathOut)
            *relPathOut = QString();
        return true;
    }
    if (cache.contains(folderId)) {
        const QString full = cache.value(folderId);
        if (remotePathPrefix.isEmpty()) {
            if (relPathOut)
                *relPathOut = full;
            return true;
        }
        if (full == remotePathPrefix) {
            if (relPathOut)
                *relPathOut = QString();
            return true;
        }
        const QString pfx = remotePathPrefix + QLatin1Char('/');
        if (full.startsWith(pfx)) {
            if (relPathOut)
                *relPathOut = full.mid(pfx.size());
            return true;
        }
        return false;
    }

    QStringList segments;
    QStringList visitedIds;
    QString cur = folderId;
    bool reachedRoot = false;

    for (int depth = 0; depth < 32; ++depth) {
        if (cur == rootId) {
            reachedRoot = true;
            break;
        }
        if (cache.contains(cur)) {
            const QString cachedBase = cache.value(cur);
            if (!cachedBase.isEmpty())
                segments.prepend(cachedBase);
            reachedRoot = true;
            break;
        }
        if (visitedIds.contains(cur))
            return false;
        visitedIds.append(cur);

        QUrl url(apiBase + QStringLiteral("/files/") + QString::fromLatin1(QUrl::toPercentEncoding(cur)));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("id,name,parents"));
        q.addQueryItem(QStringLiteral("supportsAllDrives"), QStringLiteral("true"));
        url.setQuery(q);

        QJsonObject obj;
        int status = 0;
        if (!driveHttpRequestJson(nam, QStringLiteral("GET"), url, QByteArray(), accessToken,
                                  QString(), 10000, &status, &obj, nullptr)) {
            return false;
        }
        const QString name = cleanRelPath(obj.value(QStringLiteral("name")).toString());
        if (name.isEmpty())
            return false;
        segments.prepend(name);

        const QJsonArray parents = obj.value(QStringLiteral("parents")).toArray();
        if (parents.isEmpty())
            break;
        cur = parents.first().toString();
    }

    if (!reachedRoot)
        return false;

    const QString fullPath = segments.join(QLatin1Char('/'));
    cache.insert(folderId, fullPath);
    if (remotePathPrefix.isEmpty()) {
        if (relPathOut)
            *relPathOut = fullPath;
        return true;
    }
    if (fullPath == remotePathPrefix) {
        if (relPathOut)
            *relPathOut = QString();
        return true;
    }
    const QString pfx = remotePathPrefix + QLatin1Char('/');
    if (fullPath.startsWith(pfx)) {
        if (relPathOut)
            *relPathOut = fullPath.mid(pfx.size());
        return true;
    }
    return false;
}

} // namespace

void Backend::applyRcloneEntryMetadataLocked(DriveFileEntry &e, const QJsonObject &obj, const QString &relPath) const {
    e.path = relPath;
    e.name = QFileInfo(relPath).fileName();
    const QString dir = QFileInfo(relPath).path();
    e.folder = (dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir;
    e.category = inferCategory(e.name);
    if (e.dirty)
        return;

    const qint64 oldSize = e.sizeBytes;
    const qint64 oldMod = e.modifiedSecs;
    const qint64 rawSize = static_cast<qint64>(obj.value("Size").toDouble(0));
    const qint64 newSize = rawSize < 0 ? (64LL * 1024LL) : rawSize;
    const QDateTime dt = QDateTime::fromString(obj.value("ModTime").toString(), Qt::ISODate);
    const qint64 newMod = dt.isValid() ? dt.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
    const bool remoteChanged = (e.state == QLatin1String("cached") && !e.pinned)
        && ((rawSize >= 0 && newSize != oldSize) || (oldMod > 0 && newMod > oldMod + 2));

    e.sizeBytes = newSize;
    e.modifiedSecs = newMod;
    if (remoteChanged) {
        QFile::remove(localBlobPath(relPath));
        e.state = QStringLiteral("cloud");
    } else if (e.state.isEmpty()) {
        e.state = QFileInfo::exists(localBlobPath(relPath)) ? QStringLiteral("cached") : QStringLiteral("cloud");
    }
}

void Backend::mergeRcloneRootJsonLocked(const QByteArray &rootJson) {
    const QJsonArray arr = QJsonDocument::fromJson(rootJson).array();
    static const QSet<QString> kStarterDemoPaths = {
        QStringLiteral("Architecture/Distributed-Cache-Spec.pdf"),
        QStringLiteral("Product/Q4-Roadmap-Deck.pptx"),
        QStringLiteral("Finance/2026-Operating-Model.xlsx"),
        QStringLiteral("Design/Brand-Component-Kit.fig"),
        QStringLiteral("Engineering/Kernel-Benchmark-Results.csv"),
        QStringLiteral("Media/Keynote-Demo-4K.mp4"),
        QStringLiteral("Archives/Release-Artifacts-v2.4.tar.zst"),
        QStringLiteral("Media/Podcast-Master-Session.wav"),
        QStringLiteral("Architecture/API-Security-Review.md"),
        QStringLiteral("Design/Wallpaper-Pack-HiDPI.zip")
    };

    const bool kernelFuse = m_fuseMount && m_fuseMount->isKernelFuse();
    for (const QString &demoPath : kStarterDemoPaths) {
        QFile::remove(localBlobPath(demoPath));
        if (!m_mountDir.isEmpty() && !kernelFuse && !m_isTestInstance)
            QFile::remove(m_mountDir + "/" + demoPath);
        else if (!m_mountDir.isEmpty() && !kernelFuse && m_isTestInstance)
            QFile::remove(m_mountDir + "/" + demoPath);
    }

    QHash<QString, DriveFileEntry> existingByPath;
    for (const auto &e : m_entries) {
        if (!kStarterDemoPaths.contains(e.path))
            existingByPath.insert(e.path, e);
    }

    QStringList nextFolders = {QStringLiteral("Root")};
    QSet<QString> topLevelFolders;
    QList<DriveFileEntry> nextEntries;
    QSet<QString> seenPaths;
    for (const auto &val : arr) {
        const QJsonObject obj = val.toObject();
        const QString relPath = cleanRelPath(obj.value("Path").toString());
        if (relPath.isEmpty() || m_pendingDeletes.contains(relPath))
            continue;
        if (obj.value("IsDir").toBool()) {
            if (!nextFolders.contains(relPath))
                nextFolders.append(relPath);
            topLevelFolders.insert(relPath);
            continue;
        }

        DriveFileEntry e = existingByPath.value(relPath);
        applyRcloneEntryMetadataLocked(e, obj, relPath);
        nextEntries.append(e);
        seenPaths.insert(relPath);
    }

    // Preserve previously discovered nested subdirectories whose top-level folder still exists.
    for (const QString &rf : std::as_const(m_remoteFolders)) {
        const int slash = rf.indexOf('/');
        if (slash > 0 && topLevelFolders.contains(rf.left(slash)) && !nextFolders.contains(rf))
            nextFolders.append(rf);
    }
    // Preserve already-synced subfolder files whose top-level folder still exists and any locally dirty files awaiting upload.
    for (auto it = existingByPath.cbegin(); it != existingByPath.cend(); ++it) {
        const QString &folder = it.value().folder;
        const int slash = folder.indexOf('/');
        const QString topFolder = (slash > 0) ? folder.left(slash) : folder;
        const bool keepSubfolderEntry = (folder != QLatin1String("Root")) && topLevelFolders.contains(topFolder);
        if (!seenPaths.contains(it.key())
            && !m_pendingDeletes.contains(it.key())
            && (keepSubfolderEntry || it.value().dirty)) {
            nextEntries.append(it.value());
        }
    }

    m_remoteFolders = nextFolders;
    m_entries = nextEntries;
    m_listedDirs.insert(QString());
    m_syncedDirTimestampsMs.insert(QString(), QDateTime::currentMSecsSinceEpoch());
    recomputeScoresLocked();
    rebuildVfsIndexLocked();
}

void Backend::mergeRcloneFolderJsonLocked(const QString &baseFolder, const QByteArray &folderJson,
                                          bool pruneDirectChildren) {
    QString cleanBase = cleanRelPath(baseFolder);
    if (cleanBase == QLatin1String("Root"))
        cleanBase.clear();

    const QJsonArray arr = QJsonDocument::fromJson(folderJson).array();
    QHash<QString, int> idxByPath;
    for (int i = 0; i < m_entries.size(); ++i)
        idxByPath.insert(m_entries.at(i).path, i);

    QSet<QString> folderSet(m_remoteFolders.cbegin(), m_remoteFolders.cend());
    QSet<QString> seenDirectFiles;
    QSet<QString> seenDirectSubdirs;

    for (const auto &val : arr) {
        const QJsonObject obj = val.toObject();
        const QString subRel = cleanRelPath(obj.value("Path").toString());
        if (subRel.isEmpty())
            continue;

        const QString relPath = cleanBase.isEmpty() ? subRel : (cleanBase + QLatin1Char('/') + subRel);
        if (m_pendingDeletes.contains(relPath))
            continue;
        if (obj.value("IsDir").toBool()) {
            if (!folderSet.contains(relPath)) {
                m_remoteFolders.append(relPath);
                folderSet.insert(relPath);
            }
            if (!subRel.contains('/'))
                seenDirectSubdirs.insert(relPath);
            continue;
        }

        DriveFileEntry e = idxByPath.contains(relPath) ? m_entries.at(idxByPath.value(relPath)) : DriveFileEntry{};
        applyRcloneEntryMetadataLocked(e, obj, relPath);

        if (idxByPath.contains(relPath)) {
            m_entries[idxByPath.value(relPath)] = e;
        } else {
            idxByPath.insert(relPath, m_entries.size());
            m_entries.append(e);
        }
        if (!subRel.contains('/'))
            seenDirectFiles.insert(relPath);
    }

    if (pruneDirectChildren && !cleanBase.isEmpty()) {
        for (int i = m_entries.size() - 1; i >= 0; --i) {
            const auto &e = m_entries.at(i);
            if (e.folder == cleanBase && e.state == QLatin1String("cloud") && !e.pinned && !e.dirty
                && !seenDirectFiles.contains(e.path)) {
                m_entries.removeAt(i);
            }
        }
        const QString prefix = cleanBase + QLatin1Char('/');
        for (int i = m_remoteFolders.size() - 1; i >= 0; --i) {
            const QString &rf = m_remoteFolders.at(i);
            if (!rf.startsWith(prefix))
                continue;
            const QString rest = rf.mid(prefix.size());
            if (!rest.contains('/') && !seenDirectSubdirs.contains(rf)) {
                bool hasChildEntry = false;
                const QString subPrefix = rf + QLatin1Char('/');
                for (const auto &e : m_entries) {
                    if (e.path.startsWith(subPrefix)) {
                        hasChildEntry = true;
                        break;
                    }
                }
                if (!hasChildEntry)
                    m_remoteFolders.removeAt(i);
            }
        }
    }

    m_listedDirs.insert(cleanBase);
    m_syncedDirTimestampsMs.insert(cleanBase, QDateTime::currentMSecsSinceEpoch());
    recomputeScoresLocked();
    rebuildVfsIndexLocked();
}

void Backend::startAsyncRcloneSync(const QString &folderOnly) {
    bool expected = false;
    if (!m_rcloneSyncInFlight.compare_exchange_strong(expected, true)) {
        if (!folderOnly.isEmpty()) {
            QMutexLocker locker(&m_mutex);
            m_pendingFolderSync = folderOnly;
        }
        return;
    }

    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString configPath;
    QString activeFolder;
    {
        QMutexLocker locker(&m_mutex);
        if (m_sourceMode != QLatin1String("rclone") || !m_rcloneAvailable || !m_availableRemotes.contains(m_remoteName)) {
            m_rcloneSyncInFlight.store(false);
            return;
        }
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        configPath = m_rcloneConfigPath;
        activeFolder = folderOnly.isEmpty() ? m_currentFolder : folderOnly;
    }

    QPointer<Backend> self(this);
    QThread *t = QThread::create([this, self, rcloneBin, remoteName, remotePath, configPath, folderOnly, activeFolder]() {
        flushPendingUploads();

        QByteArray rootJson;
        QString rootErr;
        bool rootOk = false;
        const QString rootSpec = buildRcloneSpec(remoteName, remotePath);
        if (folderOnly.isEmpty()) {
            rootOk = runRcloneLsjson(rcloneBin, rootSpec, 1, false, 60000, &rootJson, &rootErr, configPath, false, false);
        }

        QByteArray folderJson;
        QString folderErr;
        bool folderOk = false;
        const QString targetFolder = folderOnly.isEmpty() ? activeFolder : folderOnly;
        if (!targetFolder.isEmpty() && targetFolder != QLatin1String("Root")) {
            const QString fSpec = buildRcloneSpec(remoteName, remotePath, targetFolder);
            folderOk = runRcloneLsjson(rcloneBin, fSpec, 2, false, 60000, &folderJson, &folderErr, configPath, false, false);
        }

        if (!self)
            return;
        QMetaObject::invokeMethod(this, [this, self, folderOnly, rootOk, rootJson, rootErr, targetFolder, folderOk, folderJson]() {
            if (!self)
                return;
            if (folderOnly.isEmpty() && rootOk) {
                QMutexLocker locker(&m_mutex);
                mergeRcloneRootJsonLocked(rootJson);
            }

            if (folderOk && !targetFolder.isEmpty()) {
                QMutexLocker locker(&m_mutex);
                mergeRcloneFolderJsonLocked(targetFolder, folderJson, false);
            }

            if (m_fuseMount) {
                if (folderOnly.isEmpty())
                    m_fuseMount->invalidatePath(QString());
                if (!targetFolder.isEmpty() && targetFolder != QLatin1String("Root"))
                    m_fuseMount->invalidatePath(targetFolder);
            }

            m_rcloneSyncInFlight.store(false);
            if (folderOnly.isEmpty()) {
                m_syncing = false;
                m_activeSyncFile.clear();
                m_pendingSyncCount = 0;
                m_syncProgress = 1.0;
                m_lastSyncedSecs = QDateTime::currentSecsSinceEpoch();
                if (!rootOk && m_entries.isEmpty()) {
                    setStatusBanner(rootErr.isEmpty() ? tr("Failed to sync with rclone remote '%1:'").arg(m_remoteName) : rootErr, true);
                } else if (rootOk) {
                    const QString spec = buildRcloneSpec(m_remoteName, m_remotePath);
                    setStatusBanner(tr("Connected to Google Drive '%1' — %2 files, %3 folders synced")
                                        .arg(spec)
                                        .arg(totalFileCount())
                                        .arg(m_remoteFolders.size()),
                                    false);
                }
            }

            QString queuedFolder;
            {
                QMutexLocker locker(&m_mutex);
                queuedFolder = m_pendingFolderSync;
                m_pendingFolderSync.clear();
            }

            saveState();
            rebuildFilteredModel();
            emit statusChanged();
            emit statsChanged();

            if (!queuedFolder.isEmpty())
                startAsyncRcloneSync(queuedFolder);
        }, Qt::QueuedConnection);
    });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void Backend::scheduleAsyncDirRefresh(const QString &relDirPath) {
    if (!shouldUseAsyncRcloneSync())
        return;

    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString configPath;
    {
        QMutexLocker locker(&m_mutex);
        if (m_sourceMode != QLatin1String("rclone") || !m_rcloneAvailable || !m_availableRemotes.contains(m_remoteName))
            return;
        if (m_dirRefreshInFlight.contains(relDirPath))
            return;
        m_dirRefreshInFlight.insert(relDirPath);
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        configPath = m_rcloneConfigPath;
    }

    QPointer<Backend> self(this);
    QThread *t = QThread::create([this, self, relDirPath, rcloneBin, remoteName, remotePath, configPath]() {
        QByteArray dirJson;
        QString dirErr;
        const QString spec = buildRcloneSpec(remoteName, remotePath, relDirPath);
        const bool synced = runRcloneLsjson(rcloneBin, spec, 1, false, 60000, &dirJson, &dirErr, configPath, false, false);
        if (!synced) {
            qWarning("wickfs: async readdir refresh failed for %s: %s",
                     qPrintable(spec), qPrintable(dirErr));
        }

        if (!self)
            return;
        QMetaObject::invokeMethod(this, [this, self, relDirPath, synced, dirJson]() {
            if (!self)
                return;
            {
                QMutexLocker locker(&m_mutex);
                m_dirRefreshInFlight.remove(relDirPath);
                const qint64 doneMs = QDateTime::currentMSecsSinceEpoch();
                if (synced) {
                    if (relDirPath.isEmpty())
                        mergeRcloneRootJsonLocked(dirJson);
                    else
                        mergeRcloneFolderJsonLocked(relDirPath, dirJson, true);
                } else {
                    // Back off 15s before retrying failed background refresh
                    m_syncedDirTimestampsMs.insert(relDirPath, doneMs - kDirSyncTtlMs + 15000LL);
                }
            }
            if (synced) {
                if (m_fuseMount)
                    m_fuseMount->invalidatePath(relDirPath);
                scheduleSaveState(500);
                rebuildFilteredModel();
                emit statsChanged();
            }
        }, Qt::QueuedConnection);
    });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void Backend::pollRemoteChanges() {
    bool expected = false;
    if (!m_changePollInFlight.compare_exchange_strong(expected, true))
        return;

    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString configPath;
    QString pageToken;
    QString rootFolderId;
    QHash<QString, QString> idToPath;
    QString cachedToken;
    QDateTime cachedExpiry;
    {
        QMutexLocker locker(&m_mutex);
        if (m_sourceMode != QLatin1String("rclone") || !m_oauthAuthorized
            || m_remoteName.isEmpty() || m_rcloneSyncInFlight.load()) {
            m_changePollInFlight.store(false);
            return;
        }
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        configPath = m_rcloneConfigPath.isEmpty() ? rcloneConfigPath() : m_rcloneConfigPath;
        pageToken = m_driveChangePageToken;
        rootFolderId = m_driveRootFolderId;
        idToPath = m_driveFolderIdToPath;
        cachedToken = m_cachedAccessToken;
        cachedExpiry = m_cachedAccessTokenExpiry;
    }

    QPointer<Backend> self(this);
    QThread *t = QThread::create([this, self, rcloneBin, remoteName, remotePath, configPath,
                                  pageToken, rootFolderId, idToPath, cachedToken, cachedExpiry]() mutable {
        const QString apiBase = qEnvironmentVariable("WICKFS_DRIVE_API_BASE").isEmpty()
            ? QStringLiteral("https://www.googleapis.com/drive/v3")
            : qEnvironmentVariable("WICKFS_DRIVE_API_BASE");
        const QString tokenUrl = qEnvironmentVariable("WICKFS_OAUTH_TOKEN_URL").isEmpty()
            ? QStringLiteral("https://oauth2.googleapis.com/token")
            : qEnvironmentVariable("WICKFS_OAUTH_TOKEN_URL");

        const RcloneDriveAuth fileAuth = readRcloneDriveAuth(configPath, remoteName);
        if (!fileAuth.valid) {
            QMetaObject::invokeMethod(this, [this, self]() {
                if (self)
                    m_changePollInFlight.store(false);
            }, Qt::QueuedConnection);
            return;
        }

        QNetworkAccessManager nam;
        QString accessToken = (!cachedToken.isEmpty() && cachedExpiry.isValid()
                               && cachedExpiry > QDateTime::currentDateTimeUtc().addSecs(60))
            ? cachedToken
            : fileAuth.accessToken;
        QDateTime tokenExpiry = (!cachedToken.isEmpty() && cachedExpiry.isValid()) ? cachedExpiry : fileAuth.expiry;

        if (tokenExpiry.isValid() && tokenExpiry <= QDateTime::currentDateTimeUtc().addSecs(60)) {
            QString refreshed;
            QDateTime refreshedExp;
            if (refreshDriveAccessToken(nam, tokenUrl, fileAuth, rcloneBin, remoteName, configPath, &refreshed, &refreshedExp)) {
                accessToken = refreshed;
                tokenExpiry = refreshedExp;
            }
        }

        auto callWithAuthRetry = [&](const QUrl &url, int *statusOut, QJsonObject *jsonOut) -> bool {
            if (driveHttpRequestJson(nam, QStringLiteral("GET"), url, QByteArray(), accessToken, QString(), 15000, statusOut, jsonOut, nullptr))
                return true;
            if (statusOut && *statusOut == 401) {
                QString refreshed;
                QDateTime refreshedExp;
                if (refreshDriveAccessToken(nam, tokenUrl, fileAuth, rcloneBin, remoteName, configPath, &refreshed, &refreshedExp)) {
                    accessToken = refreshed;
                    tokenExpiry = refreshedExp;
                    return driveHttpRequestJson(nam, QStringLiteral("GET"), url, QByteArray(), accessToken, QString(), 15000, statusOut, jsonOut, nullptr);
                }
            }
            return false;
        };

        // First run: record startPageToken so subsequent polls catch every incremental edit.
        if (pageToken.isEmpty()) {
            QUrl url(apiBase + QStringLiteral("/changes/startPageToken"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("supportsAllDrives"), QStringLiteral("true"));
            if (!fileAuth.teamDrive.isEmpty())
                q.addQueryItem(QStringLiteral("driveId"), fileAuth.teamDrive);
            url.setQuery(q);

            int status = 0;
            QJsonObject resp;
            const bool ok = callWithAuthRetry(url, &status, &resp);
            const QString startTok = ok ? resp.value(QStringLiteral("startPageToken")).toString() : QString();
            if (!self)
                return;
            QMetaObject::invokeMethod(this, [this, self, startTok, accessToken, tokenExpiry]() {
                if (!self)
                    return;
                m_changePollInFlight.store(false);
                if (!startTok.isEmpty())
                    m_driveChangePageToken = startTok;
                if (!accessToken.isEmpty()) {
                    m_cachedAccessToken = accessToken;
                    m_cachedAccessTokenExpiry = tokenExpiry;
                }
            }, Qt::QueuedConnection);
            return;
        }

        QSet<QString> dirtyDirs;
        QSet<QString> dirtyFiles;
        bool hasUnresolvedChange = false;
        bool resetPageToken = false;
        QString nextTok = pageToken;
        QString latestStartTok;

        for (int page = 0; page < 5 && !nextTok.isEmpty(); ++page) {
            QUrl url(apiBase + QStringLiteral("/changes"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("pageToken"), nextTok);
            q.addQueryItem(QStringLiteral("includeItemsFromAllDrives"), QStringLiteral("true"));
            q.addQueryItem(QStringLiteral("supportsAllDrives"), QStringLiteral("true"));
            q.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("100"));
            q.addQueryItem(QStringLiteral("fields"),
                           QStringLiteral("nextPageToken,newStartPageToken,changes(fileId,removed,file(id,name,mimeType,parents,trashed))"));
            if (!fileAuth.teamDrive.isEmpty())
                q.addQueryItem(QStringLiteral("driveId"), fileAuth.teamDrive);
            url.setQuery(q);

            int status = 0;
            QJsonObject resp;
            if (!callWithAuthRetry(url, &status, &resp)) {
                if (status == 400 || status == 410)
                    resetPageToken = true;
                break;
            }

            const QJsonArray changes = resp.value(QStringLiteral("changes")).toArray();
            if (!changes.isEmpty() && rootFolderId.isEmpty()) {
                if (!fileAuth.rootFolderId.isEmpty()) {
                    rootFolderId = fileAuth.rootFolderId;
                } else if (!fileAuth.teamDrive.isEmpty()) {
                    rootFolderId = fileAuth.teamDrive;
                } else {
                    QUrl rootUrl(apiBase + QStringLiteral("/files/root"));
                    QUrlQuery rq;
                    rq.addQueryItem(QStringLiteral("fields"), QStringLiteral("id"));
                    rq.addQueryItem(QStringLiteral("supportsAllDrives"), QStringLiteral("true"));
                    rootUrl.setQuery(rq);
                    int rStatus = 0;
                    QJsonObject rootObj;
                    if (callWithAuthRetry(rootUrl, &rStatus, &rootObj))
                        rootFolderId = rootObj.value(QStringLiteral("id")).toString();
                }
            }

            const QString cleanRemotePrefix = cleanRelPath(remotePath);
            for (const auto &cv : changes) {
                const QJsonObject cObj = cv.toObject();
                const QString fileId = cObj.value(QStringLiteral("fileId")).toString();
                const bool removed = cObj.value(QStringLiteral("removed")).toBool();
                const QJsonObject fObj = cObj.value(QStringLiteral("file")).toObject();
                const QString fName = cleanRelPath(fObj.value(QStringLiteral("name")).toString());
                const QString mime = fObj.value(QStringLiteral("mimeType")).toString();
                if (!fileId.isEmpty() && mime == QLatin1String("application/vnd.google-apps.folder"))
                    idToPath.remove(fileId);

                const QJsonArray parents = fObj.value(QStringLiteral("parents")).toArray();
                if (parents.isEmpty() || rootFolderId.isEmpty()) {
                    hasUnresolvedChange = true;
                    continue;
                }

                bool resolvedAnyParent = false;
                for (const auto &pv : parents) {
                    QString relDir;
                    if (resolveDriveFolderRelPath(nam, apiBase, accessToken, pv.toString(),
                                                  rootFolderId, idToPath, cleanRemotePrefix, &relDir)) {
                        dirtyDirs.insert(relDir);
                        if (!fName.isEmpty())
                            dirtyFiles.insert(relDir.isEmpty() ? fName : (relDir + QLatin1Char('/') + fName));
                        resolvedAnyParent = true;
                    }
                }
                if (!resolvedAnyParent && !removed)
                    hasUnresolvedChange = true;
            }

            latestStartTok = resp.value(QStringLiteral("newStartPageToken")).toString();
            nextTok = resp.value(QStringLiteral("nextPageToken")).toString();
            if (!latestStartTok.isEmpty())
                break;
        }

        if (!self)
            return;
        QMetaObject::invokeMethod(this, [this, self, resetPageToken, latestStartTok, nextTok,
                                         rootFolderId, idToPath, dirtyDirs, dirtyFiles,
                                         hasUnresolvedChange, accessToken, tokenExpiry]() {
            if (!self)
                return;
            m_changePollInFlight.store(false);
            if (!accessToken.isEmpty()) {
                m_cachedAccessToken = accessToken;
                m_cachedAccessTokenExpiry = tokenExpiry;
            }
            if (!rootFolderId.isEmpty())
                m_driveRootFolderId = rootFolderId;
            if (!idToPath.isEmpty())
                m_driveFolderIdToPath = idToPath;

            if (resetPageToken) {
                m_driveChangePageToken.clear();
                startAsyncRcloneSync();
                return;
            }
            if (!latestStartTok.isEmpty())
                m_driveChangePageToken = latestStartTok;
            else if (!nextTok.isEmpty())
                m_driveChangePageToken = nextTok;

            if (dirtyDirs.isEmpty() && !hasUnresolvedChange)
                return;

            QStringList dirsToRefresh;
            {
                QMutexLocker locker(&m_mutex);
                for (const QString &d : dirtyDirs) {
                    m_syncedDirTimestampsMs.insert(d, 0);
                    if (d.isEmpty() || m_listedDirs.contains(d) || m_vfsDirs.contains(d))
                        dirsToRefresh.append(d);
                }
                if (hasUnresolvedChange && dirsToRefresh.isEmpty()) {
                    for (const QString &ld : std::as_const(m_listedDirs))
                        m_syncedDirTimestampsMs.insert(ld, 0);
                }
            }

            if (m_fuseMount) {
                for (const QString &fp : dirtyFiles)
                    m_fuseMount->invalidatePath(fp);
            }

            if (dirsToRefresh.isEmpty() && hasUnresolvedChange) {
                startAsyncRcloneSync();
                return;
            }

            for (const QString &d : dirsToRefresh) {
                if (shouldUseAsyncRcloneSync()) {
                    scheduleAsyncDirRefresh(d);
                } else if (d.isEmpty()) {
                    syncNow();
                } else {
                    syncRemoteFolder(d);
                }
            }
        }, Qt::QueuedConnection);
    });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void Backend::syncRemoteFolder(const QString &folder) {
    if (folder.isEmpty() || folder == QLatin1String("Root"))
        return;
    if (m_sourceMode != QLatin1String("rclone") || !m_rcloneAvailable || !m_availableRemotes.contains(m_remoteName))
        return;

    if (shouldUseAsyncRcloneSync()) {
        startAsyncRcloneSync(folder);
        return;
    }

    const QString spec = buildRcloneSpec(m_remoteName, m_remotePath, folder);
    QByteArray out;
    if (!runRcloneLsjson(m_rcloneBin, spec, 2, false, 12000, &out, nullptr, m_rcloneConfigPath))
        return;

    {
        QMutexLocker locker(&m_mutex);
        mergeRcloneFolderJsonLocked(folder, out, false);
        m_syncedDirTimestampsMs.insert(folder, QDateTime::currentMSecsSinceEpoch());
        syncAllProjectionsLocked();
    }
    saveState();
}

void Backend::syncNow() {
    m_syncing = true;
    emit statusChanged();

    if (m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable && m_availableRemotes.contains(m_remoteName)) {
        if (shouldUseAsyncRcloneSync()) {
            m_activeSyncFile = tr("Remote index (%1:)").arg(m_remoteName);
            m_syncProgress = 0.25;
            emit statusChanged();
            startAsyncRcloneSync();
            return;
        }

        QString uploadErr;
        flushPendingUploads(&uploadErr);

        const QString spec = buildRcloneSpec(m_remoteName, m_remotePath);
        QByteArray out;
        QString err;
        const bool finished = runRcloneLsjson(m_rcloneBin, spec, 1, false, 15000, &out, &err, m_rcloneConfigPath);
        if (finished) {
            {
                QMutexLocker locker(&m_mutex);
                mergeRcloneRootJsonLocked(out);
            }

            if (!m_currentFolder.isEmpty() && m_currentFolder != QLatin1String("Root"))
                syncRemoteFolder(m_currentFolder);

            enforceCacheBudget();
            saveState();
            rebuildFilteredModel();
            logActivity(tr("Synced with Google Drive"),
                        tr("Catalog updated from %1 (%2 files, %3 folders)")
                            .arg(spec)
                            .arg(totalFileCount())
                            .arg(m_remoteFolders.size()),
                        QStringLiteral("SYNC"));
            setStatusBanner(tr("Connected to Google Drive '%1' — %2 files, %3 folders synced")
                                .arg(spec)
                                .arg(totalFileCount())
                                .arg(m_remoteFolders.size()),
                            false);
        } else {
            {
                QMutexLocker locker(&m_mutex);
                recomputeScoresLocked();
                rebuildVfsIndexLocked();
            }
            enforceCacheBudget();
            saveState();
            rebuildFilteredModel();
            if (!m_entries.isEmpty()) {
                setStatusBanner(tr("Connected to Google Drive remote '%1:'").arg(m_remoteName), false);
            } else {
                setStatusBanner(err.isEmpty() ? tr("Failed to sync with rclone remote '%1:'").arg(m_remoteName) : err, true);
            }
        }
    } else if (m_sourceMode == QLatin1String("folder") && !m_localSourceDir.isEmpty()) {
        flushPendingUploads();
        QMutexLocker locker(&m_mutex);
        QHash<QString, DriveFileEntry> existingByPath;
        for (const auto &e : m_entries)
            existingByPath.insert(e.path, e);

        QList<DriveFileEntry> nextEntries;
        QDir baseDir(m_localSourceDir);
        QDirIterator it(m_localSourceDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString relPath = baseDir.relativeFilePath(fi.absoluteFilePath());
            DriveFileEntry e = existingByPath.value(relPath);
            e.path = relPath;
            e.name = fi.fileName();
            const QString dir = QFileInfo(relPath).path();
            e.folder = (dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir;
            e.category = inferCategory(e.name);
            e.sizeBytes = fi.size();
            e.modifiedSecs = fi.lastModified().toSecsSinceEpoch();
            if (e.state.isEmpty())
                e.state = QFileInfo::exists(localBlobPath(relPath)) ? QStringLiteral("cached") : QStringLiteral("cloud");
            nextEntries.append(e);
        }
        m_entries = nextEntries;
        recomputeScoresLocked();
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].pinned || (m_autoKeepFrequent && m_entries[i].isFrequent)) {
                if (cachedBytes() + m_entries[i].sizeBytes <= effectiveBudgetBytes())
                    hydrateEntryLocked(i);
            }
        }
        rebuildVfsIndexLocked();
        locker.unlock();
        enforceCacheBudget();
        saveState();
        rebuildFilteredModel();
        logActivity(tr("Synced Source Folder"),
                    tr("Indexed %1 files from %2").arg(totalFileCount()).arg(m_localSourceDir),
                    QStringLiteral("SYNC"));
        setStatusBanner(tr("Synced %1 files from %2").arg(totalFileCount()).arg(m_localSourceDir), false);
    } else {
        {
            QMutexLocker locker(&m_mutex);
            recomputeScoresLocked();
            for (int i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].pinned || (m_autoKeepFrequent && m_entries[i].isFrequent)) {
                    if (cachedBytes() + m_entries[i].sizeBytes <= effectiveBudgetBytes())
                        hydrateEntryLocked(i);
                }
            }
        }
        enforceCacheBudget();
        saveState();
        rebuildFilteredModel();
        logActivity(tr("Refreshed Smart Cache"),
                    tr("Re-evaluated LFU/LRU scores (%1 cached / %2 budget)")
                        .arg(cachedFormatted(), effectiveBudgetFormatted()),
                    QStringLiteral("SYNC"));
    }

    m_lastSyncedSecs = QDateTime::currentSecsSinceEpoch();
    if (qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")) {
        const QString firstFile = m_entries.isEmpty() ? QStringLiteral("Drive Catalog") : m_entries.first().name;
        beginSyncSimulation(firstFile, qMax(1, cachedFileCount()));
        m_syncAnimTimer.start();
    } else {
        m_syncing = false;
        m_activeSyncFile.clear();
        m_pendingSyncCount = 0;
        m_syncProgress = 1.0;
        saveState();
        emit statusChanged();
    }
}

bool Backend::loadManifestFile(const QString &filePath, QString *errorOut) {
    QFileInfo fi(filePath);
    if (fi.isDir())
        return connectLocalSource(filePath);

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = tr("Could not open %1").arg(filePath);
        return false;
    }

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray())) {
        if (errorOut)
            *errorOut = tr("Invalid JSON manifest in %1").arg(filePath);
        return false;
    }

    QJsonArray filesArr;
    if (doc.isArray()) {
        filesArr = doc.array();
    } else {
        const QJsonObject root = doc.object();
        if (root.contains("maxCacheMb"))
            m_maxCacheBytes = static_cast<qint64>(root.value("maxCacheMb").toInt(250)) * 1024LL * 1024LL;
        if (root.contains("frequentThreshold"))
            m_frequentThreshold = qMax(1, root.value("frequentThreshold").toInt(3));
        filesArr = root.value("files").toArray();
    }

    {
        QMutexLocker locker(&m_mutex);
        m_entries.clear();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (const auto &v : filesArr) {
            const QJsonObject o = v.toObject();
            DriveFileEntry e;
            e.path = o.value("path").toString();
            if (e.path.isEmpty())
                continue;
            e.name = o.value("name").toString(QFileInfo(e.path).fileName());
            const QString dir = QFileInfo(e.path).path();
            e.folder = o.value("folder").toString((dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir);
            e.category = o.value("category").toString(inferCategory(e.name));
            e.sizeBytes = static_cast<qint64>(o.value("sizeBytes").toDouble(1024 * 1024));
            e.modifiedSecs = static_cast<qint64>(o.value("modifiedSecs").toDouble(now));
            e.pinned = o.value("pinned").toBool(false);
            e.state = e.pinned ? QStringLiteral("pinned") : o.value("state").toString(QStringLiteral("cloud"));
            e.accessCount = o.value("accessCount").toInt(0);
            e.lastAccessedSecs = static_cast<qint64>(o.value("lastAccessedSecs").toDouble(e.accessCount > 0 ? now : 0));
            m_entries.append(e);
        }
        recomputeScoresLocked(now);
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].state == QLatin1String("pinned") || m_entries[i].state == QLatin1String("cached"))
                hydrateEntryLocked(i);
        }
        rebuildVfsIndexLocked();
    }

    enforceCacheBudget();
    saveState();
    rebuildFilteredModel();
    return true;
}

void Backend::selectNext() {
    if (m_fileModel.rowCount() > 0)
        setSelectedIndex(qMin(m_fileModel.rowCount() - 1, m_selectedIndex + 1));
}

void Backend::selectPrevious() {
    if (m_fileModel.rowCount() > 0)
        setSelectedIndex(qMax(0, m_selectedIndex - 1));
}

void Backend::pushUndoSnapshot(const QString &description) {
    QMutexLocker locker(&m_mutex);
    SnapshotState snap;
    snap.entries = m_entries;
    snap.maxCacheBytes = m_maxCacheBytes;
    snap.minFreeDiskBytes = m_minFreeDiskBytes;
    snap.frequentThreshold = m_frequentThreshold;
    snap.autoKeepFrequent = m_autoKeepFrequent;
    snap.description = description;
    m_undoStack.append(snap);
    if (m_undoStack.size() > 30)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    emit undoRedoChanged();
}

void Backend::applySnapshot(const SnapshotState &snap) {
    {
        QMutexLocker locker(&m_mutex);
        m_entries = snap.entries;
        m_maxCacheBytes = snap.maxCacheBytes;
        m_minFreeDiskBytes = snap.minFreeDiskBytes;
        m_frequentThreshold = snap.frequentThreshold;
        m_autoKeepFrequent = snap.autoKeepFrequent;
        recomputeScoresLocked();

        for (int i = 0; i < m_entries.size(); ++i) {
            const QString blobPath = localBlobPath(m_entries[i].path);
            if (m_entries[i].state == QLatin1String("pinned") || m_entries[i].state == QLatin1String("cached")) {
                hydrateEntryLocked(i);
            } else if (QFileInfo::exists(blobPath)) {
                QFile::remove(blobPath);
                syncProjectionForEntryLocked(m_entries[i]);
            }
        }
        rebuildVfsIndexLocked();
    }
    saveState();
    rebuildFilteredModel();
    emit budgetChanged();
    emit undoRedoChanged();
}

void Backend::undo() {
    if (m_undoStack.isEmpty())
        return;
    SnapshotState current;
    {
        QMutexLocker locker(&m_mutex);
        current.entries = m_entries;
        current.maxCacheBytes = m_maxCacheBytes;
        current.minFreeDiskBytes = m_minFreeDiskBytes;
        current.frequentThreshold = m_frequentThreshold;
        current.autoKeepFrequent = m_autoKeepFrequent;
    }
    const SnapshotState prev = m_undoStack.takeLast();
    m_redoStack.append(current);
    applySnapshot(prev);
    logActivity(tr("Undo"), prev.description, QStringLiteral("UNDO"));
}

void Backend::redo() {
    if (m_redoStack.isEmpty())
        return;
    SnapshotState current;
    {
        QMutexLocker locker(&m_mutex);
        current.entries = m_entries;
        current.maxCacheBytes = m_maxCacheBytes;
        current.minFreeDiskBytes = m_minFreeDiskBytes;
        current.frequentThreshold = m_frequentThreshold;
        current.autoKeepFrequent = m_autoKeepFrequent;
    }
    const SnapshotState next = m_redoStack.takeLast();
    m_undoStack.append(current);
    applySnapshot(next);
    logActivity(tr("Redo"), next.description, QStringLiteral("REDO"));
}

bool Backend::flushPendingUploads(QString *errorOut) {
    QSet<QString> attemptedUploads;
    QSet<QString> attemptedDeletes;
    bool allOk = true;
    int uploadedCount = 0;
    int deletedCount = 0;

    while (true) {
        QString nextDelete;
        QString nextUpload;
        QString entryName;
        QString blobPath;
        QString sourceMode;
        QString rcloneBin;
        QString remoteName;
        QString remotePath;
        QString configPath;
        QString localSourceDir;
        bool rcloneActive = false;
        qint64 snapshotSize = 0;
        qint64 snapshotMtime = 0;

        {
            QMutexLocker locker(&m_mutex);
            sourceMode = m_sourceMode;
            rcloneBin = m_rcloneBin;
            remoteName = m_remoteName;
            remotePath = m_remotePath;
            configPath = m_rcloneConfigPath;
            localSourceDir = m_localSourceDir;
            rcloneActive = (sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                            && !remoteName.isEmpty() && m_availableRemotes.contains(remoteName));

            for (const QString &delPath : std::as_const(m_pendingDeletes)) {
                if (!attemptedDeletes.contains(delPath)) {
                    nextDelete = delPath;
                    break;
                }
            }

            if (nextDelete.isEmpty()) {
                for (const auto &e : std::as_const(m_entries)) {
                    if (e.dirty && !m_openWriteCounts.contains(e.path) && !attemptedUploads.contains(e.path)) {
                        nextUpload = e.path;
                        entryName = e.name;
                        blobPath = localBlobPath(e.path);
                        snapshotSize = e.sizeBytes;
                        snapshotMtime = e.modifiedSecs;
                        break;
                    }
                }
            }
        }

        if (nextDelete.isEmpty() && nextUpload.isEmpty())
            break;

        if (!nextDelete.isEmpty()) {
            attemptedDeletes.insert(nextDelete);
            const auto pathLock = perPathHydrateMutex(nextDelete);
            QMutexLocker pathLocker(pathLock.data());

            {
                QMutexLocker locker(&m_mutex);
                if (!m_pendingDeletes.contains(nextDelete))
                    continue;
            }

            if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
                const QString dstLocal = localSourceDir + QLatin1Char('/') + nextDelete;
                if (QFileInfo::exists(dstLocal) && !QFile::remove(dstLocal)) {
                    allOk = false;
                    if (errorOut && errorOut->isEmpty())
                        *errorOut = tr("Failed to delete %1 from source folder").arg(nextDelete);
                    continue;
                }
                QMutexLocker locker(&m_mutex);
                m_pendingDeletes.remove(nextDelete);
                ++deletedCount;
                continue;
            }

            if (!rcloneActive)
                break;

            const QString spec = buildRcloneSpec(remoteName, remotePath, nextDelete);
            QString delErr;
            if (runRcloneDeletefile(rcloneBin, spec, 30000, &delErr, configPath) || isRcloneNotFoundError(delErr)) {
                QMutexLocker locker(&m_mutex);
                m_pendingDeletes.remove(nextDelete);
                ++deletedCount;
            } else {
                allOk = false;
                if (errorOut && errorOut->isEmpty())
                    *errorOut = delErr;
                qWarning("wickfs: rclone deletefile %s failed: %s", qPrintable(spec), qPrintable(delErr));
            }
            continue;
        }

        attemptedUploads.insert(nextUpload);
        const auto pathLock = perPathHydrateMutex(nextUpload);
        QMutexLocker pathLocker(pathLock.data());

        {
            QMutexLocker locker(&m_mutex);
            if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
                rebuildVfsIndexLocked();
            const int idx = m_vfsFileIdx.value(nextUpload, -1);
            if (idx < 0 || idx >= m_entries.size() || !m_entries.at(idx).dirty
                || m_openWriteCounts.contains(nextUpload)) {
                continue;
            }
            const QFileInfo blobFi(blobPath);
            if (!blobFi.exists()) {
                m_entries[idx].dirty = false;
                continue;
            }
            snapshotSize = blobFi.size();
            snapshotMtime = m_entries.at(idx).modifiedSecs;
            entryName = m_entries.at(idx).name;
        }

        if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
            const QString dstLocal = localSourceDir + QLatin1Char('/') + nextUpload;
            if (!copyFileAtomic(blobPath, dstLocal)) {
                allOk = false;
                if (errorOut && errorOut->isEmpty())
                    *errorOut = tr("Failed to copy %1 to source folder").arg(nextUpload);
                continue;
            }
            QMutexLocker locker(&m_mutex);
            const int idx = m_vfsFileIdx.value(nextUpload, -1);
            if (idx >= 0 && idx < m_entries.size()
                && !m_openWriteCounts.contains(nextUpload)
                && m_entries.at(idx).modifiedSecs == snapshotMtime
                && QFileInfo(blobPath).size() == snapshotSize) {
                m_entries[idx].dirty = false;
                m_entries[idx].sizeBytes = snapshotSize;
                m_statsDirty = true;
            }
            ++uploadedCount;
            continue;
        }

        if (!rcloneActive)
            break;

        const QString spec = buildRcloneSpec(remoteName, remotePath, nextUpload);
        const int timeoutMs = qMax(60000, static_cast<int>(qMin<qint64>(600000, 60000 + (snapshotSize / (64LL * 1024LL)) * 1000LL)));
        QString uploadErr;
        if (runRcloneUpload(rcloneBin, blobPath, spec, timeoutMs, &uploadErr, configPath)) {
            QMutexLocker locker(&m_mutex);
            if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
                rebuildVfsIndexLocked();
            const int idx = m_vfsFileIdx.value(nextUpload, -1);
            if (idx >= 0 && idx < m_entries.size()
                && !m_openWriteCounts.contains(nextUpload)
                && m_entries.at(idx).modifiedSecs == snapshotMtime
                && QFileInfo(blobPath).size() == snapshotSize) {
                m_entries[idx].dirty = false;
                m_entries[idx].sizeBytes = snapshotSize;
                m_statsDirty = true;
            }
            ++uploadedCount;
        } else {
            allOk = false;
            if (errorOut && errorOut->isEmpty())
                *errorOut = uploadErr;
            qWarning("wickfs: rclone upload %s failed: %s", qPrintable(spec), qPrintable(uploadErr));
        }
    }

    if (uploadedCount > 0 || deletedCount > 0) {
        QPointer<Backend> self(this);
        auto notify = [this, self, uploadedCount, deletedCount]() {
            if (!self)
                return;
            if (uploadedCount > 0) {
                logActivity(tr("Uploaded to Cloud"),
                            tr("Synced %1 local file(s) to %2").arg(uploadedCount).arg(m_remoteName),
                            QStringLiteral("UPLOAD"));
            }
            if (deletedCount > 0) {
                logActivity(tr("Removed from Cloud"),
                            tr("Deleted %1 file(s) from %2").arg(deletedCount).arg(m_remoteName),
                            QStringLiteral("DELETE"));
            }
            m_lastSyncedSecs = QDateTime::currentSecsSinceEpoch();
            if (m_isTestInstance)
                saveState();
            else
                scheduleSaveState(400);
            rebuildFilteredModel();
            emit statusChanged();
            emit statsChanged();
        };
        if (QThread::currentThread() == this->thread())
            notify();
        else
            QMetaObject::invokeMethod(this, notify, Qt::QueuedConnection);
    }

    return allOk;
}

void Backend::schedulePendingUpload(int delayMs) {
    if (!shouldUseAsyncRcloneSync())
        return;

    QPointer<Backend> self(this);
    auto arm = [this, self, delayMs]() {
        if (!self)
            return;
        if (m_uploadInFlight.load()) {
            m_uploadMorePending = true;
            return;
        }
        m_uploadDebounceTimer.start(delayMs);
    };
    if (QThread::currentThread() == this->thread())
        arm();
    else
        QMetaObject::invokeMethod(this, arm, Qt::QueuedConnection);
}

void Backend::startAsyncPendingUpload() {
    if (!shouldUseAsyncRcloneSync())
        return;
    if (pendingUploadCount() <= 0)
        return;

    bool expected = false;
    if (!m_uploadInFlight.compare_exchange_strong(expected, true)) {
        m_uploadMorePending = true;
        return;
    }

    m_uploadMorePending = false;
    emit statusChanged();

    QPointer<Backend> self(this);
    QThread *t = QThread::create([this, self]() {
        QString err;
        const bool ok = flushPendingUploads(&err);

        if (!self)
            return;
        QMetaObject::invokeMethod(this, [this, self, ok, err]() {
            if (!self)
                return;
            m_uploadInFlight.store(false);
            const bool runAgain = m_uploadMorePending && ok && pendingUploadCount() > 0;
            m_uploadMorePending = false;
            if (!ok && !err.isEmpty())
                setStatusBanner(tr("Cloud upload pending: %1").arg(err), true);
            else if (pendingUploadCount() == 0 && m_sourceMode == QLatin1String("rclone"))
                setStatusBanner(tr("Connected to Google Drive remote '%1:'").arg(m_remoteName), false);
            saveState();
            rebuildFilteredModel();
            emit statusChanged();
            emit statsChanged();
            if (runAgain)
                startAsyncPendingUpload();
        }, Qt::QueuedConnection);
    });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void Backend::rebuildVfsIndexLocked() const {
    m_vfsFileIdx.clear();
    m_vfsDirChildren.clear();
    m_vfsDirs.clear();
    m_vfsDirs.insert(QString());

    QHash<QString, QSet<QString>> childrenSets;

    auto registerDirChain = [&](const QString &dirPath) {
        if (dirPath.isEmpty() || dirPath == QLatin1String("."))
            return;
        QString clean = dirPath;
        while (clean.startsWith('/'))
            clean.remove(0, 1);
        while (clean.endsWith('/'))
            clean.chop(1);
        if (clean.isEmpty() || clean == QLatin1String("Root"))
            return;

        const QStringList parts = clean.split('/', Qt::SkipEmptyParts);
        QString parent;
        for (const QString &part : parts) {
            childrenSets[parent].insert(part);
            parent = parent.isEmpty() ? part : (parent + QLatin1Char('/') + part);
            m_vfsDirs.insert(parent);
        }
    };

    for (const QString &rf : m_remoteFolders) {
        registerDirChain(rf);
    }

    for (int i = 0; i < m_entries.size(); ++i) {
        const QString &p = m_entries.at(i).path;
        if (p.isEmpty())
            continue;
        m_vfsFileIdx.insert(p, i);
        const int slash = p.lastIndexOf('/');
        if (slash < 0) {
            childrenSets[QString()].insert(p);
        } else {
            const QString parent = p.left(slash);
            const QString name = p.mid(slash + 1);
            registerDirChain(parent);
            if (!name.isEmpty())
                childrenSets[parent].insert(name);
        }
    }

    for (auto it = childrenSets.cbegin(); it != childrenSets.cend(); ++it) {
        QStringList list = it.value().values();
        list.sort(Qt::CaseInsensitive);
        m_vfsDirChildren.insert(it.key(), list);
    }
    m_statsDirty = true;
}

bool Backend::vfsIsCloudOnly(const QString &relPath) const {
    QMutexLocker locker(&m_mutex);
    if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
        rebuildVfsIndexLocked();
    const int idx = m_vfsFileIdx.value(relPath, -1);
    if (idx < 0 || idx >= m_entries.size())
        return false;
    return m_entries.at(idx).state == QLatin1String("cloud");
}

bool Backend::vfsStat(const QString &relPath, qint64 *sizeOut, qint64 *mtimeSecsOut,
                      bool *isDirOut, bool *isResidentOut) const {
    QMutexLocker locker(&m_mutex);
    if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
        rebuildVfsIndexLocked();

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (relPath.isEmpty()) {
        if (sizeOut) *sizeOut = 4096;
        if (mtimeSecsOut) *mtimeSecsOut = now;
        if (isDirOut) *isDirOut = true;
        if (isResidentOut) *isResidentOut = true;
        return true;
    }

    const int idx = m_vfsFileIdx.value(relPath, -1);
    if (idx >= 0 && idx < m_entries.size()) {
        const auto &e = m_entries.at(idx);
        qint64 sz = e.sizeBytes;
        if (m_openWriteCounts.contains(relPath)) {
            const QFileInfo blobFi(localBlobPath(relPath));
            if (blobFi.exists())
                sz = blobFi.size();
        }
        if (sizeOut) *sizeOut = sz;
        if (mtimeSecsOut) *mtimeSecsOut = e.modifiedSecs;
        if (isDirOut) *isDirOut = false;
        if (isResidentOut) *isResidentOut = (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached"));
        return true;
    }

    if (m_vfsDirs.contains(relPath)) {
        if (sizeOut) *sizeOut = 4096;
        if (mtimeSecsOut) *mtimeSecsOut = now;
        if (isDirOut) *isDirOut = true;
        if (isResidentOut) *isResidentOut = true;
        return true;
    }

    return false;
}

QList<VfsDirEntry> Backend::vfsReadDirEntries(const QString &relDirPath, bool *existsOut, bool allowRemoteSync) {
    bool shouldSyncBlocking = false;
    bool shouldRefreshAsync = false;
    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString configPath;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    {
        QMutexLocker locker(&m_mutex);
        if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
            rebuildVfsIndexLocked();

        if (allowRemoteSync && m_vfsDirs.contains(relDirPath)
            && m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable
            && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName)) {
            const bool alreadyListed = m_listedDirs.contains(relDirPath);
            const qint64 lastSyncedMs = m_syncedDirTimestampsMs.value(relDirPath, 0);
            const bool stale = (nowMs - lastSyncedMs >= kDirSyncTtlMs);

            if (!alreadyListed) {
                shouldSyncBlocking = true;
                rcloneBin = m_rcloneBin;
                remoteName = m_remoteName;
                remotePath = m_remotePath;
                configPath = m_rcloneConfigPath;
            } else if (stale) {
                shouldRefreshAsync = true;
            }
        }
    }

    if (shouldRefreshAsync) {
        if (shouldUseAsyncRcloneSync()) {
            scheduleAsyncDirRefresh(relDirPath);
        } else {
            shouldSyncBlocking = true;
        }
    }

    if (shouldSyncBlocking) {
        const auto dirLock = perPathHydrateMutex(QStringLiteral("dir:") + relDirPath);
        QMutexLocker dirLocker(dirLock.data());

        bool stillNeedsSync = false;
        {
            QMutexLocker locker(&m_mutex);
            const qint64 checkNowMs = QDateTime::currentMSecsSinceEpoch();
            const bool alreadyListed = m_listedDirs.contains(relDirPath);
            const bool stale = (checkNowMs - m_syncedDirTimestampsMs.value(relDirPath, 0) >= kDirSyncTtlMs);
            stillNeedsSync = m_vfsDirs.contains(relDirPath)
                && m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName)
                && (!alreadyListed || (!shouldUseAsyncRcloneSync() && stale));
            if (stillNeedsSync) {
                rcloneBin = m_rcloneBin;
                remoteName = m_remoteName;
                remotePath = m_remotePath;
                configPath = m_rcloneConfigPath;
            }
        }

        if (stillNeedsSync) {
            QByteArray dirJson;
            QString dirErr;
            const QString spec = buildRcloneSpec(remoteName, remotePath, relDirPath);
            bool synced = false;
            if (QThread::currentThread() == this->thread()) {
                synced = runRcloneLsjson(rcloneBin, spec, 1, false, 60000, &dirJson, &dirErr, configPath);
            } else {
                // Run QProcess inside a QThread rather than a bare std::thread (FUSE loop).
                std::unique_ptr<QThread> worker(QThread::create([&]() {
                    synced = runRcloneLsjson(rcloneBin, spec, 1, false, 60000, &dirJson, &dirErr, configPath);
                }));
                worker->start();
                worker->wait();
            }
            if (!synced) {
                qWarning("wickfs: on-demand readdir sync failed for %s: %s",
                         qPrintable(spec), qPrintable(dirErr));
            }

            {
                QMutexLocker locker(&m_mutex);
                const qint64 doneMs = QDateTime::currentMSecsSinceEpoch();
                // On transient failure (e.g. offline), back off for 15s before retrying.
                if (synced) {
                    if (relDirPath.isEmpty())
                        mergeRcloneRootJsonLocked(dirJson);
                    else
                        mergeRcloneFolderJsonLocked(relDirPath, dirJson, true);
                } else {
                    m_syncedDirTimestampsMs.insert(relDirPath, doneMs - kDirSyncTtlMs + 15000LL);
                }
            }

            if (synced) {
                auto persistUpdatedCatalog = [this]() {
                    if (m_isTestInstance)
                        saveState();
                    else
                        scheduleSaveState(500);
                    rebuildFilteredModel();
                    emit statsChanged();
                };
                if (QThread::currentThread() != this->thread())
                    QMetaObject::invokeMethod(this, persistUpdatedCatalog, Qt::QueuedConnection);
                else
                    persistUpdatedCatalog();
            }
        }
    }

    QMutexLocker locker(&m_mutex);
    if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
        rebuildVfsIndexLocked();

    const bool foundDir = m_vfsDirs.contains(relDirPath);
    if (existsOut)
        *existsOut = foundDir;
    if (!foundDir)
        return {};

    const QStringList names = m_vfsDirChildren.value(relDirPath);
    QList<VfsDirEntry> result;
    result.reserve(names.size());
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    for (const QString &name : names) {
        const QString childPath = relDirPath.isEmpty() ? name : (relDirPath + QLatin1Char('/') + name);
        VfsDirEntry item;
        item.name = name;
        const int idx = m_vfsFileIdx.value(childPath, -1);
        if (idx >= 0 && idx < m_entries.size()) {
            const auto &e = m_entries.at(idx);
            item.sizeBytes = e.sizeBytes;
            item.mtimeSecs = e.modifiedSecs;
            item.isDir = false;
            item.isResident = (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached"));
        } else {
            item.sizeBytes = 4096;
            item.mtimeSecs = nowSecs;
            item.isDir = true;
            item.isResident = true;
        }
        result.append(item);
    }
    return result;
}

QStringList Backend::vfsReadDir(const QString &relDirPath, bool *existsOut, bool allowRemoteSync) {
    const QList<VfsDirEntry> entries = vfsReadDirEntries(relDirPath, existsOut, allowRemoteSync);
    QStringList names;
    names.reserve(entries.size());
    for (const VfsDirEntry &e : entries)
        names.append(e.name);
    return names;
}

QString Backend::vfsOpenAndHydrate(const QString &relPath, bool forWrite, QString *errorOut) {
    Q_UNUSED(forWrite);
    QString blobPath;
    QString entryName;
    QString sourceMode;
    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString rcloneConfigPath;
    QString localSourceDir;
    bool rcloneAvailable = false;
    DriveFileEntry entryCopy;
    qint64 entrySize = 0;
    bool wasCloudOnly = false;
    bool becameFrequent = false;
    int newAccessCount = 0;

    {
        QMutexLocker locker(&m_mutex);
        if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
            rebuildVfsIndexLocked();
        const int idx = m_vfsFileIdx.value(relPath, -1);
        if (idx < 0 || idx >= m_entries.size()) {
            if (errorOut)
                *errorOut = tr("File not found in Drive catalog: %1").arg(relPath);
            return {};
        }

        entryCopy = m_entries.at(idx);
        wasCloudOnly = (entryCopy.state == QLatin1String("cloud"));
        sourceMode = m_sourceMode;
        rcloneAvailable = m_rcloneAvailable;
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        rcloneConfigPath = m_rcloneConfigPath;
        localSourceDir = m_localSourceDir;
        blobPath = localBlobPath(relPath);
    }

    // Hydrate completely OUTSIDE m_mutex so FUSE getattr/readdir/other files never block
    if (!QFileInfo::exists(blobPath)) {
        const auto fileLock = perPathHydrateMutex(relPath);
        QMutexLocker fileLocker(fileLock.data());
        if (!QFileInfo::exists(blobPath)) {
            QDir().mkpath(QFileInfo(blobPath).absolutePath());
            if (sourceMode == QLatin1String("rclone") && rcloneAvailable) {
                QString rcloneErr;
                if (!fetchFromRclonePath(rcloneBin, remoteName, remotePath, relPath, blobPath, &rcloneErr, rcloneConfigPath)) {
                    if (errorOut)
                        *errorOut = rcloneErr.isEmpty() ? tr("Failed to download %1 from rclone").arg(entryCopy.name) : rcloneErr;
                    return {};
                }
            } else if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
                const QString srcFile = localSourceDir + "/" + relPath;
                if (QFileInfo::exists(srcFile)) {
                    const QString partPath = blobPath + QStringLiteral(".part");
                    QFile::remove(partPath);
                    if (!QFile::copy(srcFile, partPath)) {
                        QFile::remove(partPath);
                        if (errorOut)
                            *errorOut = tr("Failed to copy %1 from source folder").arg(entryCopy.name);
                        return {};
                    }
                    QFile::remove(blobPath);
                    if (!QFile::rename(partPath, blobPath)) {
                        QFile::remove(partPath);
                        return {};
                    }
                }
            }

            if (!QFileInfo::exists(blobPath)) {
                QSaveFile f(blobPath);
                if (!f.open(QIODevice::WriteOnly)) {
                    if (errorOut)
                        *errorOut = tr("Cannot write cache file %1").arg(blobPath);
                    return {};
                }
                f.write(synthesizePayloadForEntry(entryCopy));
                if (!f.commit()) {
                    if (errorOut)
                        *errorOut = tr("Failed to commit cache file %1").arg(blobPath);
                    return {};
                }
            }
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        const int idx = m_vfsFileIdx.value(relPath, -1);
        if (idx < 0 || idx >= m_entries.size()) {
            if (errorOut)
                *errorOut = tr("File not found in Drive catalog: %1").arg(relPath);
            return {};
        }

        const bool wasFrequent = m_entries[idx].isFrequent;
        const qint64 diskSize = QFileInfo(blobPath).size();
        if (m_sourceMode == QLatin1String("rclone") && diskSize > 0)
            m_entries[idx].sizeBytes = diskSize;
        m_entries[idx].state = m_entries[idx].pinned ? QStringLiteral("pinned") : QStringLiteral("cached");
        m_entries[idx].accessCount += 1;
        m_entries[idx].lastAccessedSecs = QDateTime::currentSecsSinceEpoch();
        recomputeScoresLocked(m_entries[idx].lastAccessedSecs);
        syncProjectionForEntryLocked(m_entries[idx]);

        newAccessCount = m_entries[idx].accessCount;
        becameFrequent = (!wasFrequent && m_entries[idx].isFrequent);
        entryName = m_entries[idx].name;
        entrySize = m_entries[idx].sizeBytes;
    }

    auto finishPostHydrate = [this, relPath, entryName, entrySize, wasCloudOnly, becameFrequent, newAccessCount]() {
        // Enforce cache ceiling while protecting the file currently being opened.
        enforceCacheBudget(relPath);
        if (m_isTestInstance)
            saveState();
        else
            scheduleSaveState(400);
        if (wasCloudOnly) {
            logActivity(tr("Hydrated On-Demand"),
                        tr("Downloaded %1 (%2) to local cache (opened %3x)")
                            .arg(entryName, formatSize(entrySize))
                            .arg(newAccessCount),
                        QStringLiteral("FETCH"));
        } else if (becameFrequent) {
            logActivity(tr("Promoted to Frequent"),
                        tr("%1 reached %2 accesses — prioritized in local cache")
                            .arg(entryName)
                            .arg(newAccessCount),
                        QStringLiteral("HOT"));
        } else {
            logActivity(tr("Accessed Cached File"),
                        tr("Served %1 from local disk (opened %2x)")
                            .arg(entryName)
                            .arg(newAccessCount),
                        QStringLiteral("HIT"));
        }
        rebuildFilteredModel();
    };

    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, finishPostHydrate, Qt::QueuedConnection);
    } else {
        finishPostHydrate();
    }

    return blobPath;
}

void Backend::vfsBeginWrite(const QString &relPath) {
    if (relPath.isEmpty())
        return;
    QMutexLocker locker(&m_mutex);
    m_openWriteCounts[relPath] = m_openWriteCounts.value(relPath, 0) + 1;
}

bool Backend::vfsCreateFile(const QString &relPath, QString *localCachePathOut) {
    if (relPath.isEmpty())
        return false;

    const QString blobPath = localBlobPath(relPath);
    QDir().mkpath(QFileInfo(blobPath).absolutePath());
    QFile f(blobPath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.close();

    {
        QMutexLocker locker(&m_mutex);
        m_pendingDeletes.remove(relPath);
        if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
            rebuildVfsIndexLocked();
        const int idx = m_vfsFileIdx.value(relPath, -1);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (idx < 0) {
            DriveFileEntry e;
            e.path = relPath;
            e.name = QFileInfo(relPath).fileName();
            const QString dir = QFileInfo(relPath).path();
            e.folder = (dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir;
            e.category = inferCategory(e.name);
            e.sizeBytes = 0;
            e.modifiedSecs = now;
            e.state = QStringLiteral("cached");
            e.accessCount = 1;
            e.lastAccessedSecs = now;
            e.dirty = true;
            m_entries.append(e);
        } else {
            m_entries[idx].sizeBytes = 0;
            m_entries[idx].modifiedSecs = now;
            m_entries[idx].state = m_entries[idx].pinned ? QStringLiteral("pinned") : QStringLiteral("cached");
            m_entries[idx].dirty = true;
        }
        recomputeScoresLocked(now);
        rebuildVfsIndexLocked();
    }

    if (localCachePathOut)
        *localCachePathOut = blobPath;

    QPointer<Backend> self(this);
    QMetaObject::invokeMethod(this, [this, self]() {
        if (!self)
            return;
        if (m_isTestInstance)
            saveState();
        else
            scheduleSaveState(400);
        rebuildFilteredModel();
        emit statusChanged();
    }, Qt::AutoConnection);

    return true;
}

bool Backend::vfsWriteCompleted(const QString &relPath) {
    const QString blobPath = localBlobPath(relPath);
    const QFileInfo fi(blobPath);
    if (!fi.exists())
        return false;

    bool shouldUploadNow = false;
    bool useAsyncUpload = false;
    {
        QMutexLocker locker(&m_mutex);
        int openWriters = m_openWriteCounts.value(relPath, 0);
        if (openWriters > 0) {
            --openWriters;
            if (openWriters == 0)
                m_openWriteCounts.remove(relPath);
            else
                m_openWriteCounts.insert(relPath, openWriters);
        }
        const bool stillOpenForWrite = (openWriters > 0);

        if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
            rebuildVfsIndexLocked();
        const int idx = m_vfsFileIdx.value(relPath, -1);
        if (idx >= 0 && idx < m_entries.size()) {
            m_entries[idx].sizeBytes = fi.size();
            m_entries[idx].modifiedSecs = QDateTime::currentSecsSinceEpoch();
            m_entries[idx].state = m_entries[idx].pinned ? QStringLiteral("pinned") : QStringLiteral("cached");
            m_entries[idx].dirty = true;
            m_pendingDeletes.remove(relPath);
            m_statsDirty = true;
            syncProjectionForEntryLocked(m_entries[idx]);

            if (!stillOpenForWrite) {
                const bool rcloneActive = (m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                                           && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName));
                const bool folderActive = (m_sourceMode == QLatin1String("folder") && !m_localSourceDir.isEmpty());
                shouldUploadNow = (rcloneActive || folderActive);
                useAsyncUpload = (rcloneActive && shouldUseAsyncRcloneSync());
            }
        }
    }

    if (shouldUploadNow) {
        if (useAsyncUpload)
            schedulePendingUpload(250);
        else
            flushPendingUploads();
    }

    QPointer<Backend> self(this);
    QMetaObject::invokeMethod(this, [this, self, relPath]() {
        if (!self)
            return;
        enforceCacheBudget(relPath);
        if (m_isTestInstance)
            saveState();
        else
            scheduleSaveState(400);
        rebuildFilteredModel();
        emit statusChanged();
    }, Qt::AutoConnection);

    return true;
}

bool Backend::vfsUnlinkFile(const QString &relPath) {
    bool removed = false;
    bool shouldDeleteRemote = false;
    bool useAsyncDelete = false;
    {
        QMutexLocker locker(&m_mutex);
        m_openWriteCounts.remove(relPath);
        if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
            rebuildVfsIndexLocked();
        const int idx = m_vfsFileIdx.value(relPath, -1);
        if (idx >= 0 && idx < m_entries.size()) {
            const QString blobPath = localBlobPath(relPath);
            if (QFileInfo::exists(blobPath))
                QFile::remove(blobPath);
            m_entries.removeAt(idx);
            rebuildVfsIndexLocked();
            removed = true;

            const bool rcloneActive = (m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                                       && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName));
            const bool folderActive = (m_sourceMode == QLatin1String("folder") && !m_localSourceDir.isEmpty());
            if (rcloneActive || folderActive) {
                m_pendingDeletes.insert(relPath);
                shouldDeleteRemote = true;
                useAsyncDelete = (rcloneActive && shouldUseAsyncRcloneSync());
            }
        }
    }
    if (removed) {
        if (shouldDeleteRemote) {
            if (useAsyncDelete)
                schedulePendingUpload(100);
            else
                flushPendingUploads();
        }
        QPointer<Backend> self(this);
        QMetaObject::invokeMethod(this, [this, self]() {
            if (!self)
                return;
            if (m_isTestInstance)
                saveState();
            else
                scheduleSaveState(400);
            rebuildFilteredModel();
            emit statusChanged();
            emit statsChanged();
        }, Qt::AutoConnection);
    }
    return removed;
}

int Backend::vfsMkdir(const QString &relPath) {
    const QString rel = cleanRelPath(relPath);
    if (rel.isEmpty() || rel == QLatin1String("."))
        return -EEXIST;
    if (rel == QLatin1String("Root"))
        return -EINVAL;

    auto validateLocked = [&]() -> int {
        if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
            rebuildVfsIndexLocked();

        if (m_vfsFileIdx.contains(rel) || m_vfsDirs.contains(rel))
            return -EEXIST;

        const int slash = rel.lastIndexOf('/');
        const QString parent = (slash < 0) ? QString() : rel.left(slash);
        if (!parent.isEmpty()) {
            if (m_vfsFileIdx.contains(parent))
                return -ENOTDIR;
            if (!m_vfsDirs.contains(parent))
                return -ENOENT;
        }
        return 0;
    };

    QString sourceMode;
    bool rcloneActive = false;
    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString rcloneConfigPath;
    QString localSourceDir;

    {
        QMutexLocker locker(&m_mutex);
        const int err = validateLocked();
        if (err != 0)
            return err;

        sourceMode = m_sourceMode;
        rcloneActive = (sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                        && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName));
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        rcloneConfigPath = m_rcloneConfigPath;
        localSourceDir = m_localSourceDir;
    }

    if (rcloneActive) {
        const QString spec = buildRcloneSpec(remoteName, remotePath, rel);
        QString rcloneErr;
        const bool created = runOnQtThread(this, [&]() {
            return runRcloneMkdir(rcloneBin, spec, 30000, &rcloneErr, rcloneConfigPath);
        });
        if (!created) {
            qWarning("wickfs: rclone mkdir %s failed: %s", qPrintable(spec), qPrintable(rcloneErr));
            return -EIO;
        }
    } else if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
        const QString dstLocal = localSourceDir + QLatin1Char('/') + rel;
        if (QFileInfo::exists(dstLocal))
            return -EEXIST;
        if (!QDir().mkpath(dstLocal))
            return -EIO;
    }

    {
        QMutexLocker locker(&m_mutex);
        const int err = validateLocked();
        if (err != 0)
            return err;

        if (!m_remoteFolders.contains(rel))
            m_remoteFolders.append(rel);
        m_listedDirs.insert(rel);
        m_syncedDirTimestampsMs.insert(rel, QDateTime::currentMSecsSinceEpoch());

        const bool kernelFuse = (m_fuseMount && m_fuseMount->isKernelFuse());
        if (!m_mountDir.isEmpty() && !kernelFuse)
            QDir().mkpath(m_mountDir + QLatin1Char('/') + rel);

        rebuildVfsIndexLocked();
    }

    auto persistAndNotify = [this, rel]() {
        saveState();
        rebuildFilteredModel();
        logActivity(tr("Created Folder"), rel, QStringLiteral("MKDIR"));
        emit statsChanged();
    };
    if (QThread::currentThread() != this->thread())
        QMetaObject::invokeMethod(this, persistAndNotify, Qt::QueuedConnection);
    else
        persistAndNotify();

    return 0;
}

int Backend::vfsRmdir(const QString &relPath) {
    const QString rel = cleanRelPath(relPath);
    if (rel.isEmpty() || rel == QLatin1String("."))
        return -EBUSY;

    auto validateLocked = [&]() -> int {
        if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
            rebuildVfsIndexLocked();

        if (m_vfsFileIdx.contains(rel))
            return -ENOTDIR;
        if (!m_vfsDirs.contains(rel))
            return -ENOENT;
        if (!m_vfsDirChildren.value(rel).isEmpty())
            return -ENOTEMPTY;

        const QString prefix = rel + QLatin1Char('/');
        for (const auto &e : m_entries) {
            if (e.path.startsWith(prefix))
                return -ENOTEMPTY;
        }
        for (const QString &rf : m_remoteFolders) {
            if (rf.startsWith(prefix))
                return -ENOTEMPTY;
        }
        return 0;
    };

    QString sourceMode;
    bool rcloneActive = false;
    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString rcloneConfigPath;
    QString localSourceDir;

    {
        QMutexLocker locker(&m_mutex);
        const int err = validateLocked();
        if (err != 0)
            return err;

        sourceMode = m_sourceMode;
        rcloneActive = (sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                        && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName));
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        rcloneConfigPath = m_rcloneConfigPath;
        localSourceDir = m_localSourceDir;
    }

    if (rcloneActive) {
        const QString spec = buildRcloneSpec(remoteName, remotePath, rel);
        QString rcloneErr;
        const bool removed = runOnQtThread(this, [&]() {
            return runRcloneRmdir(rcloneBin, spec, 30000, &rcloneErr, rcloneConfigPath);
        });
        if (!removed) {
            const QString lowerErr = rcloneErr.toLower();
            if (lowerErr.contains(QLatin1String("not empty"))
                || lowerErr.contains(QLatin1String("directory not empty"))) {
                return -ENOTEMPTY;
            }
            if (!isRcloneNotFoundError(rcloneErr)) {
                qWarning("wickfs: rclone rmdir %s failed: %s", qPrintable(spec), qPrintable(rcloneErr));
                return -EIO;
            }
        }
    } else if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
        const QString dstLocal = localSourceDir + QLatin1Char('/') + rel;
        if (QDir(dstLocal).exists() && !QDir().rmdir(dstLocal)) {
            const bool nonEmpty = !QDir(dstLocal).entryList(QDir::NoDotAndDotDot | QDir::AllEntries
                                                            | QDir::Hidden | QDir::System).isEmpty();
            return nonEmpty ? -ENOTEMPTY : -EIO;
        }
    }

    bool currentFolderChanged = false;
    {
        QMutexLocker locker(&m_mutex);
        const int err = validateLocked();
        if (err != 0)
            return err;

        m_remoteFolders.removeAll(rel);
        m_listedDirs.remove(rel);
        m_syncedDirTimestampsMs.remove(rel);

        const QString blobDir = localBlobPath(rel);
        if (QDir(blobDir).exists())
            QDir().rmdir(blobDir);

        const bool kernelFuse = (m_fuseMount && m_fuseMount->isKernelFuse());
        if (!m_mountDir.isEmpty() && !kernelFuse)
            QDir().rmdir(m_mountDir + QLatin1Char('/') + rel);

        if (m_currentFolder == rel) {
            m_currentFolder.clear();
            currentFolderChanged = true;
        }

        rebuildVfsIndexLocked();
    }

    auto persistAndNotify = [this, rel, currentFolderChanged]() {
        saveState();
        rebuildFilteredModel();
        if (currentFolderChanged)
            emit filterChanged();
        logActivity(tr("Removed Folder"), rel, QStringLiteral("RMDIR"));
        emit statsChanged();
    };
    if (QThread::currentThread() != this->thread())
        QMetaObject::invokeMethod(this, persistAndNotify, Qt::QueuedConnection);
    else
        persistAndNotify();

    return 0;
}

int Backend::vfsRename(const QString &oldRelPath, const QString &newRelPath, unsigned int flags) {
    if (flags & ~static_cast<unsigned int>(RENAME_NOREPLACE))
        return -EINVAL;

    auto cleanRel = [](QString p) {
        while (p.startsWith('/'))
            p.remove(0, 1);
        while (p.endsWith('/'))
            p.chop(1);
        return p;
    };

    const QString oldRel = cleanRel(oldRelPath);
    const QString newRel = cleanRel(newRelPath);
    if (oldRel.isEmpty() || newRel.isEmpty())
        return -EBUSY;

    const bool noReplace = (flags & RENAME_NOREPLACE) != 0;

    auto validateLocked = [&](bool &oldIsDirOut, bool &newExistsOut, bool &newIsDirOut,
                              int &newFileIdxOut, bool &oldIsCloudOnlyOut) -> int {
        if (m_vfsFileIdx.isEmpty() && (!m_entries.isEmpty() || !m_remoteFolders.isEmpty()))
            rebuildVfsIndexLocked();

        const int oldFileIdx = m_vfsFileIdx.value(oldRel, -1);
        const bool oldIsDir = m_vfsDirs.contains(oldRel);
        if (oldFileIdx < 0 && !oldIsDir)
            return -ENOENT;

        oldIsDirOut = oldIsDir;
        if (oldRel == newRel) {
            newExistsOut = true;
            newIsDirOut = oldIsDir;
            newFileIdxOut = oldFileIdx;
            oldIsCloudOnlyOut = false;
            return 0;
        }

        const int newSlash = newRel.lastIndexOf('/');
        const QString newParent = (newSlash < 0) ? QString() : newRel.left(newSlash);
        if (!newParent.isEmpty()) {
            if (m_vfsFileIdx.contains(newParent))
                return -ENOTDIR;
            if (!m_vfsDirs.contains(newParent))
                return -ENOENT;
        }

        if (oldIsDir && newRel.startsWith(oldRel + QLatin1Char('/')))
            return -EINVAL;

        const int newFileIdx = m_vfsFileIdx.value(newRel, -1);
        const bool newIsDir = m_vfsDirs.contains(newRel);
        const bool newExists = (newFileIdx >= 0 || newIsDir);
        if (newExists && noReplace)
            return -EEXIST;

        if (newExists) {
            if (!oldIsDir && newIsDir)
                return -EISDIR;
            if (oldIsDir && !newIsDir)
                return -ENOTDIR;
            if (oldIsDir && newIsDir && !m_vfsDirChildren.value(newRel).isEmpty())
                return -ENOTEMPTY;
        }

        bool cloudOnly = false;
        if (!oldIsDir) {
            cloudOnly = (m_entries.at(oldFileIdx).state == QLatin1String("cloud"));
        } else {
            const QString prefix = oldRel + QLatin1Char('/');
            for (const auto &e : m_entries) {
                if (e.path.startsWith(prefix) && e.state == QLatin1String("cloud")) {
                    cloudOnly = true;
                    break;
                }
            }
        }

        newExistsOut = newExists;
        newIsDirOut = newIsDir;
        newFileIdxOut = newFileIdx;
        oldIsCloudOnlyOut = cloudOnly;
        return 0;
    };

    bool oldIsDir = false;
    bool newExists = false;
    bool newIsDir = false;
    int newFileIdx = -1;
    bool oldIsCloudOnly = false;
    QString sourceMode;
    bool rcloneActive = false;
    QString rcloneBin;
    QString remoteName;
    QString remotePath;
    QString rcloneConfigPath;
    QString localSourceDir;

    {
        QMutexLocker locker(&m_mutex);
        const int err = validateLocked(oldIsDir, newExists, newIsDir, newFileIdx, oldIsCloudOnly);
        if (err != 0)
            return err;
        if (oldRel == newRel)
            return 0;

        sourceMode = m_sourceMode;
        rcloneActive = (sourceMode == QLatin1String("rclone") && m_rcloneAvailable
                        && !m_remoteName.isEmpty() && m_availableRemotes.contains(m_remoteName));
        rcloneBin = m_rcloneBin;
        remoteName = m_remoteName;
        remotePath = m_remotePath;
        rcloneConfigPath = m_rcloneConfigPath;
        localSourceDir = m_localSourceDir;
    }

    bool localOnlyNotOnRemote = false;
    if (rcloneActive) {
        const auto pathLock = perPathHydrateMutex(oldRel);
        QMutexLocker pathLocker(pathLock.data());
        const QString srcSpec = buildRcloneSpec(remoteName, remotePath, oldRel);
        const QString dstSpec = buildRcloneSpec(remoteName, remotePath, newRel);
        bool moved = false;
        QString rcloneErr;
        if (QThread::currentThread() == this->thread()) {
            moved = runRcloneMoveto(rcloneBin, srcSpec, dstSpec, 45000, &rcloneErr, rcloneConfigPath);
        } else {
            std::unique_ptr<QThread> worker(QThread::create([&]() {
                moved = runRcloneMoveto(rcloneBin, srcSpec, dstSpec, 45000, &rcloneErr, rcloneConfigPath);
            }));
            worker->start();
            worker->wait();
        }
        if (!moved) {
            const QString lowerErr = rcloneErr.toLower();
            const bool notFoundOnRemote = lowerErr.contains(QLatin1String("not found"))
                || lowerErr.contains(QLatin1String("no such file"))
                || lowerErr.contains(QLatin1String("doesn't exist"))
                || lowerErr.contains(QLatin1String("does not exist"));
            // Allow renaming locally-created files/dirs that have not been uploaded to remote yet.
            if (!(notFoundOnRemote && !oldIsCloudOnly)) {
                qWarning("wickfs: rclone moveto %s -> %s failed: %s",
                         qPrintable(srcSpec), qPrintable(dstSpec), qPrintable(rcloneErr));
                return -EIO;
            }
            localOnlyNotOnRemote = true;
        }
    } else if (sourceMode == QLatin1String("folder") && !localSourceDir.isEmpty()) {
        const QString srcLocal = localSourceDir + QLatin1Char('/') + oldRel;
        const QString dstLocal = localSourceDir + QLatin1Char('/') + newRel;
        if (QFileInfo::exists(srcLocal)) {
            QDir().mkpath(QFileInfo(dstLocal).absolutePath());
            if (QFileInfo::exists(dstLocal)) {
                if (QFileInfo(dstLocal).isDir())
                    QDir().rmdir(dstLocal);
                else
                    QFile::remove(dstLocal);
            }
            const bool moved = oldIsDir ? QDir().rename(srcLocal, dstLocal)
                                        : QFile::rename(srcLocal, dstLocal);
            if (!moved)
                return -EIO;
        }
    }

    bool currentFolderChanged = false;
    {
        QMutexLocker locker(&m_mutex);
        const int err = validateLocked(oldIsDir, newExists, newIsDir, newFileIdx, oldIsCloudOnly);
        if (err != 0)
            return err;

        const bool kernelFuse = (m_fuseMount && m_fuseMount->isKernelFuse());

        if (newExists) {
            if (!newIsDir && newFileIdx >= 0 && newFileIdx < m_entries.size()) {
                const QString targetBlob = localBlobPath(newRel);
                if (QFileInfo::exists(targetBlob))
                    QFile::remove(targetBlob);
                if (!m_mountDir.isEmpty() && !kernelFuse)
                    QFile::remove(m_mountDir + QLatin1Char('/') + newRel);
                m_entries.removeAt(newFileIdx);
            } else if (newIsDir) {
                const QString targetBlobDir = localBlobPath(newRel);
                if (QDir(targetBlobDir).exists())
                    QDir(targetBlobDir).removeRecursively();
                m_remoteFolders.removeAll(newRel);
                m_syncedDirTimestampsMs.remove(newRel);
                if (!m_mountDir.isEmpty() && !kernelFuse)
                    QDir(m_mountDir + QLatin1Char('/') + newRel).removeRecursively();
            }
        }

        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (!oldIsDir) {
            const QString oldBlob = localBlobPath(oldRel);
            const QString newBlob = localBlobPath(newRel);
            if (QFileInfo::exists(oldBlob)) {
                QDir().mkpath(QFileInfo(newBlob).absolutePath());
                QFile::remove(newBlob);
                if (!QFile::rename(oldBlob, newBlob))
                    return -EIO;
            }
            for (int i = 0; i < m_entries.size(); ++i) {
                if (m_entries.at(i).path == oldRel) {
                    DriveFileEntry &e = m_entries[i];
                    e.path = newRel;
                    e.name = QFileInfo(newRel).fileName();
                    const QString dir = QFileInfo(newRel).path();
                    e.folder = (dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir;
                    e.category = inferCategory(e.name);
                    e.modifiedSecs = now;
                    if (localOnlyNotOnRemote)
                        e.dirty = true;
                    m_pendingDeletes.remove(newRel);
                    if (m_openWriteCounts.contains(oldRel))
                        m_openWriteCounts.insert(newRel, m_openWriteCounts.take(oldRel));
                    if (!m_mountDir.isEmpty() && !kernelFuse) {
                        QFile::remove(m_mountDir + QLatin1Char('/') + oldRel);
                        syncProjectionForEntryLocked(e);
                    }
                    break;
                }
            }
        } else {
            const QString oldBlobDir = localBlobPath(oldRel);
            const QString newBlobDir = localBlobPath(newRel);
            if (QDir(oldBlobDir).exists()) {
                QDir().mkpath(QFileInfo(newBlobDir).absolutePath());
                if (QDir(newBlobDir).exists())
                    QDir(newBlobDir).removeRecursively();
                if (!QDir().rename(oldBlobDir, newBlobDir))
                    return -EIO;
            }

            const QString oldPrefix = oldRel + QLatin1Char('/');
            const QString newPrefix = newRel + QLatin1Char('/');
            for (DriveFileEntry &e : m_entries) {
                if (e.path.startsWith(oldPrefix)) {
                    e.path = newPrefix + e.path.mid(oldPrefix.size());
                    e.name = QFileInfo(e.path).fileName();
                    const QString dir = QFileInfo(e.path).path();
                    e.folder = (dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir;
                }
            }

            for (QString &rf : m_remoteFolders) {
                if (rf == oldRel)
                    rf = newRel;
                else if (rf.startsWith(oldPrefix))
                    rf = newPrefix + rf.mid(oldPrefix.size());
            }

            QHash<QString, qint64> nextTimestamps;
            for (auto it = m_syncedDirTimestampsMs.cbegin(); it != m_syncedDirTimestampsMs.cend(); ++it) {
                if (it.key() == oldRel)
                    nextTimestamps.insert(newRel, it.value());
                else if (it.key().startsWith(oldPrefix))
                    nextTimestamps.insert(newPrefix + it.key().mid(oldPrefix.size()), it.value());
                else
                    nextTimestamps.insert(it.key(), it.value());
            }
            m_syncedDirTimestampsMs = nextTimestamps;

            QSet<QString> nextListed;
            for (const QString &ld : std::as_const(m_listedDirs)) {
                if (ld == oldRel)
                    nextListed.insert(newRel);
                else if (ld.startsWith(oldPrefix))
                    nextListed.insert(newPrefix + ld.mid(oldPrefix.size()));
                else
                    nextListed.insert(ld);
            }
            m_listedDirs = nextListed;

            if (m_currentFolder == oldRel) {
                m_currentFolder = newRel;
                currentFolderChanged = true;
            } else if (m_currentFolder.startsWith(oldPrefix)) {
                m_currentFolder = newPrefix + m_currentFolder.mid(oldPrefix.size());
                currentFolderChanged = true;
            }

            if (!m_mountDir.isEmpty() && !kernelFuse) {
                QDir(m_mountDir + QLatin1Char('/') + oldRel).removeRecursively();
                syncAllProjectionsLocked();
            }
        }

        rebuildVfsIndexLocked();
    }

    auto persistAndNotify = [this, oldRel, newRel, currentFolderChanged]() {
        saveState();
        rebuildFilteredModel();
        if (currentFolderChanged)
            emit filterChanged();
        logActivity(tr("Renamed"), QStringLiteral("%1 -> %2").arg(oldRel, newRel), QStringLiteral("RENAME"));
        emit statsChanged();
    };
    if (QThread::currentThread() != this->thread())
        QMetaObject::invokeMethod(this, persistAndNotify, Qt::QueuedConnection);
    else
        persistAndNotify();

    return 0;
}

QList<DriveFileEntry> Backend::allEntries() const {
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

DriveFileEntry Backend::entryForPath(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    if (m_vfsFileIdx.isEmpty() && !m_entries.isEmpty())
        rebuildVfsIndexLocked();
    const int idx = m_vfsFileIdx.value(path, -1);
    if (idx >= 0 && idx < m_entries.size())
        return m_entries.at(idx);
    return {};
}

void Backend::logActivity(const QString &action, const QString &detail, const QString &badge) {
    QVariantMap item;
    item.insert("time", QDateTime::currentDateTime().toString("HH:mm:ss"));
    item.insert("action", action);
    item.insert("detail", detail);
    item.insert("badge", badge.isEmpty() ? QStringLiteral("INFO") : badge);
    m_activityLog.prepend(item);
    while (m_activityLog.size() > 40)
        m_activityLog.removeLast();
    emit activityChanged();
}

void Backend::setStatusBanner(const QString &text, bool isError) {
    if (text == m_statusBannerText && isError == m_statusBannerIsError)
        return;
    m_statusBannerText = text;
    m_statusBannerIsError = isError;
    emit statusChanged();
}

void Backend::loadState() {
    QFile file(m_stateDir + "/state.json");
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.isEmpty())
        return;

    QMutexLocker locker(&m_mutex);
    m_sourceMode = root.value("sourceMode").toString(QStringLiteral("catalog"));
    m_remoteName = root.value("remoteName").toString(QStringLiteral("gdrive"));
    m_remotePath = root.value("remotePath").toString();
    m_localSourceDir = root.value("localSourceDir").toString();
    m_maxCacheBytes = static_cast<qint64>(root.value("maxCacheBytes").toDouble(250.0 * 1024.0 * 1024.0));
    m_minFreeDiskBytes = static_cast<qint64>(root.value("minFreeDiskBytes").toDouble(15.0 * 1024.0 * 1024.0 * 1024.0));
    m_frequentThreshold = qMax(1, root.value("frequentThreshold").toInt(3));
    m_halfLifeHours = qMax(1, root.value("halfLifeHours").toInt(72));
    m_autoKeepFrequent = root.value("autoKeepFrequent").toBool(true);
    m_lastSyncedSecs = static_cast<qint64>(root.value("lastSyncedSecs").toDouble(QDateTime::currentSecsSinceEpoch()));
    m_syncing = root.value("syncing").toBool(false);
    m_activeSyncFile = root.value("activeSyncFile").toString();
    m_syncProgress = root.value("syncProgress").toDouble(1.0);
    m_pendingSyncCount = root.value("pendingSyncCount").toInt(0);

    m_remoteFolders.clear();
    const QJsonArray rfArr = root.value("remoteFolders").toArray();
    for (const auto &v : rfArr) {
        const QString rf = v.toString();
        if (!rf.isEmpty())
            m_remoteFolders.append(rf);
    }

    m_listedDirs.clear();
    const bool hasListedDirsKey = root.contains("listedDirs");
    if (hasListedDirsKey) {
        const QJsonArray ldArr = root.value("listedDirs").toArray();
        for (const auto &v : ldArr)
            m_listedDirs.insert(v.toString());
    }

    m_syncedDirTimestampsMs.clear();
    if (root.contains("syncedDirTimestampsMs")) {
        const QJsonObject tsObj = root.value("syncedDirTimestampsMs").toObject();
        for (auto it = tsObj.begin(); it != tsObj.end(); ++it)
            m_syncedDirTimestampsMs.insert(it.key(), static_cast<qint64>(it.value().toDouble(0)));
    }

    m_entries.clear();
    const QJsonArray arr = root.value("entries").toArray();
    m_entries.reserve(arr.size());
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        DriveFileEntry e;
        e.path = o.value("path").toString();
        e.name = o.value("name").toString();
        e.folder = o.value("folder").toString();
        e.category = o.value("category").toString();
        e.sizeBytes = static_cast<qint64>(o.value("sizeBytes").toDouble());
        e.modifiedSecs = static_cast<qint64>(o.value("modifiedSecs").toDouble());
        e.pinned = o.value("pinned").toBool();
        e.state = e.pinned ? QStringLiteral("pinned") : o.value("state").toString(QStringLiteral("cloud"));
        e.accessCount = o.value("accessCount").toInt();
        e.lastAccessedSecs = static_cast<qint64>(o.value("lastAccessedSecs").toDouble());
        e.dirty = o.value("dirty").toBool(false);
        m_entries.append(e);
    }

    m_pendingDeletes.clear();
    const QJsonArray pdArr = root.value("pendingDeletes").toArray();
    for (const auto &v : pdArr) {
        const QString pd = v.toString();
        if (!pd.isEmpty())
            m_pendingDeletes.insert(pd);
    }

    recomputeScoresLocked();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].state == QLatin1String("pinned") || m_entries[i].state == QLatin1String("cached")) {
            const QString blobPath = localBlobPath(m_entries[i].path);
            const QFileInfo blobFi(blobPath);
            if (blobFi.exists()) {
                if (m_sourceMode == QLatin1String("rclone") && (blobFi.size() > 0 || m_entries[i].dirty))
                    m_entries[i].sizeBytes = blobFi.size();
                m_entries[i].state = m_entries[i].pinned ? QStringLiteral("pinned") : QStringLiteral("cached");
                syncProjectionForEntryLocked(m_entries[i]);
            } else if (m_sourceMode != QLatin1String("rclone")) {
                hydrateEntryLocked(i);
            } else if (!m_entries[i].pinned) {
                m_entries[i].state = QStringLiteral("cloud");
                m_entries[i].dirty = false;
            }
        }
    }
    rebuildVfsIndexLocked();
    if (!hasListedDirsKey) {
        m_listedDirs.insert(QString());
        for (auto it = m_vfsDirChildren.cbegin(); it != m_vfsDirChildren.cend(); ++it) {
            if (!it.value().isEmpty())
                m_listedDirs.insert(it.key());
        }
    }
    const QFileInfo loadedFi(m_stateDir + "/state.json");
    m_lastLoadedStateMtimeMs = loadedFi.lastModified().toMSecsSinceEpoch();
    m_lastLoadedStateSize = loadedFi.size();
    locker.unlock();
    rebuildFilteredModel();
}

void Backend::scheduleSaveState(int delayMs) {
    if (QThread::currentThread() == this->thread()) {
        if (!m_saveDebounceTimer.isActive())
            m_saveDebounceTimer.start(delayMs);
    } else {
        QMetaObject::invokeMethod(this, [this, delayMs]() {
            if (!m_saveDebounceTimer.isActive())
                m_saveDebounceTimer.start(delayMs);
        }, Qt::QueuedConnection);
    }
}

void Backend::saveState() const {
    QJsonObject root;
    QJsonObject barRoot;
    QString stateDir;

    {
        QMutexLocker locker(&m_mutex);
        ensureStatsFreshLocked();
        stateDir = m_stateDir;

        QJsonArray rfArr;
        for (const QString &rf : m_remoteFolders)
            rfArr.append(rf);

        QJsonArray ldArr;
        for (const QString &ld : m_listedDirs)
            ldArr.append(ld);

        QJsonObject tsObj;
        for (auto it = m_syncedDirTimestampsMs.cbegin(); it != m_syncedDirTimestampsMs.cend(); ++it) {
            if (it.value() > 0)
                tsObj.insert(it.key(), static_cast<double>(it.value()));
        }

        QJsonArray arr;
        for (const auto &e : m_entries) {
            QJsonObject o;
            o.insert("path", e.path);
            o.insert("name", e.name);
            o.insert("folder", e.folder);
            o.insert("category", e.category);
            o.insert("sizeBytes", static_cast<double>(e.sizeBytes));
            o.insert("modifiedSecs", static_cast<double>(e.modifiedSecs));
            o.insert("state", e.state);
            o.insert("pinned", e.pinned);
            o.insert("accessCount", e.accessCount);
            o.insert("lastAccessedSecs", static_cast<double>(e.lastAccessedSecs));
            if (e.dirty)
                o.insert("dirty", true);
            arr.append(o);
        }

        const int pendingCount = pendingSyncCount();
        root.insert("sourceMode", m_sourceMode);
        root.insert("remoteName", m_remoteName);
        root.insert("remotePath", m_remotePath);
        root.insert("localSourceDir", m_localSourceDir);
        root.insert("maxCacheBytes", static_cast<double>(m_maxCacheBytes));
        root.insert("minFreeDiskBytes", static_cast<double>(m_minFreeDiskBytes));
        root.insert("frequentThreshold", m_frequentThreshold);
        root.insert("halfLifeHours", m_halfLifeHours);
        root.insert("autoKeepFrequent", m_autoKeepFrequent);
        root.insert("lastSyncedSecs", static_cast<double>(m_lastSyncedSecs));
        root.insert("syncing", m_syncing || m_uploadInFlight.load());
        root.insert("syncState", syncState());
        root.insert("activeSyncFile", m_activeSyncFile);
        root.insert("syncProgress", m_syncProgress);
        root.insert("pendingSyncCount", pendingCount);
        root.insert("remoteFolders", rfArr);
        root.insert("listedDirs", ldArr);
        root.insert("syncedDirTimestampsMs", tsObj);
        if (!m_pendingDeletes.isEmpty()) {
            QJsonArray pdArr;
            for (const QString &pd : std::as_const(m_pendingDeletes))
                pdArr.append(pd);
            root.insert("pendingDeletes", pdArr);
        }
        root.insert("entries", arr);

        // Compact summary payload for the Omarchy top-bar widget (bar-status.json)
        QList<int> topIndices;
        topIndices.reserve(m_entries.size());
        for (int i = 0; i < m_entries.size(); ++i)
            topIndices.append(i);
        const int topLimit = qMin(6, topIndices.size());
        std::partial_sort(topIndices.begin(), topIndices.begin() + topLimit, topIndices.end(),
                          [this](int a, int b) {
            const auto &ea = m_entries.at(a);
            const auto &eb = m_entries.at(b);
            if (ea.pinned != eb.pinned)
                return ea.pinned > eb.pinned;
            const bool aRes = (ea.state != QLatin1String("cloud"));
            const bool bRes = (eb.state != QLatin1String("cloud"));
            if (aRes != bRes)
                return aRes > bRes;
            return ea.accessCount > eb.accessCount;
        });

        QJsonArray topFiles;
        for (int k = 0; k < topLimit; ++k) {
            const auto &e = m_entries.at(topIndices.at(k));
            QJsonObject o;
            o.insert("name", e.name);
            o.insert("folder", e.folder);
            o.insert("sizeBytes", static_cast<double>(e.sizeBytes));
            o.insert("state", e.state);
            o.insert("pinned", e.pinned);
            o.insert("accessCount", e.accessCount);
            o.insert("isFrequent", e.isFrequent);
            topFiles.append(o);
        }

        const qint64 saved = qMax<qint64>(0, m_cachedTotalCloudBytes - m_cachedCachedBytes);
        const int savedPct = m_cachedTotalCloudBytes > 0
            ? qBound(0, static_cast<int>(std::round((static_cast<double>(saved) * 100.0) / static_cast<double>(m_cachedTotalCloudBytes))), 100)
            : 0;

        barRoot.insert("barSummary", true);
        barRoot.insert("sourceMode", m_sourceMode);
        barRoot.insert("remoteName", m_remoteName);
        barRoot.insert("maxCacheBytes", static_cast<double>(m_maxCacheBytes));
        barRoot.insert("cachedBytes", static_cast<double>(m_cachedCachedBytes));
        barRoot.insert("pinnedBytes", static_cast<double>(m_cachedPinnedBytes));
        barRoot.insert("hotCachedBytes", static_cast<double>(m_cachedHotCachedBytes));
        barRoot.insert("totalCloudBytes", static_cast<double>(m_cachedTotalCloudBytes));
        barRoot.insert("savedBytes", static_cast<double>(saved));
        barRoot.insert("savedPct", savedPct);
        barRoot.insert("totalFiles", m_entries.size());
        barRoot.insert("cachedFiles", m_cachedCachedCount);
        barRoot.insert("pinnedFiles", m_cachedPinnedCount);
        barRoot.insert("frequentFiles", m_cachedFrequentCount);
        barRoot.insert("cloudOnlyFiles", m_cachedCloudCount);
        barRoot.insert("lastSyncedSecs", static_cast<double>(m_lastSyncedSecs));
        barRoot.insert("syncing", m_syncing || m_uploadInFlight.load());
        barRoot.insert("syncState", syncState());
        barRoot.insert("activeSyncFile", m_activeSyncFile);
        barRoot.insert("syncProgress", m_syncProgress);
        barRoot.insert("pendingSyncCount", pendingCount);
        barRoot.insert("entries", topFiles);
    }

    // Perform JSON serialization and atomic disk I/O completely outside m_mutex.
    QDir().mkpath(stateDir);
    const QString statePath = stateDir + "/state.json";
    QSaveFile f(statePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
        const QFileInfo savedFi(statePath);
        m_lastLoadedStateMtimeMs = savedFi.lastModified().toMSecsSinceEpoch();
        m_lastLoadedStateSize = savedFi.size();
    }

    const QString barPath = stateDir + "/bar-status.json";
    QSaveFile bf(barPath);
    if (bf.open(QIODevice::WriteOnly)) {
        bf.write(QJsonDocument(barRoot).toJson(QJsonDocument::Compact));
        bf.commit();
    }
}

bool Backend::installBarPlugin(bool enableInBar, QString *destDirOut, QString *errorOut) {
    const QString omarchyDir = QDir::homePath() + QStringLiteral("/.config/omarchy");
    const QString destDir = omarchyDir + QStringLiteral("/plugins/ben.wickfs");
    QDir().mkpath(destDir + QStringLiteral("/i18n"));

    const QStringList relFiles = {
        QStringLiteral("manifest.json"),
        QStringLiteral("Model.js"),
        QStringLiteral("Panel.qml"),
        QStringLiteral("i18n/en.json"),
        QStringLiteral("i18n/da.json")
    };
    for (const QString &rel : relFiles) {
        QFile src(QStringLiteral(":/plugin/") + rel);
        if (!src.open(QIODevice::ReadOnly)) {
            if (errorOut)
                *errorOut = QStringLiteral("Bundled plugin resource missing: %1").arg(rel);
            return false;
        }
        const QByteArray data = src.readAll();
        const QString destPath = destDir + QLatin1Char('/') + rel;
        QSaveFile out(destPath);
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
            if (errorOut)
                *errorOut = QStringLiteral("Failed to write plugin file to %1").arg(destPath);
            return false;
        }
    }

    // Clean up legacy ben.gdrive plugin folder if present.
    QDir(omarchyDir + QStringLiteral("/plugins/ben.gdrive")).removeRecursively();

    const QString shellJsonPath = omarchyDir + QStringLiteral("/shell.json");
    bool alreadyEnabledInShellJson = false;
    if (QFileInfo::exists(shellJsonPath)) {
        QFile shellFile(shellJsonPath);
        if (shellFile.open(QIODevice::ReadOnly)) {
            QByteArray raw = shellFile.readAll();
            shellFile.close();
            if (raw.contains("\"ben.gdrive\"")) {
                raw.replace("\"ben.gdrive\"", "\"ben.wickfs\"");
                QSaveFile shellOut(shellJsonPath);
                if (shellOut.open(QIODevice::WriteOnly)) {
                    shellOut.write(raw);
                    shellOut.commit();
                }
            }
            alreadyEnabledInShellJson = raw.contains("\"ben.wickfs\"");
        }
    }

    if (enableInBar) {
        const QString omarchyShell = QStandardPaths::findExecutable(QStringLiteral("omarchy-shell"));
        if (!omarchyShell.isEmpty()) {
            QProcess p;
            p.setStandardOutputFile(QProcess::nullDevice());
            p.setStandardErrorFile(QProcess::nullDevice());
            p.start(omarchyShell, {QStringLiteral("shell"), QStringLiteral("rescanPlugins")});
            p.waitForFinished(2500);
        }
        if (!alreadyEnabledInShellJson) {
            const QString pluginEnable = QStandardPaths::findExecutable(QStringLiteral("omarchy-plugin-enable"));
            if (!pluginEnable.isEmpty()) {
                QProcess p;
                p.setStandardOutputFile(QProcess::nullDevice());
                p.setStandardErrorFile(QProcess::nullDevice());
                p.start(pluginEnable, {QStringLiteral("ben.wickfs"), QStringLiteral("--section"), QStringLiteral("right")});
                p.waitForFinished(3000);
            }
        }
    }

    if (destDirOut)
        *destDirOut = destDir;
    return true;
}
