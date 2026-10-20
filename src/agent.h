#pragma once
#include "project.h"
#include <QJsonObject>
#include <QStringList>

namespace fc {
// The agent protocol deliberately uses the same units as the GUI, and 1-based
// musical coordinates. Native .ferro serialization remains an internal format.
QString agentRevision(const Project& project);
QJsonObject agentError(QString code, QString message, int operation = -1);
QJsonObject agentInspect(const Project& project);
QJsonObject agentSchema();
bool agentApply(const Project& base, const QJsonObject& plan, Project& out, QJsonObject& response);
int agentMain(const QStringList& arguments);
} // namespace fc
