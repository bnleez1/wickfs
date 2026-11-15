QT += core gui qml quick quickcontrols2 testlib dbus network
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += fuse3
TARGET = wickfs_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/backend.h \
    ../src/filepicker.h \
    ../src/fusemount.h \
    ../src/portalfilepicker.h \
    ../src/theme.h \
    ../src/translator.h

SOURCES += \
    wickfs_tests.cpp \
    ../src/backend.cpp \
    ../src/fusemount.cpp \
    ../src/portalfilepicker.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp

RESOURCES += ../src/resources.qrc
