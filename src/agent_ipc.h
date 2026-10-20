#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace fc {
// The endpoint is scoped to the current user and graphical session. It is local,
// never TCP, and its socket permits access only by the current user.
QString agentServerName();
QJsonObject sendAgentRequest(const QJsonObject& request, int timeoutMs = 15000, const QString& endpoint = {});
class AgentServer : public QObject {
  public:
    AgentServer(QObject* parent, std::function<QJsonObject(const QJsonObject&)> handler,
                const QString& endpoint = {});
    ~AgentServer() override;
    bool listen(QString& error);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace fc
