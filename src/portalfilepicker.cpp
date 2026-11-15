// Portal file picker follows Omacut's pattern (MIT, David Heinemeier Hansson).
#include "portalfilepicker.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUrl>

struct PortalFilterPattern {
    uint type = 0;
    QString pattern;
};

struct PortalFilter {
    QString name;
    QList<PortalFilterPattern> patterns;
};

Q_DECLARE_METATYPE(PortalFilterPattern)
Q_DECLARE_METATYPE(QList<PortalFilterPattern>)
Q_DECLARE_METATYPE(PortalFilter)
Q_DECLARE_METATYPE(QList<PortalFilter>)

static QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilterPattern &item) {
    arg.beginStructure();
    arg << item.type << item.pattern;
    arg.endStructure();
    return arg;
}

static const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilterPattern &item) {
    arg.beginStructure();
    arg >> item.type >> item.pattern;
    arg.endStructure();
    return arg;
}

static QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilter &item) {
    arg.beginStructure();
    arg << item.name << item.patterns;
    arg.endStructure();
    return arg;
}

static const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilter &item) {
    arg.beginStructure();
    arg >> item.name >> item.patterns;
    arg.endStructure();
    return arg;
}

PortalFilePicker::PortalFilePicker(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<PortalFilterPattern>();
    qDBusRegisterMetaType<QList<PortalFilterPattern>>();
    qDBusRegisterMetaType<PortalFilter>();
    qDBusRegisterMetaType<QList<PortalFilter>>();
}

void PortalFilePicker::openFile(const QString &title, const QList<FileFilter> &filters, Callback callback) {
    callChooser("OpenFile", title, {}, filters, std::move(callback));
}

void PortalFilePicker::saveFile(const QString &title, const QString &currentName,
                                const QList<FileFilter> &filters, Callback callback) {
    QVariantMap extra;
    if (!currentName.isEmpty())
        extra.insert("current_name", currentName);
    callChooser("SaveFile", title, extra, filters, std::move(callback));
}

void PortalFilePicker::openDirectory(const QString &title, Callback callback) {
    QVariantMap extra;
    extra.insert("directory", true);
    callChooser("OpenFile", title, extra, {}, std::move(callback));
}

void PortalFilePicker::callChooser(const QString &method, const QString &title,
                                   const QVariantMap &extraOptions,
                                   const QList<FileFilter> &filters, Callback callback) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (callback)
            callback({});
        return;
    }

    if (!m_activeRequestPath.isEmpty()) {
        bus.disconnect("org.freedesktop.portal.Desktop", m_activeRequestPath,
                       "org.freedesktop.portal.Request", "Response",
                       this, SLOT(onPortalResponse(uint, QVariantMap)));
        m_activeRequestPath.clear();
    }

    m_pendingCallback = std::move(callback);

    const QString token = QStringLiteral("wickfs_%1_%2")
                              .arg(QCoreApplication::applicationPid())
                              .arg(++m_tokenCounter);
    QString sender = bus.baseService();
    if (sender.startsWith(':'))
        sender.remove(0, 1);
    sender.replace('.', '_');
    const QString predictedPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                                      .arg(sender, token);
    m_activeRequestPath = predictedPath;

    bus.connect("org.freedesktop.portal.Desktop", predictedPath,
                "org.freedesktop.portal.Request", "Response",
                this, SLOT(onPortalResponse(uint, QVariantMap)));

    QVariantMap options = extraOptions;
    options.insert("handle_token", token);
    options.insert("modal", true);

    if (!filters.isEmpty()) {
        QList<PortalFilter> portalFilters;
        for (const auto &f : filters) {
            PortalFilter pf;
            pf.name = f.label;
            for (const auto &pat : f.patterns)
                pf.patterns.append({0u, pat});
            portalFilters.append(pf);
        }
        options.insert("filters", QVariant::fromValue(portalFilters));
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser",
        method);
    msg << QString() << title << options;

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QDBusObjectPath> reply(*w);
        if (reply.isError()) {
            if (!m_activeRequestPath.isEmpty()) {
                QDBusConnection::sessionBus().disconnect(
                    "org.freedesktop.portal.Desktop", m_activeRequestPath,
                    "org.freedesktop.portal.Request", "Response",
                    this, SLOT(onPortalResponse(uint, QVariantMap)));
                m_activeRequestPath.clear();
            }
            auto cb = std::move(m_pendingCallback);
            m_pendingCallback = nullptr;
            if (cb)
                cb({});
        } else {
            const QString actualPath = reply.value().path();
            if (!actualPath.isEmpty() && actualPath != m_activeRequestPath) {
                auto bus = QDBusConnection::sessionBus();
                bus.disconnect("org.freedesktop.portal.Desktop", m_activeRequestPath,
                               "org.freedesktop.portal.Request", "Response",
                               this, SLOT(onPortalResponse(uint, QVariantMap)));
                m_activeRequestPath = actualPath;
                bus.connect("org.freedesktop.portal.Desktop", m_activeRequestPath,
                            "org.freedesktop.portal.Request", "Response",
                            this, SLOT(onPortalResponse(uint, QVariantMap)));
            }
        }
    });
}

void PortalFilePicker::onPortalResponse(uint response, const QVariantMap &results) {
    if (!m_activeRequestPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            "org.freedesktop.portal.Desktop", m_activeRequestPath,
            "org.freedesktop.portal.Request", "Response",
            this, SLOT(onPortalResponse(uint, QVariantMap)));
        m_activeRequestPath.clear();
    }

    auto cb = std::move(m_pendingCallback);
    m_pendingCallback = nullptr;
    if (!cb)
        return;

    if (response != 0) {
        cb({});
        return;
    }

    const QStringList uris = results.value("uris").toStringList();
    if (uris.isEmpty()) {
        cb({});
        return;
    }
    cb(QUrl(uris.first()).toLocalFile());
}
