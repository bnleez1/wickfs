#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QRecursiveMutex>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class FilePicker;
class FuseMount;

struct DriveFileEntry {
    QString path;            // Relative path inside Drive, e.g. "Engineering/System-Design.pdf"
    QString name;            // Base file name
    QString folder;          // Parent folder ("Engineering" or "/")
    QString category;        // "document", "spreadsheet", "presentation", "image", "video", "audio", "archive", "code"
    qint64 sizeBytes = 0;    // Logical cloud size in bytes
    qint64 modifiedSecs = 0; // Last modified timestamp (epoch seconds)
    QString state;           // "pinned", "cached", "cloud"
    bool pinned = false;     // Keep Offline (never evicted automatically)
    int accessCount = 0;     // Total times opened/accessed
    qint64 lastAccessedSecs = 0; // Most recent access timestamp (epoch seconds)
    double hotScore = 0.0;   // 0..100 LFU+LRU retention score
    bool isFrequent = false; // True when accessCount >= frequentThreshold
};

class DriveFileModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        NameRole,
        FolderRole,
        CategoryRole,
        SizeBytesRole,
        SizeFormattedRole,
        CachedBytesRole,
        StateRole,
        PinnedRole,
        CachedRole,
        AccessCountRole,
        LastAccessedFormattedRole,
        ModifiedFormattedRole,
        HotScoreRole,
        IsFrequentRole
    };

    explicit DriveFileModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setEntries(const QList<DriveFileEntry> &entries);
    const QList<DriveFileEntry> &entries() const { return m_entries; }
    DriveFileEntry entryAt(int row) const;
    int indexOfPath(const QString &path) const;

private:
    QList<DriveFileEntry> m_entries;
};

struct SnapshotState {
    QList<DriveFileEntry> entries;
    qint64 maxCacheBytes = 0;
    qint64 minFreeDiskBytes = 0;
    int frequentThreshold = 3;
    bool autoKeepFrequent = true;
    QString description;
};

class Backend : public QObject {
    Q_OBJECT

    Q_PROPERTY(DriveFileModel *fileModel READ fileModel CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY statsChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedFile READ selectedFile NOTIFY selectionChanged)

    Q_PROPERTY(QString filterMode READ filterMode WRITE setFilterMode NOTIFY filterChanged)
    Q_PROPERTY(QString currentFolder READ currentFolder WRITE setCurrentFolder NOTIFY filterChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY filterChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY filterChanged)
    Q_PROPERTY(QStringList folders READ folders NOTIFY statsChanged)

    Q_PROPERTY(bool mounted READ mounted NOTIFY mountStateChanged)
    Q_PROPERTY(bool kernelFuseActive READ kernelFuseActive NOTIFY mountStateChanged)
    Q_PROPERTY(QString mountPoint READ mountPoint NOTIFY mountStateChanged)
    Q_PROPERTY(QString mountStatusText READ mountStatusText NOTIFY mountStateChanged)

    Q_PROPERTY(bool rcloneAvailable READ rcloneAvailable NOTIFY connectionChanged)
    Q_PROPERTY(bool fuseAvailable READ fuseAvailable NOTIFY connectionChanged)
    Q_PROPERTY(QString sourceMode READ sourceMode NOTIFY connectionChanged)
    Q_PROPERTY(QString remoteName READ remoteName NOTIFY connectionChanged)
    Q_PROPERTY(QString remotePath READ remotePath NOTIFY connectionChanged)
    Q_PROPERTY(QStringList availableRemotes READ availableRemotes NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionSummary READ connectionSummary NOTIFY connectionChanged)
    Q_PROPERTY(bool oauthAuthorized READ oauthAuthorized NOTIFY connectionChanged)
    Q_PROPERTY(QString oauthClientId READ oauthClientId NOTIFY connectionChanged)
    Q_PROPERTY(QString oauthClientSecret READ oauthClientSecret NOTIFY connectionChanged)
    Q_PROPERTY(QString oauthClientModeLabel READ oauthClientModeLabel NOTIFY connectionChanged)
    Q_PROPERTY(QString oauthStatusSummary READ oauthStatusSummary NOTIFY connectionChanged)
    Q_PROPERTY(QString statusBannerText READ statusBannerText NOTIFY statusChanged)
    Q_PROPERTY(bool statusBannerIsError READ statusBannerIsError NOTIFY statusChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY statusChanged)
    Q_PROPERTY(QString syncState READ syncState NOTIFY statusChanged)
    Q_PROPERTY(QString syncStateLabel READ syncStateLabel NOTIFY statusChanged)
    Q_PROPERTY(QString syncDetailText READ syncDetailText NOTIFY statusChanged)
    Q_PROPERTY(QString activeSyncFile READ activeSyncFile NOTIFY statusChanged)
    Q_PROPERTY(double syncProgress READ syncProgress NOTIFY statusChanged)
    Q_PROPERTY(QString lastSyncedFormatted READ lastSyncedFormatted NOTIFY statusChanged)
    Q_PROPERTY(int pendingSyncCount READ pendingSyncCount NOTIFY statusChanged)
    Q_PROPERTY(int syncedFileCount READ syncedFileCount NOTIFY statsChanged)
    Q_PROPERTY(bool autoStartEnabled READ autoStartEnabled NOTIFY statusChanged)
    Q_PROPERTY(QString autoStartSummary READ autoStartSummary NOTIFY statusChanged)
    Q_PROPERTY(int wakeSyncCount READ wakeSyncCount NOTIFY statusChanged)

    Q_PROPERTY(qint64 totalCloudBytes READ totalCloudBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 cachedBytes READ cachedBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 pinnedBytes READ pinnedBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 hotCachedBytes READ hotCachedBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 savedBytes READ savedBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 maxCacheBytes READ maxCacheBytes NOTIFY budgetChanged)
    Q_PROPERTY(qint64 effectiveBudgetBytes READ effectiveBudgetBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 minFreeDiskBytes READ minFreeDiskBytes NOTIFY budgetChanged)
    Q_PROPERTY(qint64 diskAvailableBytes READ diskAvailableBytes NOTIFY statsChanged)
    Q_PROPERTY(qint64 diskTotalBytes READ diskTotalBytes NOTIFY statsChanged)

    Q_PROPERTY(int maxCacheMb READ maxCacheMb WRITE setMaxCacheMb NOTIFY budgetChanged)
    Q_PROPERTY(int minFreeDiskGb READ minFreeDiskGb WRITE setMinFreeDiskGb NOTIFY budgetChanged)
    Q_PROPERTY(int frequentThreshold READ frequentThreshold WRITE setFrequentThreshold NOTIFY budgetChanged)
    Q_PROPERTY(int halfLifeHours READ halfLifeHours WRITE setHalfLifeHours NOTIFY budgetChanged)
    Q_PROPERTY(bool autoKeepFrequent READ autoKeepFrequent WRITE setAutoKeepFrequent NOTIFY budgetChanged)

    Q_PROPERTY(QString totalCloudFormatted READ totalCloudFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString cachedFormatted READ cachedFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString pinnedFormatted READ pinnedFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString hotCachedFormatted READ hotCachedFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString savedFormatted READ savedFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString maxCacheFormatted READ maxCacheFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString effectiveBudgetFormatted READ effectiveBudgetFormatted NOTIFY statsChanged)
    Q_PROPERTY(QString diskAvailableFormatted READ diskAvailableFormatted NOTIFY statsChanged)
    Q_PROPERTY(double cacheUsageRatio READ cacheUsageRatio NOTIFY statsChanged)
    Q_PROPERTY(double pinnedUsageRatio READ pinnedUsageRatio NOTIFY statsChanged)
    Q_PROPERTY(int spaceSavedPercent READ spaceSavedPercent NOTIFY statsChanged)
    Q_PROPERTY(bool lowDiskFloorActive READ lowDiskFloorActive NOTIFY statsChanged)

    Q_PROPERTY(int totalFileCount READ totalFileCount NOTIFY statsChanged)
    Q_PROPERTY(int visibleFileCount READ visibleFileCount NOTIFY filterChanged)
    Q_PROPERTY(int frequentFileCount READ frequentFileCount NOTIFY statsChanged)
    Q_PROPERTY(int pinnedFileCount READ pinnedFileCount NOTIFY statsChanged)
    Q_PROPERTY(int cachedFileCount READ cachedFileCount NOTIFY statsChanged)
    Q_PROPERTY(int cloudOnlyFileCount READ cloudOnlyFileCount NOTIFY statsChanged)

    Q_PROPERTY(QVariantList activityLog READ activityLog NOTIFY activityChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoRedoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoRedoChanged)

public:
    explicit Backend(const QString &stateDir = {}, const QString &mountDir = {},
                     FilePicker *picker = nullptr, QObject *parent = nullptr);
    ~Backend() override;

    DriveFileModel *fileModel() { return &m_fileModel; }
    int count() const { return m_actionCount; }

    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int index);
    QString selectedPath() const;
    QVariantMap selectedFile() const;

    QString filterMode() const { return m_filterMode; }
    void setFilterMode(const QString &mode);
    QString currentFolder() const { return m_currentFolder; }
    void setCurrentFolder(const QString &folder);
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    QString sortMode() const { return m_sortMode; }
    void setSortMode(const QString &mode);
    QStringList folders() const;

    bool mounted() const;
    bool kernelFuseActive() const;
    QString mountPoint() const { return m_mountDir; }
    QString mountStatusText() const;

    bool rcloneAvailable() const { return m_rcloneAvailable; }
    bool fuseAvailable() const { return m_fuseAvailable; }
    QString sourceMode() const { return m_sourceMode; }
    QString remoteName() const { return m_remoteName; }
    QString remotePath() const { return m_remotePath; }
    QStringList availableRemotes() const { return m_availableRemotes; }
    QString connectionSummary() const;
    bool oauthAuthorized() const { return m_oauthAuthorized; }
    QString oauthClientId() const { return m_oauthClientId; }
    QString oauthClientSecret() const { return m_oauthClientSecret; }
    QString oauthClientModeLabel() const;
    QString oauthStatusSummary() const;
    void setRcloneConfigPath(const QString &path);
    QString rcloneConfigPath() const;
    QString statusBannerText() const { return m_statusBannerText; }
    bool statusBannerIsError() const { return m_statusBannerIsError; }
    bool syncing() const { return m_syncing; }
    QString syncState() const;
    QString syncStateLabel() const;
    QString syncDetailText() const;
    QString activeSyncFile() const { return m_activeSyncFile; }
    double syncProgress() const { return m_syncProgress; }
    QString lastSyncedFormatted() const;
    int pendingSyncCount() const { return m_pendingSyncCount; }
    int syncedFileCount() const;
    bool autoStartEnabled() const;
    QString autoStartSummary() const;
    int wakeSyncCount() const { return m_wakeSyncCount; }

    Q_INVOKABLE void beginSyncSimulation(const QString &fileName = {}, int totalFiles = 1);
    Q_INVOKABLE void finishSyncSimulation();

public slots:
    void handleSystemWake(bool sleeping = false);

    qint64 totalCloudBytes() const;
    qint64 cachedBytes() const;
    qint64 pinnedBytes() const;
    qint64 hotCachedBytes() const;
    qint64 savedBytes() const;
    qint64 maxCacheBytes() const { return m_maxCacheBytes; }
    qint64 effectiveBudgetBytes() const;
    qint64 minFreeDiskBytes() const { return m_minFreeDiskBytes; }
    qint64 diskAvailableBytes() const;
    qint64 diskTotalBytes() const;

    int maxCacheMb() const { return static_cast<int>(m_maxCacheBytes / (1024LL * 1024LL)); }
    void setMaxCacheMb(int mb);
    int minFreeDiskGb() const { return static_cast<int>(m_minFreeDiskBytes / (1024LL * 1024LL * 1024LL)); }
    void setMinFreeDiskGb(int gb);
    int frequentThreshold() const { return m_frequentThreshold; }
    void setFrequentThreshold(int threshold);
    int halfLifeHours() const { return m_halfLifeHours; }
    void setHalfLifeHours(int hours);
    bool autoKeepFrequent() const { return m_autoKeepFrequent; }
    void setAutoKeepFrequent(bool enabled);

    void setMaxCacheBytesDirect(qint64 bytes);
    void setMinFreeDiskBytesDirect(qint64 bytes);
    void setSimulatedDiskFreeBytes(qint64 bytes);

    QString totalCloudFormatted() const;
    QString cachedFormatted() const;
    QString pinnedFormatted() const;
    QString hotCachedFormatted() const;
    QString savedFormatted() const;
    QString maxCacheFormatted() const;
    QString effectiveBudgetFormatted() const;
    QString diskAvailableFormatted() const;
    double cacheUsageRatio() const;
    double pinnedUsageRatio() const;
    int spaceSavedPercent() const;
    bool lowDiskFloorActive() const;

    int totalFileCount() const;
    int visibleFileCount() const { return m_fileModel.rowCount(); }
    int frequentFileCount() const;
    int pinnedFileCount() const;
    int cachedFileCount() const;
    int cloudOnlyFileCount() const;

    QVariantList activityLog() const { return m_activityLog; }
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    // QML & CLI actions
    Q_INVOKABLE void primaryAction();
    Q_INVOKABLE bool accessFile(const QString &path, QString *errorOut = nullptr);
    Q_INVOKABLE void openSelectedInDesktop();
    Q_INVOKABLE void openMountFolderInDesktop();
    Q_INVOKABLE bool togglePin(const QString &path);
    Q_INVOKABLE void togglePinSelected();
    Q_INVOKABLE bool setPinned(const QString &path, bool pinned);
    Q_INVOKABLE bool evictFile(const QString &path, QString *errorOut = nullptr);
    Q_INVOKABLE void evictSelected();
    Q_INVOKABLE int evictColdFiles(int maxFilesToEvict = 0);
    Q_INVOKABLE int enforceCacheBudget(const QString &protectedPath = {});

    Q_INVOKABLE bool toggleMount();
    Q_INVOKABLE bool mountDrive(QString *errorOut = nullptr);
    Q_INVOKABLE void unmountDrive();

    Q_INVOKABLE void syncNow();
    Q_INVOKABLE bool connectRcloneRemote(const QString &remoteName, const QString &remotePath = {});
    Q_INVOKABLE bool connectLocalSource(const QString &folderPath);
    Q_INVOKABLE void pickLocalSourceFolder();
    Q_INVOKABLE void launchRcloneAuth(const QString &remoteName = QStringLiteral("gdrive"));
    Q_INVOKABLE bool configureOAuthClient(const QString &remoteName, const QString &clientId,
                                          const QString &clientSecret, const QString &scope = QStringLiteral("drive"));
    Q_INVOKABLE bool importGoogleClientSecretJson(const QString &jsonFilePath,
                                                  const QString &remoteName = QStringLiteral("gdrive"),
                                                  QString *errorOut = nullptr);
    Q_INVOKABLE void pickGoogleClientSecretJson(const QString &remoteName = QStringLiteral("gdrive"));
    Q_INVOKABLE void openGoogleCloudConsole();
    Q_INVOKABLE void refreshRemotes();
    Q_INVOKABLE bool loadManifestFile(const QString &filePath, QString *errorOut = nullptr);

    Q_INVOKABLE void selectNext();
    Q_INVOKABLE void selectPrevious();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Thread-safe VFS operations called by FuseMount and tests
    bool vfsStat(const QString &relPath, qint64 *sizeOut, qint64 *mtimeSecsOut,
                 bool *isDirOut, bool *isResidentOut) const;
    QStringList vfsReadDir(const QString &relDirPath, bool *existsOut = nullptr) const;
    QString vfsOpenAndHydrate(const QString &relPath, bool forWrite, QString *errorOut = nullptr);
    bool vfsCreateFile(const QString &relPath, QString *localCachePathOut);
    bool vfsWriteCompleted(const QString &relPath);
    bool vfsUnlinkFile(const QString &relPath);

    FuseMount *fuseMount() const { return m_fuseMount; }
    QList<DriveFileEntry> allEntries() const;
    DriveFileEntry entryForPath(const QString &path) const;
    QString localBlobPath(const QString &relPath) const;

    static QString formatSize(qint64 bytes);
    static double computeHotScore(int accessCount, qint64 lastAccessedSecs,
                                  qint64 nowSecs, int halfLifeHours);

signals:
    void statsChanged();
    void filterChanged();
    void selectionChanged();
    void mountStateChanged();
    void connectionChanged();
    void budgetChanged();
    void statusChanged();
    void activityChanged();
    void undoRedoChanged();

private:
    void initStarterCatalog();
    void loadState();
    void saveState() const;
    void recomputeScoresLocked(qint64 nowSecs = 0);
    void rebuildFilteredModel();
    bool hydrateEntryLocked(int idx, QString *errorOut = nullptr);
    void syncProjectionForEntryLocked(const DriveFileEntry &entry) const;
    void syncAllProjectionsLocked() const;
    void logActivity(const QString &action, const QString &detail, const QString &badge = {});
    void setStatusBanner(const QString &text, bool isError = false);
    void pushUndoSnapshot(const QString &description);
    void applySnapshot(const SnapshotState &snap);
    bool fetchFromRcloneLocked(const DriveFileEntry &entry, const QString &destBlobPath, QString *errorOut) const;
    QByteArray synthesizePayloadForEntry(const DriveFileEntry &entry) const;

    mutable QRecursiveMutex m_mutex;
    QString m_stateDir;
    QString m_blobDir;
    QString m_mountDir;
    FilePicker *m_filePicker = nullptr;
    bool m_ownsPicker = false;
    FuseMount *m_fuseMount = nullptr;

    QList<DriveFileEntry> m_entries;
    DriveFileModel m_fileModel;
    int m_selectedIndex = 0;
    int m_actionCount = 0;

    QString m_filterMode = QStringLiteral("all"); // "all", "frequent", "pinned", "cached", "cloud"
    QString m_currentFolder;                      // "" for all folders, or folder name
    QString m_searchQuery;
    QString m_sortMode = QStringLiteral("hot");   // "hot", "name", "size", "recent"

    bool m_rcloneAvailable = false;
    bool m_fuseAvailable = false;
    QString m_rcloneBin;
    QString m_sourceMode = QStringLiteral("catalog"); // "rclone", "folder", "catalog"
    QString m_remoteName = QStringLiteral("gdrive");
    QString m_remotePath;
    QString m_localSourceDir;
    QString m_rcloneConfigPath;
    QStringList m_availableRemotes;
    bool m_oauthAuthorized = false;
    QString m_oauthClientId;
    QString m_oauthClientSecret;
    QString m_oauthScope = QStringLiteral("drive");
    QString m_statusBannerText;
    bool m_statusBannerIsError = false;
    bool m_syncing = false;
    QString m_activeSyncFile;
    double m_syncProgress = 1.0;
    qint64 m_lastSyncedSecs = 0;
    int m_pendingSyncCount = 0;
    int m_wakeSyncCount = 0;
    QTimer m_syncAnimTimer;

    qint64 m_maxCacheBytes = 250LL * 1024LL * 1024LL;        // Default 250 MB smart cache ceiling
    qint64 m_minFreeDiskBytes = 2LL * 1024LL * 1024LL * 1024LL; // Default 2 GB free space floor
    qint64 m_simulatedDiskFreeBytes = -1;
    int m_frequentThreshold = 3;
    int m_halfLifeHours = 72;
    bool m_autoKeepFrequent = true;

    QVariantList m_activityLog;
    QList<SnapshotState> m_undoStack;
    QList<SnapshotState> m_redoStack;
};
