#include "cli.h"
#include "backend.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

namespace {

void printErr(const QString &msg, bool json) {
    if (json) {
        QJsonObject o;
        o.insert("ok", false);
        o.insert("error", msg);
        std::fprintf(stderr, "%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    } else {
        std::fprintf(stderr, "%s\n", msg.toUtf8().constData());
    }
}

void printOut(const QJsonObject &o) {
    std::fprintf(stdout, "%s\n", QJsonDocument(o).toJson(QJsonDocument::Indented).constData());
}

void printUsage() {
    std::fprintf(stdout,
        "gdrive — Smart on-demand Google Drive virtual filesystem & LFU/LRU cache manager\n\n"
        "Usage:\n"
        "  gdrive [open] [manifest.json|folder]                        Open the desktop app\n"
        "  gdrive check [--json]                                       Verify FUSE3, rclone, and cache readiness\n"
        "  gdrive status [--state-dir DIR] [--json]                    Show cache usage, disk budget, and file counts\n"
        "  gdrive ls [--filter all|frequent|pinned|cached|cloud] [--json]\n"
        "  gdrive access <path> [--state-dir DIR] [--json]             Hydrate file on-demand & record access\n"
        "  gdrive pin <path> [--state-dir DIR] [--json]                Pin file offline (never evicted)\n"
        "  gdrive unpin <path> [--state-dir DIR] [--json]              Return pinned file to smart cache\n"
        "  gdrive evict [<path>|--cold] [--state-dir DIR] [--json]     Evict a file or all cold cached files\n"
        "  gdrive budget [--max-mb N] [--min-free-gb N] [--threshold N] [--json]\n"
        "  gdrive sync [--remote NAME] [--folder DIR] [--json]         Sync catalog & enforce cache budget\n"
        "  gdrive skill install [--json]                               Install the gdrive agent skill\n"
        "  gdrive --help                                               Show this help\n");
}

int cmdSkillInstall(bool json) {
    QFile src(":/skill/SKILL.md");
    if (!src.open(QIODevice::ReadOnly)) {
        printErr("Bundled skill resource missing.", json);
        return 1;
    }
    const QByteArray data = src.readAll();
    const QString destDir = QDir::homePath() + "/.agents/skills/gdrive";
    QDir().mkpath(destDir);
    const QString destPath = destDir + "/SKILL.md";
    QSaveFile out(destPath);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        printErr("Failed to write skill to " + destPath, json);
        return 1;
    }
    if (json) {
        printOut({{"ok", true}, {"path", destPath}});
    } else {
        std::fprintf(stdout, "Installed skill to %s\n", destPath.toUtf8().constData());
    }
    return 0;
}

QString extractOption(const QStringList &args, const QString &flag) {
    for (int i = 2; i < args.size(); ++i) {
        if (args.at(i) == flag && i + 1 < args.size())
            return args.at(i + 1);
    }
    return {};
}

QString extractPositional(const QStringList &args) {
    for (int i = 2; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == "--state-dir" || a == "--filter" || a == "--max-mb"
            || a == "--min-free-gb" || a == "--threshold" || a == "--remote" || a == "--folder") {
            ++i;
            continue;
        }
        if (!a.startsWith('-'))
            return a;
    }
    return {};
}

} // namespace

bool isCliInvocation(const QStringList &args) {
    if (args.size() < 2)
        return false;
    const QString cmd = args.at(1);
    return cmd == "check" || cmd == "status" || cmd == "ls" || cmd == "access"
        || cmd == "pin" || cmd == "unpin" || cmd == "evict" || cmd == "budget"
        || cmd == "sync" || cmd == "skill"
        || cmd == "--help" || cmd == "-h" || cmd == "--version" || cmd == "-v";
}

int runCli(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("QT_QPA_PLATFORMTHEME");
    QGuiApplication app(argc, argv);
    app.setApplicationName("gdrive");
    app.setApplicationVersion("0.1.0");

    const QStringList args = app.arguments();
    const QString cmd = args.value(1);
    const bool json = args.contains("--json");

    if (cmd == "--help" || cmd == "-h") {
        printUsage();
        return 0;
    }
    if (cmd == "--version" || cmd == "-v") {
        std::fprintf(stdout, "gdrive 0.1.0\n");
        return 0;
    }
    if (cmd == "skill") {
        if (args.value(2) == "install")
            return cmdSkillInstall(json);
        printErr("Usage: gdrive skill install [--json]", json);
        return 1;
    }

    const QString stateDir = extractOption(args, "--state-dir");
    const QString mountDir = stateDir.isEmpty() ? QString() : (stateDir + "/mnt");
    Backend backend(stateDir, mountDir);

    if (cmd == "check") {
        const bool ok = backend.fuseAvailable();
        if (json) {
            QJsonObject o;
            o.insert("ok", ok);
            o.insert("fuseAvailable", backend.fuseAvailable());
            o.insert("rcloneAvailable", backend.rcloneAvailable());
            o.insert("sourceMode", backend.sourceMode());
            o.insert("remoteName", backend.remoteName());
            o.insert("mountPoint", backend.mountPoint());
            o.insert("status", backend.statusBannerText());
            printOut(o);
        } else {
            std::fprintf(stdout, "FUSE3 available: %s\n", backend.fuseAvailable() ? "yes" : "no");
            std::fprintf(stdout, "rclone available: %s\n", backend.rcloneAvailable() ? "yes" : "no");
            std::fprintf(stdout, "Mount point: %s\n", backend.mountPoint().toUtf8().constData());
            std::fprintf(stdout, "Status: %s\n", backend.statusBannerText().toUtf8().constData());
        }
        return ok ? 0 : 1;
    }

    if (cmd == "status") {
        QJsonObject o;
        o.insert("ok", true);
        o.insert("sourceMode", backend.sourceMode());
        o.insert("connection", backend.connectionSummary());
        o.insert("mountPoint", backend.mountPoint());
        o.insert("totalCloudBytes", static_cast<double>(backend.totalCloudBytes()));
        o.insert("cachedBytes", static_cast<double>(backend.cachedBytes()));
        o.insert("pinnedBytes", static_cast<double>(backend.pinnedBytes()));
        o.insert("hotCachedBytes", static_cast<double>(backend.hotCachedBytes()));
        o.insert("savedBytes", static_cast<double>(backend.savedBytes()));
        o.insert("maxCacheBytes", static_cast<double>(backend.maxCacheBytes()));
        o.insert("effectiveBudgetBytes", static_cast<double>(backend.effectiveBudgetBytes()));
        o.insert("diskAvailableBytes", static_cast<double>(backend.diskAvailableBytes()));
        o.insert("spaceSavedPercent", backend.spaceSavedPercent());
        o.insert("totalFiles", backend.totalFileCount());
        o.insert("frequentFiles", backend.frequentFileCount());
        o.insert("pinnedFiles", backend.pinnedFileCount());
        o.insert("cachedFiles", backend.cachedFileCount());
        o.insert("cloudOnlyFiles", backend.cloudOnlyFileCount());
        if (json) {
            printOut(o);
        } else {
            std::fprintf(stdout, "Connection:   %s\n", backend.connectionSummary().toUtf8().constData());
            std::fprintf(stdout, "Mount point:  %s\n", backend.mountPoint().toUtf8().constData());
            std::fprintf(stdout, "Local cache:  %s / %s budget (%s pinned, %s hot)\n",
                         backend.cachedFormatted().toUtf8().constData(),
                         backend.effectiveBudgetFormatted().toUtf8().constData(),
                         backend.pinnedFormatted().toUtf8().constData(),
                         backend.hotCachedFormatted().toUtf8().constData());
            std::fprintf(stdout, "Cloud total:  %s (%d%% disk space saved: %s in cloud)\n",
                         backend.totalCloudFormatted().toUtf8().constData(),
                         backend.spaceSavedPercent(),
                         backend.savedFormatted().toUtf8().constData());
            std::fprintf(stdout, "Files:        %d total (%d frequent, %d pinned, %d cached, %d cloud-only)\n",
                         backend.totalFileCount(),
                         backend.frequentFileCount(),
                         backend.pinnedFileCount(),
                         backend.cachedFileCount(),
                         backend.cloudOnlyFileCount());
        }
        return 0;
    }

    if (cmd == "ls") {
        const QString filter = extractOption(args, "--filter");
        if (!filter.isEmpty())
            backend.setFilterMode(filter);

        QJsonArray arr;
        const auto &entries = backend.fileModel()->entries();
        for (const auto &e : entries) {
            QJsonObject item;
            item.insert("path", e.path);
            item.insert("name", e.name);
            item.insert("folder", e.folder);
            item.insert("category", e.category);
            item.insert("sizeBytes", static_cast<double>(e.sizeBytes));
            item.insert("sizeFormatted", Backend::formatSize(e.sizeBytes));
            item.insert("state", e.state);
            item.insert("pinned", e.pinned);
            item.insert("accessCount", e.accessCount);
            item.insert("hotScore", e.hotScore);
            item.insert("isFrequent", e.isFrequent);
            arr.append(item);
        }
        if (json) {
            printOut({{"ok", true}, {"count", arr.size()}, {"files", arr}});
        } else {
            for (const auto &e : entries) {
                std::fprintf(stdout, "%-8s  %-9s  %3dx  score:%5.1f  %s\n",
                             e.state.toUpper().toUtf8().constData(),
                             Backend::formatSize(e.sizeBytes).toUtf8().constData(),
                             e.accessCount,
                             e.hotScore,
                             e.path.toUtf8().constData());
            }
        }
        return 0;
    }

    if (cmd == "access") {
        const QString target = extractPositional(args);
        if (target.isEmpty()) {
            printErr("Usage: gdrive access <path> [--json]", json);
            return 1;
        }
        QString err;
        if (!backend.accessFile(target, &err)) {
            printErr(err, json);
            return 1;
        }
        const DriveFileEntry e = backend.entryForPath(target);
        if (json) {
            printOut({
                {"ok", true},
                {"path", e.path},
                {"state", e.state},
                {"accessCount", e.accessCount},
                {"hotScore", e.hotScore},
                {"isFrequent", e.isFrequent},
                {"cachedBytes", static_cast<double>(backend.cachedBytes())},
                {"localBlobPath", backend.localBlobPath(e.path)}
            });
        } else {
            std::fprintf(stdout, "Hydrated %s (accessCount=%d, hotScore=%.1f, state=%s)\n",
                         e.path.toUtf8().constData(), e.accessCount, e.hotScore, e.state.toUtf8().constData());
        }
        return 0;
    }

    if (cmd == "pin" || cmd == "unpin") {
        const QString target = extractPositional(args);
        if (target.isEmpty()) {
            printErr(QStringLiteral("Usage: gdrive %1 <path> [--json]").arg(cmd), json);
            return 1;
        }
        const bool pinVal = (cmd == "pin");
        if (!backend.setPinned(target, pinVal)) {
            printErr(QStringLiteral("File not found: %1").arg(target), json);
            return 1;
        }
        const DriveFileEntry e = backend.entryForPath(target);
        if (json) {
            printOut({
                {"ok", true},
                {"path", e.path},
                {"pinned", e.pinned},
                {"state", e.state},
                {"cachedBytes", static_cast<double>(backend.cachedBytes())}
            });
        } else {
            std::fprintf(stdout, "%s %s (state=%s)\n",
                         pinVal ? "Pinned" : "Unpinned",
                         e.path.toUtf8().constData(),
                         e.state.toUtf8().constData());
        }
        return 0;
    }

    if (cmd == "evict") {
        if (args.contains("--cold") || extractPositional(args).isEmpty()) {
            const int count = backend.evictColdFiles();
            if (json) {
                printOut({
                    {"ok", true},
                    {"evictedCount", count},
                    {"cachedBytes", static_cast<double>(backend.cachedBytes())},
                    {"savedBytes", static_cast<double>(backend.savedBytes())}
                });
            } else {
                std::fprintf(stdout, "Evicted %d cold cached file(s). Cached: %s\n",
                             count, backend.cachedFormatted().toUtf8().constData());
            }
            return 0;
        }
        const QString target = extractPositional(args);
        QString err;
        if (!backend.evictFile(target, &err)) {
            printErr(err, json);
            return 1;
        }
        if (json) {
            printOut({
                {"ok", true},
                {"path", target},
                {"cachedBytes", static_cast<double>(backend.cachedBytes())}
            });
        } else {
            std::fprintf(stdout, "Evicted %s to Cloud Only\n", target.toUtf8().constData());
        }
        return 0;
    }

    if (cmd == "budget") {
        const QString maxMbStr = extractOption(args, "--max-mb");
        const QString minFreeGbStr = extractOption(args, "--min-free-gb");
        const QString threshStr = extractOption(args, "--threshold");

        if (!maxMbStr.isEmpty())
            backend.setMaxCacheMb(maxMbStr.toInt());
        if (!minFreeGbStr.isEmpty())
            backend.setMinFreeDiskGb(minFreeGbStr.toInt());
        if (!threshStr.isEmpty())
            backend.setFrequentThreshold(threshStr.toInt());

        if (json) {
            printOut({
                {"ok", true},
                {"maxCacheMb", backend.maxCacheMb()},
                {"minFreeDiskGb", backend.minFreeDiskGb()},
                {"frequentThreshold", backend.frequentThreshold()},
                {"cachedBytes", static_cast<double>(backend.cachedBytes())},
                {"effectiveBudgetBytes", static_cast<double>(backend.effectiveBudgetBytes())}
            });
        } else {
            std::fprintf(stdout, "Max Cache: %d MB | Min Free Disk Floor: %d GB | Frequent Threshold: %dx | Currently Cached: %s\n",
                         backend.maxCacheMb(),
                         backend.minFreeDiskGb(),
                         backend.frequentThreshold(),
                         backend.cachedFormatted().toUtf8().constData());
        }
        return 0;
    }

    if (cmd == "sync") {
        const QString remote = extractOption(args, "--remote");
        const QString folder = extractOption(args, "--folder");
        if (!remote.isEmpty()) {
            if (!backend.connectRcloneRemote(remote)) {
                printErr(backend.statusBannerText(), json);
                return 1;
            }
        } else if (!folder.isEmpty()) {
            if (!backend.connectLocalSource(folder)) {
                printErr(backend.statusBannerText(), json);
                return 1;
            }
        } else {
            backend.syncNow();
        }
        if (json) {
            printOut({
                {"ok", true},
                {"sourceMode", backend.sourceMode()},
                {"totalFiles", backend.totalFileCount()},
                {"cachedBytes", static_cast<double>(backend.cachedBytes())}
            });
        } else {
            std::fprintf(stdout, "Synced %d files (%s cached)\n",
                         backend.totalFileCount(), backend.cachedFormatted().toUtf8().constData());
        }
        return 0;
    }

    printErr("Unknown command: " + cmd, json);
    return 1;
}
