#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

#include "backend.h"
#include "fusemount.h"
#include "theme.h"
#include "translator.h"

class WickfsTests : public QObject {
    Q_OBJECT

private slots:
    void primaryActionCountsAndHydrates()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("state"), dir.filePath("mnt"));
        backend.primaryAction();
        QCOMPARE(backend.count(), 1);
    }

    void cloudFilesReportZeroDiskBlocksUntilOpened()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("state"), dir.filePath("mnt"));
        FuseMount *vfs = backend.fuseMount();
        QVERIFY(vfs != nullptr);

        // "Media/Keynote-Demo-4K.mp4" starts as cloud-only (145 MB logical, 0 disk blocks).
        struct stat st {};
        QCOMPARE(vfs->handleGetattr("/Media/Keynote-Demo-4K.mp4", &st), 0);
        QCOMPARE(static_cast<qint64>(st.st_size), 145LL * 1024LL * 1024LL);
        QCOMPARE(static_cast<qint64>(st.st_blocks), 0LL);

        // Reading through FUSE hydrates the file on-demand and increments accessCount.
        char buf[128] {};
        const int n = vfs->handleOpenAndRead("/Media/Keynote-Demo-4K.mp4", buf, sizeof(buf) - 1, 0);
        QVERIFY(n > 0);
        QVERIFY(QString::fromUtf8(buf, n).contains("Media/Keynote-Demo-4K.mp4"));

        const DriveFileEntry after = backend.entryForPath("Media/Keynote-Demo-4K.mp4");
        QCOMPARE(after.state, QString("cached"));
        QCOMPARE(after.accessCount, 2);

        // Now getattr reports allocated disk blocks.
        struct stat stAfter {};
        QCOMPARE(vfs->handleGetattr("/Media/Keynote-Demo-4K.mp4", &stAfter), 0);
        QVERIFY(stAfter.st_blocks > 0);
    }

    void lfuLruHotScoreRewardsFrequencyAndRecency()
    {
        const qint64 now = 1700000000LL;
        const double neverOpened = Backend::computeHotScore(0, 0, now, 72);
        const double openedOnceNow = Backend::computeHotScore(1, now, now, 72);
        const double openedFiveTimesNow = Backend::computeHotScore(5, now, now, 72);
        const double openedFiveTimesWeekAgo = Backend::computeHotScore(5, now - 3600 * 144, now, 72);

        QCOMPARE(neverOpened, 0.0);
        QVERIFY(openedFiveTimesNow > openedOnceNow);
        QVERIFY(openedFiveTimesNow > openedFiveTimesWeekAgo);
        QVERIFY(openedFiveTimesWeekAgo > 0.0);
    }

    void frequentPromotionWhenThresholdReached()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("state"), dir.filePath("mnt"));
        backend.setFrequentThreshold(3);

        // "Architecture/API-Security-Review.md" starts with accessCount = 2 (below threshold 3).
        DriveFileEntry before = backend.entryForPath("Architecture/API-Security-Review.md");
        QCOMPARE(before.accessCount, 2);
        QVERIFY(!before.isFrequent);

        // Opening it once more promotes it to Frequently Accessed (accessCount = 3).
        QVERIFY(backend.accessFile("Architecture/API-Security-Review.md"));
        DriveFileEntry after = backend.entryForPath("Architecture/API-Security-Review.md");
        QCOMPARE(after.accessCount, 3);
        QVERIFY(after.isFrequent);
        QCOMPARE(after.state, QString("cached"));
    }

    void budgetEvictsColdestFilesFirstAndProtectsPinned()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("state"), dir.filePath("mnt"));

        // Starter catalog has:
        // - Pinned: Distributed-Cache-Spec.pdf (28 MB), 2026-Operating-Model.xlsx (18 MB) -> 46 MB pinned
        // - Cached: Q4-Roadmap-Deck.pptx (64 MB, 8x), Brand-Component-Kit.fig (52 MB, 5x),
        //           Kernel-Benchmark-Results.csv (34 MB, 2x) -> 150 MB unpinned cached (total 196 MB)
        QCOMPARE(backend.pinnedBytes(), 46LL * 1024LL * 1024LL);
        QCOMPARE(backend.cachedBytes(), 196LL * 1024LL * 1024LL);

        // Lower max cache budget to 120 MB: must evict coldest cached files
        // (Kernel-Benchmark-Results.csv [2x] and Brand-Component-Kit.fig [5x])
        // while keeping Q4-Roadmap-Deck.pptx [8x] and both Pinned files!
        backend.setMaxCacheMb(120);
        QVERIFY(backend.cachedBytes() <= 120LL * 1024LL * 1024LL);

        QCOMPARE(backend.entryForPath("Architecture/Distributed-Cache-Spec.pdf").state, QString("pinned"));
        QCOMPARE(backend.entryForPath("Finance/2026-Operating-Model.xlsx").state, QString("pinned"));
        QCOMPARE(backend.entryForPath("Product/Q4-Roadmap-Deck.pptx").state, QString("cached"));
        QCOMPARE(backend.entryForPath("Engineering/Kernel-Benchmark-Results.csv").state, QString("cloud"));
        QCOMPARE(backend.entryForPath("Design/Brand-Component-Kit.fig").state, QString("cloud"));
    }

    void freeDiskFloorTriggersAutomaticEviction()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("state"), dir.filePath("mnt"));
        backend.setMaxCacheMb(500);
        backend.setMinFreeDiskGb(10); // Require 10 GB free on disk

        // Simulate disk free space dropping so only 80 MB above the 10 GB floor remains.
        const qint64 tenGb = 10LL * 1024LL * 1024LL * 1024LL;
        const qint64 currentCached = backend.cachedBytes();
        backend.setSimulatedDiskFreeBytes(tenGb - currentCached + 80LL * 1024LL * 1024LL);

        QVERIFY(backend.lowDiskFloorActive());
        QVERIFY(backend.cachedBytes() <= 80LL * 1024LL * 1024LL);
        // Pinned files (46 MB) remain untouched.
        QCOMPARE(backend.pinnedBytes(), 46LL * 1024LL * 1024LL);
    }

    void localSourceFolderAndUndoRedo()
    {
        QTemporaryDir dir;
        const QString remoteDir = dir.filePath("remote_cloud");
        QDir().mkpath(remoteDir + "/Notes");
        write(remoteDir + "/Notes/Design.md", "Cloud note content\n");
        write(remoteDir + "/Notes/Data.csv", "a,b,c\n1,2,3\n");

        Backend backend(dir.filePath("state"), dir.filePath("mnt"));
        QVERIFY(backend.connectLocalSource(remoteDir));
        QCOMPARE(backend.totalFileCount(), 2);

        QVERIFY(backend.setPinned("Notes/Design.md", true));
        QCOMPARE(backend.entryForPath("Notes/Design.md").state, QString("pinned"));
        QVERIFY(backend.canUndo());

        backend.undo();
        QVERIFY(!backend.entryForPath("Notes/Design.md").pinned);
        QVERIFY(backend.canRedo());

        backend.redo();
        QVERIFY(backend.entryForPath("Notes/Design.md").pinned);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto writeExec = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(writeExec(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(writeExec(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(writeExec(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n files": {"one": "%n file", "other": "%n files"}})");
        write(dir.path() + "/ru.json", R"({"%n files": {"one": "%n файл", "few": "%n файла", "many": "%n файлов", "other": "%n файла"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n файл"));
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 22), QString("%n файла"));
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 25), QString("%n файлов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    void syncStateTransitionsBetweenSyncingAndInSync()
    {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.path() + "/mnt");

        QVERIFY(!backend.syncing());
        QCOMPARE(backend.syncState(), QString("synced"));
        QCOMPARE(backend.syncStateLabel(), QString("All files in sync"));
        QCOMPARE(backend.syncedFileCount(), backend.totalFileCount());

        backend.beginSyncSimulation("Q4-Roadmap-Deck.pptx", 2);
        QVERIFY(backend.syncing());
        QCOMPARE(backend.syncState(), QString("syncing"));
        QCOMPARE(backend.syncStateLabel(), QString("Syncing files…"));
        QCOMPARE(backend.activeSyncFile(), QString("Q4-Roadmap-Deck.pptx"));
        QCOMPARE(backend.pendingSyncCount(), 2);

        // Verify state.json reflects active sync state for the Omarchy bar widget
        QFile stateFile(dir.path() + "/state.json");
        QVERIFY(stateFile.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(stateFile.readAll()).object();
        QCOMPARE(root.value("syncing").toBool(), true);
        QCOMPARE(root.value("syncState").toString(), QString("syncing"));
        QCOMPARE(root.value("activeSyncFile").toString(), QString("Q4-Roadmap-Deck.pptx"));
        stateFile.close();

        backend.finishSyncSimulation();
        QVERIFY(!backend.syncing());
        QCOMPARE(backend.syncState(), QString("synced"));
        QCOMPARE(backend.syncStateLabel(), QString("All files in sync"));
        QCOMPARE(backend.pendingSyncCount(), 0);
    }

    void pluginModelParsesStateAndSanitizesMarkup()
    {
        QFile manifestFile(":/plugin/manifest.json");
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        QCOMPARE(manifest.value("id").toString(), QString("ben.wickfs"));
        QCOMPARE(manifest.value("name").toString(), QString("WickFS Sync"));

        QFile jsFile(":/plugin/Model.js");
        QVERIFY(jsFile.open(QIODevice::ReadOnly));
        QString code = QString::fromUtf8(jsFile.readAll());
        code.replace(".pragma library", "");

        QJSEngine engine;
        QJSValue evalRes = engine.evaluate(code);
        QVERIFY2(!evalRes.isError(), qPrintable(evalRes.toString()));

        // Verify markup-shaped input is sanitized
        QJsonObject entry;
        entry.insert("path", "folder/<b>evil.doc</b>");
        entry.insert("name", "<b>evil.doc</b>");
        entry.insert("folder", "folder");
        entry.insert("sizeBytes", 1048576);
        entry.insert("state", "cached");
        entry.insert("pinned", false);
        entry.insert("accessCount", 5);
        entry.insert("isFrequent", true);
        entry.insert("hotScore", 88.5);

        QJsonArray entriesArr;
        entriesArr.append(entry);

        QJsonObject payload;
        payload.insert("syncing", true);
        payload.insert("syncState", "syncing");
        payload.insert("activeSyncFile", "<script>alert(1)</script> & \"bad.txt\"");
        payload.insert("syncProgress", 0.65);
        payload.insert("pendingSyncCount", 2);
        payload.insert("maxCacheMb", 500);
        payload.insert("entries", entriesArr);

        const QString maliciousJson = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

        QJSValue parseFn = engine.globalObject().property("parseStateJson");
        QVERIFY(parseFn.isCallable());
        QJSValue parsed = parseFn.call({maliciousJson});
        QCOMPARE(parsed.property("ok").toBool(), true);
        QCOMPARE(parsed.property("syncing").toBool(), true);
        QCOMPARE(parsed.property("syncState").toString(), QString("syncing"));
        QCOMPARE(parsed.property("syncPercent").toInt(), 65);

        const QString safeActive = parsed.property("activeSyncFile").toString();
        QVERIFY(!safeActive.contains('<'));
        QVERIFY(!safeActive.contains('>'));
        QVERIFY(!safeActive.contains('&'));

        QJSValue tooltipFn = engine.globalObject().property("buildTooltip");
        const QString tooltip = tooltipFn.call({parsed}).toString();
        QVERIFY(tooltip.startsWith("WickFS Smart Cache"));
        QVERIFY(!tooltip.contains('<'));
        QVERIFY(!tooltip.contains('>'));
    }

    void testStandbyWakeAndAutoStart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Backend b(dir.path() + "/state", dir.path() + "/mnt");

        QCOMPARE(b.wakeSyncCount(), 0);
        QCOMPARE(b.autoStartEnabled(), true);
        QVERIFY(!b.autoStartSummary().isEmpty());

        // Entering standby (sleeping=true) should flush state without incrementing wakeSyncCount
        b.handleSystemWake(true);
        QCOMPARE(b.wakeSyncCount(), 0);
        QVERIFY(QFileInfo::exists(dir.path() + "/state/state.json"));

        // Waking from standby (sleeping=false) should mount ~/Drive and trigger syncNow()
        b.handleSystemWake(false);
        QCOMPARE(b.wakeSyncCount(), 1);
        QVERIFY(b.mounted());
        QCOMPARE(b.syncState(), QString("synced"));
    }

    void testOAuthClientIdAndJsonImport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Backend b(dir.path() + "/state", dir.path() + "/mnt");
        const QString cfgPath = dir.path() + "/rclone.conf";

        // Pre-seed a token in rclone.conf like an authorized remote
        write(cfgPath,
              "[gdrive]\n"
              "type = drive\n"
              "client_id = \n"
              "client_secret = \n"
              "token = {\"access_token\":\"test-tok\",\"refresh_token\":\"test-ref\"}\n");
        b.setRcloneConfigPath(cfgPath);

        QCOMPARE(b.oauthAuthorized(), true);
        QCOMPARE(b.oauthClientId(), QString(""));
        QCOMPARE(b.oauthClientModeLabel(), QString("Built-in Google OAuth Client ID"));

        // Import a Google Cloud client_secret.json file while preserving the existing token
        const QString jsonPath = dir.path() + "/client_secret.json";
        write(jsonPath,
              "{\"installed\":{\"client_id\":\"12345-test.apps.googleusercontent.com\","
              "\"client_secret\":\"GOCSPX-secret123\"}}");

        QString err;
        QVERIFY2(b.importGoogleClientSecretJson(jsonPath, "gdrive", &err), qPrintable(err));
        QCOMPARE(b.oauthAuthorized(), true);
        QCOMPARE(b.oauthClientId(), QString("12345-test.apps.googleusercontent.com"));
        QCOMPARE(b.oauthClientSecret(), QString("GOCSPX-secret123"));
        QCOMPARE(b.oauthClientModeLabel(), QString("Custom Google OAuth Client ID"));

        // Reset back to built-in Client ID while keeping the token intact
        QVERIFY(b.configureOAuthClient("gdrive", "", ""));
        QCOMPARE(b.oauthAuthorized(), true);
        QCOMPARE(b.oauthClientId(), QString(""));
    }

    void rcloneSubdirectoriesAppearInVfsAndSurviveRootSync()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString fakeBinDir = dir.path() + "/bin";
        QVERIFY(QDir().mkpath(fakeBinDir));
        const QString fakeRclone = fakeBinDir + "/rclone";
        write(fakeRclone,
              "#!/bin/sh\n"
              "if [ \"$1\" = \"listremotes\" ]; then\n"
              "  echo \"gdrive:\"\n"
              "  exit 0\n"
              "fi\n"
              "if [ \"$1\" = \"lsjson\" ]; then\n"
              "  for a in \"$@\"; do last=\"$a\"; done\n"
              "  case \"$last\" in\n"
              "    \"gdrive:\")\n"
              "      echo '[{\"Path\":\"A\",\"Name\":\"A\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"}]'\n"
              "      ;;\n"
              "    \"gdrive:A\")\n"
              "      echo '[{\"Path\":\"Antigravity Agent\",\"Name\":\"Antigravity Agent\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"},"\
              "{\"Path\":\"Antigravity Agent/Guide.pdf\",\"Name\":\"Guide.pdf\",\"Size\":1024,\"IsDir\":false,\"ModTime\":\"2026-11-15T10:00:00Z\"},"\
              "{\"Path\":\"Antigravity Agent/Antigravity Projects\",\"Name\":\"Antigravity Projects\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"}]'\n"
              "      ;;\n"
              "    \"gdrive:A/Antigravity Agent\")\n"
              "      echo '[{\"Path\":\"Antigravity Projects\",\"Name\":\"Antigravity Projects\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"},"\
              "{\"Path\":\"EmptySubdir\",\"Name\":\"EmptySubdir\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"},"\
              "{\"Path\":\"Guide.pdf\",\"Name\":\"Guide.pdf\",\"Size\":1024,\"IsDir\":false,\"ModTime\":\"2026-11-15T10:00:00Z\"}]'\n"
              "      ;;\n"
              "    \"gdrive:A/Antigravity Agent/Antigravity Projects\")\n"
              "      echo '[{\"Path\":\"BLSLLM\",\"Name\":\"BLSLLM\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"},"\
              "{\"Path\":\"CONVERSATION_HISTORY.md\",\"Name\":\"CONVERSATION_HISTORY.md\",\"Size\":2048,\"IsDir\":false,\"ModTime\":\"2026-11-15T10:00:00Z\"}]'\n"
              "      ;;\n"
              "    *)\n"
              "      echo '[]'\n"
              "      ;;\n"
              "  esac\n"
              "  exit 0\n"
              "fi\n"
              "exit 1\n");
        QVERIFY(QFile::setPermissions(fakeRclone, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

        const QByteArray oldPath = qgetenv("PATH");
        const QByteArray oldSyncBlocking = qgetenv("WICKFS_SYNC_BLOCKING");
        auto restoreEnv = qScopeGuard([&]() {
            qputenv("PATH", oldPath);
            if (oldSyncBlocking.isEmpty())
                qunsetenv("WICKFS_SYNC_BLOCKING");
            else
                qputenv("WICKFS_SYNC_BLOCKING", oldSyncBlocking);
        });
        qputenv("PATH", fakeBinDir.toUtf8() + ":" + oldPath);
        qputenv("WICKFS_SYNC_BLOCKING", "1");

        const QString cfgPath = dir.path() + "/rclone.conf";
        write(cfgPath, "[gdrive]\ntype = drive\ntoken = {\"access_token\":\"tok\"}\n");

        Backend b(dir.path() + "/state", dir.path() + "/mnt");
        b.setRcloneConfigPath(cfgPath);
        QVERIFY(b.connectRcloneRemote("gdrive"));

        // Selecting folder "A" syncs depth 2 with directories included, so "Antigravity Projects" appears.
        b.setCurrentFolder("A");
        FuseMount *vfs = b.fuseMount();
        QVERIFY(vfs != nullptr);

        int err = 0;
        const QStringList agentEntries = vfs->handleReaddir("/A/Antigravity Agent", &err);
        QCOMPARE(err, 0);
        QVERIFY(agentEntries.contains("Guide.pdf"));
        QVERIFY(agentEntries.contains("Antigravity Projects"));
        QVERIFY(agentEntries.contains("EmptySubdir"));

        struct stat st {};
        QCOMPARE(vfs->handleGetattr("/A/Antigravity Agent/Antigravity Projects", &st), 0);
        QVERIFY(S_ISDIR(st.st_mode));

        // Opening the deeper subdirectory triggers on-demand readdir sync at depth 3+.
        const QStringList projectEntries = vfs->handleReaddir("/A/Antigravity Agent/Antigravity Projects", &err);
        QCOMPARE(err, 0);
        QVERIFY(projectEntries.contains("BLSLLM"));
        QVERIFY(projectEntries.contains("CONVERSATION_HISTORY.md"));

        // Periodic root sync must preserve discovered nested subdirectories.
        b.setCurrentFolder("");
        b.syncNow();
        QVERIFY(b.folders().contains("A/Antigravity Agent/Antigravity Projects"));
        QVERIFY(b.folders().contains("A/Antigravity Agent/Antigravity Projects/BLSLLM"));
    }

    void renameFilesAndDirectoriesInVfs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Backend b(dir.filePath("state"), dir.filePath("mnt"));
        FuseMount *vfs = b.fuseMount();
        QVERIFY(vfs != nullptr);

        // Create a new local file (simulating "Untitled Document") and write content.
        QString localPath;
        QVERIFY(b.vfsCreateFile("Architecture/Untitled Document", &localPath));
        const QByteArray payload("Hello rename test\n");
        QCOMPARE(vfs->handleWriteFile("/Architecture/Untitled Document", payload.constData(), payload.size(), 0),
                 static_cast<int>(payload.size()));

        // Rename file in place.
        QCOMPARE(vfs->handleRename("/Architecture/Untitled Document", "/Architecture/Testing.txt", 0), 0);
        struct stat st {};
        QCOMPARE(vfs->handleGetattr("/Architecture/Untitled Document", &st), -ENOENT);
        QCOMPARE(vfs->handleGetattr("/Architecture/Testing.txt", &st), 0);
        QCOMPARE(b.entryForPath("Architecture/Testing.txt").name, QString("Testing.txt"));
        QVERIFY(QFileInfo::exists(b.localBlobPath("Architecture/Testing.txt")));
        QVERIFY(!QFileInfo::exists(b.localBlobPath("Architecture/Untitled Document")));

        char buf[64] {};
        const int readN = vfs->handleOpenAndRead("/Architecture/Testing.txt", buf, sizeof(buf) - 1, 0);
        QCOMPARE(readN, payload.size());
        QCOMPARE(QByteArray(buf, readN), payload);

        // Move file across directories.
        QCOMPARE(vfs->handleRename("/Architecture/Testing.txt", "/Design/Testing.txt", 0), 0);
        QCOMPARE(vfs->handleGetattr("/Architecture/Testing.txt", &st), -ENOENT);
        QCOMPARE(vfs->handleGetattr("/Design/Testing.txt", &st), 0);
        QCOMPARE(b.entryForPath("Design/Testing.txt").folder, QString("Design"));

        // RENAME_NOREPLACE rejects overwriting an existing file.
        QCOMPARE(vfs->handleRename("/Design/Testing.txt", "/Design/Brand-Component-Kit.fig", RENAME_NOREPLACE),
                 -EEXIST);

        // Overwrite without RENAME_NOREPLACE succeeds.
        QCOMPARE(vfs->handleRename("/Design/Testing.txt", "/Design/Brand-Component-Kit.fig", 0), 0);
        QCOMPARE(vfs->handleGetattr("/Design/Testing.txt", &st), -ENOENT);
        QCOMPARE(b.entryForPath("Design/Brand-Component-Kit.fig").sizeBytes, static_cast<qint64>(payload.size()));

        // POSIX error cases.
        QCOMPARE(vfs->handleRename("/NoSuchFile.txt", "/Other.txt", 0), -ENOENT);
        QCOMPARE(vfs->handleRename("/Design/Brand-Component-Kit.fig", "/MissingDir/File.txt", 0), -ENOENT);
        QCOMPARE(vfs->handleRename("/Design/Brand-Component-Kit.fig", "/Architecture", 0), -EISDIR);
        QCOMPARE(vfs->handleRename("/Architecture", "/Design/Brand-Component-Kit.fig", 0), -ENOTDIR);
        QCOMPARE(vfs->handleRename("/Architecture", "/Design", 0), -ENOTEMPTY);
        QCOMPARE(vfs->handleRename("/Architecture", "/Architecture/Sub", 0), -EINVAL);
        QCOMPARE(vfs->handleRename("/", "/RootRename", 0), -EBUSY);
        QCOMPARE(vfs->handleRename("/Architecture", "/Architecture2", 0x2 /* RENAME_EXCHANGE */), -EINVAL);

        // Rename an entire directory and verify nested files move with it.
        QCOMPARE(vfs->handleRename("/Architecture", "/ArchitectureRenamed", 0), 0);
        QCOMPARE(vfs->handleGetattr("/Architecture", &st), -ENOENT);
        QCOMPARE(vfs->handleGetattr("/ArchitectureRenamed", &st), 0);
        QVERIFY(S_ISDIR(st.st_mode));

        int err = 0;
        const QStringList renamedDirEntries = vfs->handleReaddir("/ArchitectureRenamed", &err);
        QCOMPARE(err, 0);
        QVERIFY(renamedDirEntries.contains("Distributed-Cache-Spec.pdf"));
        QVERIFY(renamedDirEntries.contains("API-Security-Review.md"));
        QCOMPARE(b.entryForPath("ArchitectureRenamed/Distributed-Cache-Spec.pdf").folder,
                 QString("ArchitectureRenamed"));
        QVERIFY(QFileInfo::exists(b.localBlobPath("ArchitectureRenamed/Distributed-Cache-Spec.pdf")));
    }

    void renameInLocalFolderAndRcloneModes()
    {
        // 1. Local source folder mode propagates renames to source folder.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString remoteDir = dir.filePath("remote_cloud");
            QVERIFY(QDir().mkpath(remoteDir + "/Docs"));
            write(remoteDir + "/Docs/Draft.txt", "draft body\n");

            Backend b(dir.filePath("state"), dir.filePath("mnt"));
            QVERIFY(b.connectLocalSource(remoteDir));
            FuseMount *vfs = b.fuseMount();
            QVERIFY(vfs != nullptr);

            QCOMPARE(vfs->handleRename("/Docs/Draft.txt", "/Docs/Final.txt", 0), 0);
            QVERIFY(!QFileInfo::exists(remoteDir + "/Docs/Draft.txt"));
            QVERIFY(QFileInfo::exists(remoteDir + "/Docs/Final.txt"));

            QCOMPARE(vfs->handleRename("/Docs", "/Archive", 0), 0);
            QVERIFY(!QFileInfo::exists(remoteDir + "/Docs"));
            QVERIFY(QFileInfo::exists(remoteDir + "/Archive/Final.txt"));
        }

        // 2. rclone mode invokes `rclone moveto` for remote files & dirs and allows local-only renames.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString fakeBinDir = dir.path() + "/bin";
            QVERIFY(QDir().mkpath(fakeBinDir));
            const QString movetoLog = dir.path() + "/moveto.log";
            const QString fakeRclone = fakeBinDir + "/rclone";

            write(fakeRclone,
                  "#!/bin/sh\n"
                  "if [ \"$1\" = \"listremotes\" ]; then\n"
                  "  echo \"gdrive:\"\n"
                  "  exit 0\n"
                  "fi\n"
                  "if [ \"$1\" = \"lsjson\" ]; then\n"
                  "  echo '[{\"Path\":\"Projects\",\"Name\":\"Projects\",\"Size\":0,\"IsDir\":true,\"ModTime\":\"2026-11-15T10:00:00Z\"},"
                  "{\"Path\":\"Notes.txt\",\"Name\":\"Notes.txt\",\"Size\":12,\"IsDir\":false,\"ModTime\":\"2026-11-15T10:00:00Z\"}]'\n"
                  "  exit 0\n"
                  "fi\n"
                  "if [ \"$1\" = \"moveto\" ]; then\n"
                  "  src=\"\"; dst=\"\"\n"
                  "  for a in \"$@\"; do src=\"$dst\"; dst=\"$a\"; done\n"
                  "  echo \"$src -> $dst\" >> '" + movetoLog.toUtf8() + "'\n"
                  "  case \"$src\" in\n"
                  "    *Untitled*)\n"
                  "      echo \"ERROR : $src: object not found\" >&2\n"
                  "      exit 3\n"
                  "      ;;\n"
                  "  esac\n"
                  "  exit 0\n"
                  "fi\n"
                  "exit 1\n");
            QVERIFY(QFile::setPermissions(fakeRclone, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

            const QByteArray oldPath = qgetenv("PATH");
            const QByteArray oldSyncBlocking = qgetenv("WICKFS_SYNC_BLOCKING");
            auto restoreEnv = qScopeGuard([&]() {
                qputenv("PATH", oldPath);
                if (oldSyncBlocking.isEmpty())
                    qunsetenv("WICKFS_SYNC_BLOCKING");
                else
                    qputenv("WICKFS_SYNC_BLOCKING", oldSyncBlocking);
            });
            qputenv("PATH", fakeBinDir.toUtf8() + ":" + oldPath);
            qputenv("WICKFS_SYNC_BLOCKING", "1");

            const QString cfgPath = dir.path() + "/rclone.conf";
            write(cfgPath, "[gdrive]\ntype = drive\ntoken = {\"access_token\":\"tok\"}\n");

            Backend b(dir.path() + "/state", dir.path() + "/mnt");
            b.setRcloneConfigPath(cfgPath);
            QVERIFY(b.connectRcloneRemote("gdrive"));
            FuseMount *vfs = b.fuseMount();
            QVERIFY(vfs != nullptr);

            // Rename remote cloud-only file & remote directory.
            QCOMPARE(vfs->handleRename("/Notes.txt", "/NotesRenamed.txt", 0), 0);
            QCOMPARE(vfs->handleRename("/Projects", "/ProjectsRenamed", 0), 0);

            // Rename locally-created file that is not on the remote yet ("object not found").
            QString localPath;
            QVERIFY(b.vfsCreateFile("ProjectsRenamed/Untitled Document", &localPath));
            QCOMPARE(vfs->handleRename("/ProjectsRenamed/Untitled Document",
                                       "/ProjectsRenamed/Testing", 0), 0);

            QFile logFile(movetoLog);
            QVERIFY(logFile.open(QIODevice::ReadOnly));
            const QString logText = QString::fromUtf8(logFile.readAll());
            QVERIFY(logText.contains("gdrive:Notes.txt -> gdrive:NotesRenamed.txt"));
            QVERIFY(logText.contains("gdrive:Projects -> gdrive:ProjectsRenamed"));
            QVERIFY(logText.contains("gdrive:ProjectsRenamed/Untitled Document -> gdrive:ProjectsRenamed/Testing"));

            struct stat st {};
            QCOMPARE(vfs->handleGetattr("/ProjectsRenamed/Testing", &st), 0);
            QCOMPARE(vfs->handleGetattr("/NotesRenamed.txt", &st), 0);
        }
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(WickfsTests)
#include "wickfs_tests.moc"
