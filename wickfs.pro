QT += core gui qml quick quickcontrols2 dbus network

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += fuse3
TARGET = wickfs
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/filepicker.h \
    src/fusemount.h \
    src/portalfilepicker.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/fusemount.cpp \
    src/portalfilepicker.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
