#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

#include "backend.h"
#include "fusemount.h"
#include "theme.h"
#include "translator.h"

class GDriveTests : public QObject {
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

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(GDriveTests)
#include "gdrive_tests.moc"
