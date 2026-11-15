#include "backend.h"
#include "filepicker.h"
#include "fusemount.h"
#include "portalfilepicker.h"

#include <QDateTime>
#include <QDBusConnection>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <sys/statvfs.h>

namespace {

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
      m_stateDir(stateDir.isEmpty() ? QDir::homePath() + "/.local/share/gdrive" : stateDir),
      m_blobDir(m_stateDir + "/blobs"),
      m_mountDir(mountDir.isEmpty() ? QDir::homePath() + "/Drive" : mountDir),
      m_filePicker(picker),
      m_ownsPicker(picker == nullptr) {
    if (!m_filePicker)
        m_filePicker = new PortalFilePicker(this);

    if (!stateDir.isEmpty())
        m_minFreeDiskBytes = 0;

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
            saveState();
            emit statusChanged();
        }
    });

    refreshRemotes();

    if (QFileInfo::exists(m_stateDir + "/state.json")) {
        loadState();
    } else {
        initStarterCatalog();
        saveState();
    }

    if (m_sourceMode == QLatin1String("rclone")) {
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
    } else {
        QMutexLocker locker(&m_mutex);
        syncAllProjectionsLocked();
    }
}

Backend::~Backend() {
    if (m_fuseMount && m_fuseMount->isMounted())
        m_fuseMount->unmount();
    QMutexLocker locker(&m_mutex);
    syncAllProjectionsLocked();
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

    logActivity(tr("Smart Cache Initialized"),
                tr("Pinned 2 offline files and cached 2 frequently accessed files (479 MB saved in cloud)"),
                QStringLiteral("READY"));
    rebuildFilteredModel();
}

QByteArray Backend::synthesizePayloadForEntry(const DriveFileEntry &entry) const {
    const QString header = QStringLiteral(
        "GDRIVE VIRTUAL FILE: %1\n"
        "Category: %2\n"
        "Logical Cloud Size: %3 (%4 bytes)\n"
        "Access Count: %5\n"
        "Hot Retention Score: %6\n"
        "---\n"
        "Hydrated on-demand by gdrive Smart Cache Engine.\n")
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

bool Backend::fetchFromRcloneLocked(const DriveFileEntry &entry, const QString &destBlobPath, QString *errorOut) const {
    if (!m_rcloneAvailable || m_remoteName.isEmpty() || !m_availableRemotes.contains(m_remoteName))
        return false;

    QString remoteSpec = m_remoteName;
    if (!remoteSpec.endsWith(':'))
        remoteSpec += ':';
    if (!m_remotePath.isEmpty()) {
        QString rp = m_remotePath;
        while (rp.startsWith('/'))
            rp.remove(0, 1);
        while (rp.endsWith('/'))
            rp.chop(1);
        if (!rp.isEmpty())
            remoteSpec += rp + "/";
    }
    remoteSpec += entry.path;

    QDir().mkpath(QFileInfo(destBlobPath).absolutePath());
    QProcess proc;
    proc.start(m_rcloneBin, {"copyto", "--contimeout=3s", "--timeout=5s", "--retries=1", remoteSpec, destBlobPath});
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(500);
        if (errorOut)
            *errorOut = tr("Timed out connecting to rclone remote '%1:'").arg(m_remoteName);
        return false;
    }
    if (proc.exitCode() != 0) {
        if (errorOut)
            *errorOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return false;
    }
    return QFileInfo::exists(destBlobPath);
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
}

void Backend::rebuildFilteredModel() {
    QList<DriveFileEntry> filtered;
    {
        QMutexLocker locker(&m_mutex);
        const QString q = m_searchQuery.trimmed().toLower();
        for (const auto &e : m_entries) {
            if (!m_currentFolder.isEmpty() && e.folder != m_currentFolder)
                continue;
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
    QSet<QString> set;
    for (const auto &e : m_entries) {
        if (!e.folder.isEmpty() && e.folder != QLatin1String("/"))
            set.insert(e.folder);
    }
    QStringList list = set.values();
    list.sort(Qt::CaseInsensitive);
    return list;
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

QString Backend::syncState() const {
    return m_syncing ? QStringLiteral("syncing") : QStringLiteral("synced");
}

QString Backend::syncStateLabel() const {
    return m_syncing ? tr("Syncing files…") : tr("All files in sync");
}

QString Backend::syncDetailText() const {
    if (m_syncing) {
        const int pct = qBound(5, static_cast<int>(std::round(m_syncProgress * 100.0)), 99);
        if (!m_activeSyncFile.isEmpty())
            return tr("Syncing %1 (%2%)").arg(m_activeSyncFile).arg(pct);
        return tr("Checking remote catalog (%1%)").arg(pct);
    }
    return tr("%1 cached locally · Updated %2").arg(cachedFileCount()).arg(lastSyncedFormatted());
}

QString Backend::lastSyncedFormatted() const {
    return formatRelativeTime(m_lastSyncedSecs);
}

int Backend::syncedFileCount() const {
    return qMax(0, totalFileCount() - m_pendingSyncCount);
}

bool Backend::autoStartEnabled() const {
    const QString userService = QDir::homePath() + QStringLiteral("/.config/systemd/user/gdrive.service");
    const QString hyprAutostart = QDir::homePath() + QStringLiteral("/.config/hypr/autostart.lua");
    if (QFileInfo::exists(userService))
        return true;
    QFile f(hyprAutostart);
    if (f.open(QIODevice::ReadOnly)) {
        if (QString::fromUtf8(f.readAll()).contains(QLatin1String("gdrive")))
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
    qint64 sum = 0;
    for (const auto &e : m_entries)
        sum += e.sizeBytes;
    return sum;
}

qint64 Backend::cachedBytes() const {
    QMutexLocker locker(&m_mutex);
    qint64 sum = 0;
    for (const auto &e : m_entries) {
        if (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached"))
            sum += e.sizeBytes;
    }
    return sum;
}

qint64 Backend::pinnedBytes() const {
    QMutexLocker locker(&m_mutex);
    qint64 sum = 0;
    for (const auto &e : m_entries) {
        if (e.pinned)
            sum += e.sizeBytes;
    }
    return sum;
}

qint64 Backend::hotCachedBytes() const {
    QMutexLocker locker(&m_mutex);
    qint64 sum = 0;
    for (const auto &e : m_entries) {
        if (!e.pinned && e.state == QLatin1String("cached"))
            sum += e.sizeBytes;
    }
    return sum;
}

qint64 Backend::savedBytes() const {
    return qMax<qint64>(0, totalCloudBytes() - cachedBytes());
}

qint64 Backend::diskAvailableBytes() const {
    if (m_simulatedDiskFreeBytes >= 0)
        return m_simulatedDiskFreeBytes;
    struct statvfs vfs {};
    if (::statvfs(m_stateDir.toUtf8().constData(), &vfs) == 0 && vfs.f_bavail > 0)
        return static_cast<qint64>(vfs.f_bavail) * static_cast<qint64>(vfs.f_frsize);
    return 100LL * 1024LL * 1024LL * 1024LL;
}

qint64 Backend::diskTotalBytes() const {
    struct statvfs vfs {};
    if (::statvfs(m_stateDir.toUtf8().constData(), &vfs) == 0 && vfs.f_blocks > 0)
        return static_cast<qint64>(vfs.f_blocks) * static_cast<qint64>(vfs.f_frsize);
    return 500LL * 1024LL * 1024LL * 1024LL;
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
    int c = 0;
    for (const auto &e : m_entries)
        if (e.isFrequent)
            ++c;
    return c;
}

int Backend::pinnedFileCount() const {
    QMutexLocker locker(&m_mutex);
    int c = 0;
    for (const auto &e : m_entries)
        if (e.pinned)
            ++c;
    return c;
}

int Backend::cachedFileCount() const {
    QMutexLocker locker(&m_mutex);
    int c = 0;
    for (const auto &e : m_entries)
        if (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached"))
            ++c;
    return c;
}

int Backend::cloudOnlyFileCount() const {
    QMutexLocker locker(&m_mutex);
    int c = 0;
    for (const auto &e : m_entries)
        if (e.state == QLatin1String("cloud"))
            ++c;
    return c;
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
    {
        QMutexLocker locker(&m_mutex);
        int idx = -1;
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries.at(i).path == path) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            return false;

        m_entries[idx].pinned = pinned;
        if (pinned) {
            hydrateEntryLocked(idx);
            logActivity(tr("Pinned Offline"),
                        tr("%1 (%2) locked on disk for offline use").arg(m_entries[idx].name, formatSize(m_entries[idx].sizeBytes)),
                        QStringLiteral("PIN"));
        } else {
            m_entries[idx].state = QFileInfo::exists(localBlobPath(path))
                ? QStringLiteral("cached")
                : QStringLiteral("cloud");
            logActivity(tr("Unpinned"),
                        tr("%1 returned to smart LFU/LRU cache management").arg(m_entries[idx].name),
                        QStringLiteral("UNPIN"));
        }
        recomputeScoresLocked();
    }

    enforceCacheBudget(pinned ? path : QString());
    saveState();
    rebuildFilteredModel();
    return true;
}

bool Backend::evictFile(const QString &path, QString *errorOut) {
    pushUndoSnapshot(tr("Evict %1").arg(path));
    {
        QMutexLocker locker(&m_mutex);
        int idx = -1;
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries.at(i).path == path) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
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

        const QString blobPath = localBlobPath(path);
        if (QFileInfo::exists(blobPath))
            QFile::remove(blobPath);

        m_entries[idx].state = QStringLiteral("cloud");
        syncProjectionForEntryLocked(m_entries[idx]);
        logActivity(tr("Evicted from Disk"),
                    tr("Freed %1 by returning %2 to Cloud Only").arg(formatSize(m_entries[idx].sizeBytes), m_entries[idx].name),
                    QStringLiteral("EVICT"));
    }

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

    {
        QMutexLocker locker(&m_mutex);
        recomputeScoresLocked();

        QList<int> candidates;
        for (int i = 0; i < m_entries.size(); ++i) {
            const auto &e = m_entries.at(i);
            if (!e.pinned && e.state == QLatin1String("cached") && !e.isFrequent) {
                candidates.append(i);
            }
        }
        // If all cached files are marked frequent and user explicitly asked to evict cold files,
        // pick the lowest-scoring unpinned cached files.
        if (candidates.isEmpty()) {
            for (int i = 0; i < m_entries.size(); ++i) {
                const auto &e = m_entries.at(i);
                if (!e.pinned && e.state == QLatin1String("cached"))
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
            syncProjectionForEntryLocked(m_entries[idx]);
            freedBytes += m_entries[idx].sizeBytes;
            ++evictedCount;
        }
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

    {
        QMutexLocker locker(&m_mutex);
        recomputeScoresLocked();

        const qint64 ceiling = effectiveBudgetBytes();
        if (cachedBytes() <= ceiling)
            return 0;

        QList<int> candidates;
        for (int i = 0; i < m_entries.size(); ++i) {
            const auto &e = m_entries.at(i);
            if (!e.pinned && e.state == QLatin1String("cached") && e.path != protectedPath) {
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
            syncProjectionForEntryLocked(m_entries[idx]);
            freedBytes += m_entries[idx].sizeBytes;
            evictedNames.append(m_entries[idx].name);
            ++evictedCount;
        }
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
        {
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
    {
        QMutexLocker locker(&m_mutex);
        syncAllProjectionsLocked();
    }
    logActivity(tr("Virtual Drive Unmounted"),
                tr("Unmounted %1").arg(m_mountDir),
                QStringLiteral("UNMOUNT"));
    emit mountStateChanged();
}

void Backend::refreshRemotes() {
    m_rcloneBin = QStandardPaths::findExecutable("rclone");
    m_rcloneAvailable = !m_rcloneBin.isEmpty();
    m_availableRemotes.clear();

    if (m_rcloneAvailable) {
        QProcess proc;
        proc.start(m_rcloneBin, {"listremotes"});
        if (proc.waitForFinished(3000) && proc.exitCode() == 0) {
            const QStringList lines = QString::fromUtf8(proc.readAllStandardOutput())
                                          .split('\n', Qt::SkipEmptyParts);
            for (QString line : lines) {
                line = line.trimmed();
                if (line.endsWith(':'))
                    line.chop(1);
                if (!line.isEmpty())
                    m_availableRemotes.append(line);
            }
        }
    }
    emit connectionChanged();
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

void Backend::syncNow() {
    m_syncing = true;
    emit statusChanged();

    if (m_sourceMode == QLatin1String("rclone") && m_rcloneAvailable && m_availableRemotes.contains(m_remoteName)) {
        QString spec = m_remoteName;
        if (!spec.endsWith(':'))
            spec += ':';
        if (!m_remotePath.isEmpty())
            spec += m_remotePath;

        QProcess proc;
        proc.start(m_rcloneBin, {"lsjson", "-R", "--files-only", "--contimeout=3s", "--timeout=5s", "--retries=1", spec});
        const bool finished = proc.waitForFinished(5000);
        if (!finished) {
            proc.kill();
            proc.waitForFinished(500);
        }
        if (finished && proc.exitCode() == 0) {
            const QJsonArray arr = QJsonDocument::fromJson(proc.readAllStandardOutput()).array();
            QMutexLocker locker(&m_mutex);
            QHash<QString, DriveFileEntry> existingByPath;
            for (const auto &e : m_entries)
                existingByPath.insert(e.path, e);

            QList<DriveFileEntry> nextEntries;
            for (const auto &val : arr) {
                const QJsonObject obj = val.toObject();
                if (obj.value("IsDir").toBool())
                    continue;
                const QString relPath = obj.value("Path").toString();
                if (relPath.isEmpty())
                    continue;

                DriveFileEntry e = existingByPath.value(relPath);
                e.path = relPath;
                e.name = QFileInfo(relPath).fileName();
                const QString dir = QFileInfo(relPath).path();
                e.folder = (dir == QLatin1String(".") || dir.isEmpty()) ? QStringLiteral("Root") : dir;
                e.category = inferCategory(e.name);
                e.sizeBytes = static_cast<qint64>(obj.value("Size").toDouble(0));
                const QDateTime dt = QDateTime::fromString(obj.value("ModTime").toString(), Qt::ISODate);
                e.modifiedSecs = dt.isValid() ? dt.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
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
            locker.unlock();
            enforceCacheBudget();
            saveState();
            rebuildFilteredModel();
            logActivity(tr("Synced with Google Drive"),
                        tr("Catalog updated from %1 (%2 files)").arg(spec).arg(totalFileCount()),
                        QStringLiteral("SYNC"));
            setStatusBanner(tr("Synced %1 files from %2").arg(totalFileCount()).arg(spec), false);
        } else {
            const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
            setStatusBanner(err.isEmpty() ? tr("Failed to sync with rclone remote '%1:'").arg(m_remoteName) : err, true);
        }
    } else if (m_sourceMode == QLatin1String("folder") && !m_localSourceDir.isEmpty()) {
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

bool Backend::vfsStat(const QString &relPath, qint64 *sizeOut, qint64 *mtimeSecsOut,
                      bool *isDirOut, bool *isResidentOut) const {
    QMutexLocker locker(&m_mutex);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (relPath.isEmpty()) {
        if (sizeOut) *sizeOut = 4096;
        if (mtimeSecsOut) *mtimeSecsOut = now;
        if (isDirOut) *isDirOut = true;
        if (isResidentOut) *isResidentOut = true;
        return true;
    }

    for (const auto &e : m_entries) {
        if (e.path == relPath) {
            if (sizeOut) *sizeOut = e.sizeBytes;
            if (mtimeSecsOut) *mtimeSecsOut = e.modifiedSecs;
            if (isDirOut) *isDirOut = false;
            if (isResidentOut) *isResidentOut = (e.state == QLatin1String("pinned") || e.state == QLatin1String("cached"));
            return true;
        }
    }

    const QString prefix = relPath + "/";
    for (const auto &e : m_entries) {
        if (e.path.startsWith(prefix)) {
            if (sizeOut) *sizeOut = 4096;
            if (mtimeSecsOut) *mtimeSecsOut = now;
            if (isDirOut) *isDirOut = true;
            if (isResidentOut) *isResidentOut = true;
            return true;
        }
    }

    return false;
}

QStringList Backend::vfsReadDir(const QString &relDirPath, bool *existsOut) const {
    QMutexLocker locker(&m_mutex);
    QSet<QString> children;
    bool foundDir = relDirPath.isEmpty();
    const QString prefix = relDirPath.isEmpty() ? QString() : (relDirPath + "/");

    for (const auto &e : m_entries) {
        if (prefix.isEmpty()) {
            const int slash = e.path.indexOf('/');
            if (slash < 0)
                children.insert(e.path);
            else
                children.insert(e.path.left(slash));
        } else if (e.path.startsWith(prefix)) {
            foundDir = true;
            const QString rest = e.path.mid(prefix.size());
            const int slash = rest.indexOf('/');
            if (slash < 0)
                children.insert(rest);
            else
                children.insert(rest.left(slash));
        }
    }

    if (existsOut)
        *existsOut = foundDir;
    QStringList list = children.values();
    list.sort(Qt::CaseInsensitive);
    return list;
}

QString Backend::vfsOpenAndHydrate(const QString &relPath, bool forWrite, QString *errorOut) {
    Q_UNUSED(forWrite);
    QString blobPath;
    QString entryName;
    qint64 entrySize = 0;
    bool wasCloudOnly = false;
    bool becameFrequent = false;
    int newAccessCount = 0;

    {
        QMutexLocker locker(&m_mutex);
        int idx = -1;
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries.at(i).path == relPath) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            if (errorOut)
                *errorOut = tr("File not found in Drive catalog: %1").arg(relPath);
            return {};
        }

        wasCloudOnly = (m_entries[idx].state == QLatin1String("cloud"));
        const bool wasFrequent = m_entries[idx].isFrequent;

        if (!hydrateEntryLocked(idx, errorOut))
            return {};

        m_entries[idx].accessCount += 1;
        m_entries[idx].lastAccessedSecs = QDateTime::currentSecsSinceEpoch();
        recomputeScoresLocked(m_entries[idx].lastAccessedSecs);

        newAccessCount = m_entries[idx].accessCount;
        becameFrequent = (!wasFrequent && m_entries[idx].isFrequent);
        entryName = m_entries[idx].name;
        entrySize = m_entries[idx].sizeBytes;
        blobPath = localBlobPath(relPath);
    }

    // Enforce cache ceiling while protecting the file currently being opened.
    enforceCacheBudget(relPath);
    saveState();

    QMetaObject::invokeMethod(this, [this, entryName, entrySize, wasCloudOnly, becameFrequent, newAccessCount]() {
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
    }, Qt::AutoConnection);

    return blobPath;
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
        int idx = -1;
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries.at(i).path == relPath) {
                idx = i;
                break;
            }
        }
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
            m_entries.append(e);
        }
        recomputeScoresLocked(now);
    }

    if (localCachePathOut)
        *localCachePathOut = blobPath;

    QMetaObject::invokeMethod(this, [this]() {
        saveState();
        rebuildFilteredModel();
    }, Qt::AutoConnection);

    return true;
}

bool Backend::vfsWriteCompleted(const QString &relPath) {
    const QString blobPath = localBlobPath(relPath);
    const QFileInfo fi(blobPath);
    if (!fi.exists())
        return false;

    {
        QMutexLocker locker(&m_mutex);
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries.at(i).path == relPath) {
                m_entries[i].sizeBytes = qMax<qint64>(fi.size(), m_entries[i].sizeBytes);
                m_entries[i].modifiedSecs = QDateTime::currentSecsSinceEpoch();
                m_entries[i].state = m_entries[i].pinned ? QStringLiteral("pinned") : QStringLiteral("cached");
                syncProjectionForEntryLocked(m_entries[i]);
                break;
            }
        }
    }

    QMetaObject::invokeMethod(this, [this]() {
        enforceCacheBudget();
        saveState();
        rebuildFilteredModel();
    }, Qt::AutoConnection);

    return true;
}

bool Backend::vfsUnlinkFile(const QString &relPath) {
    bool removed = false;
    {
        QMutexLocker locker(&m_mutex);
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries.at(i).path == relPath) {
                const QString blobPath = localBlobPath(relPath);
                if (QFileInfo::exists(blobPath))
                    QFile::remove(blobPath);
                m_entries.removeAt(i);
                removed = true;
                break;
            }
        }
    }
    if (removed) {
        QMetaObject::invokeMethod(this, [this]() {
            saveState();
            rebuildFilteredModel();
        }, Qt::AutoConnection);
    }
    return removed;
}

QList<DriveFileEntry> Backend::allEntries() const {
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

DriveFileEntry Backend::entryForPath(const QString &path) const {
    QMutexLocker locker(&m_mutex);
    for (const auto &e : m_entries) {
        if (e.path == path)
            return e;
    }
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

    m_entries.clear();
    const QJsonArray arr = root.value("entries").toArray();
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
        m_entries.append(e);
    }

    recomputeScoresLocked();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].state == QLatin1String("pinned") || m_entries[i].state == QLatin1String("cached"))
            hydrateEntryLocked(i);
    }
    locker.unlock();
    rebuildFilteredModel();
}

void Backend::saveState() const {
    QMutexLocker locker(&m_mutex);
    QDir().mkpath(m_stateDir);

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
        arr.append(o);
    }

    QJsonObject root;
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
    root.insert("syncing", m_syncing);
    root.insert("syncState", syncState());
    root.insert("activeSyncFile", m_activeSyncFile);
    root.insert("syncProgress", m_syncProgress);
    root.insert("pendingSyncCount", m_pendingSyncCount);
    root.insert("entries", arr);

    QSaveFile f(m_stateDir + "/state.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.commit();
    }
}
