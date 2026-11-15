#pragma once

#include <QStringList>

bool isCliInvocation(const QStringList &args);
int runCli(int argc, char *argv[]);
