#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>

struct FileFilter {
    QString label;
    QStringList patterns;
};

// Abstract file picker so the app uses xdg-desktop-portal in production and
// tests can inject a deterministic fake without touching D-Bus.
class FilePicker {
public:
    virtual ~FilePicker() = default;

    using Callback = std::function<void(const QString &path)>;

    virtual void openFile(const QString &title, const QList<FileFilter> &filters, Callback callback) = 0;
    virtual void saveFile(const QString &title, const QString &currentName,
                          const QList<FileFilter> &filters, Callback callback) = 0;
    virtual void openDirectory(const QString &title, Callback callback) = 0;
};
