#pragma once

#include "filepicker.h"

#include <QDBusVariant>
#include <QObject>
#include <QVariantMap>

// Portal file picker follows Omacut's pattern (MIT, David Heinemeier Hansson).
class PortalFilePicker : public QObject, public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openFile(const QString &title, const QList<FileFilter> &filters, Callback callback) override;
    void saveFile(const QString &title, const QString &currentName,
                  const QList<FileFilter> &filters, Callback callback) override;
    void openDirectory(const QString &title, Callback callback) override;

private slots:
    void onPortalResponse(uint response, const QVariantMap &results);

private:
    void callChooser(const QString &method, const QString &title, const QVariantMap &extraOptions,
                     const QList<FileFilter> &filters, Callback callback);

    Callback m_pendingCallback;
    QString m_activeRequestPath;
    int m_tokenCounter = 0;
};
