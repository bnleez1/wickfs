#include <QDateTime>
#include <QDir>
#include <QFile>
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
