#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(QString windowBg READ windowBg NOTIFY changed)
    Q_PROPERTY(QString panelBg READ panelBg NOTIFY changed)
    Q_PROPERTY(QString cardBg READ cardBg NOTIFY changed)
    Q_PROPERTY(QString cardSelectedBg READ cardSelectedBg NOTIFY changed)
    Q_PROPERTY(QString inputBg READ inputBg NOTIFY changed)
    Q_PROPERTY(QString border READ border NOTIFY changed)
    Q_PROPERTY(QString borderStrong READ borderStrong NOTIFY changed)
    Q_PROPERTY(QString textPrimary READ textPrimary NOTIFY changed)
    Q_PROPERTY(QString textSecondary READ textSecondary NOTIFY changed)
    Q_PROPERTY(QString textMuted READ textMuted NOTIFY changed)
    Q_PROPERTY(QString green READ green NOTIFY changed)
    Q_PROPERTY(QString yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QString red READ red NOTIFY changed)
    Q_PROPERTY(QString cyan READ cyan NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }

    QString windowBg() const { return m_windowBg; }
    QString panelBg() const { return m_panelBg; }
    QString cardBg() const { return m_cardBg; }
    QString cardSelectedBg() const { return m_cardSelectedBg; }
    QString inputBg() const { return m_inputBg; }
    QString border() const { return m_border; }
    QString borderStrong() const { return m_borderStrong; }
    QString textPrimary() const { return m_textPrimary; }
    QString textSecondary() const { return m_textSecondary; }
    QString textMuted() const { return m_textMuted; }
    QString green() const { return m_green; }
    QString yellow() const { return m_yellow; }
    QString red() const { return m_red; }
    QString cyan() const { return m_cyan; }

    static QString readAccent(const QString &path);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();
    bool applyColors(const QString &path);

    QString m_directory;
    QString m_accent = "#FFD60A";
    QString m_windowBg = "#0e0e10";
    QString m_panelBg = "#141418";
    QString m_cardBg = "#1a1a20";
    QString m_cardSelectedBg = "#24242d";
    QString m_inputBg = "#121216";
    QString m_border = "#282832";
    QString m_borderStrong = "#3a3a48";
    QString m_textPrimary = "#f2f2f7";
    QString m_textSecondary = "#b8b8c4";
    QString m_textMuted = "#7c7c8a";
    QString m_green = "#32d74b";
    QString m_yellow = "#ffd60a";
    QString m_red = "#ff453a";
    QString m_cyan = "#64d2ff";

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
