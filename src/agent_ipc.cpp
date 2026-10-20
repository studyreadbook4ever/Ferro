#include "agent_ipc.h"
#include "agent.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <memory>
#include <unistd.h>

namespace fc {
namespace {
constexpr qsizetype MaxRequest = 16 * 1024 * 1024;
// Inspect includes both the normalized agent view and the native snapshot.
// Bound this separately so every permitted 16 MiB project can be inspected.
constexpr qsizetype MaxResponse = 64 * 1024 * 1024;
constexpr int ServerTimeoutMs = 15000;
struct ConnectionState {
    QByteArray bytes;
    bool handled = false;
};
QByteArray encoded(const QJsonObject& value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject decode(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return agentError("invalid_json", "Expected one JSON object: " + error.errorString());
    return document.object();
}
} // namespace
QString agentServerName() {
    const auto scope = (QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + '\n' +
                        qEnvironmentVariable("DISPLAY") + '\n' + qEnvironmentVariable("WAYLAND_DISPLAY"))
                           .toUtf8();
    const auto digest = QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex().left(20);
    return QString("ferro-agent-%1-%2").arg(qulonglong(getuid())).arg(QString::fromLatin1(digest));
}
struct AgentServer::Impl {
    QLocalServer server;
    QString endpoint;
    std::function<QJsonObject(const QJsonObject&)> handler;
    int active = 0;
};
AgentServer::AgentServer(QObject* parent, std::function<QJsonObject(const QJsonObject&)> handler,
                         const QString& endpoint)
    : QObject(parent), d(std::make_unique<Impl>()) {
    d->endpoint = endpoint.isEmpty() ? agentServerName() : endpoint;
    d->handler = std::move(handler);
    d->server.setSocketOptions(QLocalServer::UserAccessOption);
    d->server.setMaxPendingConnections(8);
    connect(&d->server, &QLocalServer::newConnection, this, [this] {
        while (auto* socket = d->server.nextPendingConnection()) {
            socket->setParent(this);
            if (socket->state() != QLocalSocket::ConnectedState || d->active >= 8) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            ++d->active;
            socket->setReadBufferSize(MaxRequest + 1);
            auto state = std::make_shared<ConnectionState>();
            auto* deadline = new QTimer(socket);
            deadline->setSingleShot(true);
            deadline->start(ServerTimeoutMs);
            connect(deadline, &QTimer::timeout, socket, [socket] { socket->abort(); });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                --d->active;
                socket->deleteLater();
            });
            auto reply = [socket, state](QJsonObject response) {
                if (state->handled)
                    return;
                state->handled = true;
                auto message = encoded(response);
                if (message.size() > MaxResponse)
                    message = encoded(agentError("message_too_large", "Response exceeds 64 MiB."));
                socket->write(message);
                socket->disconnectFromServer();
            };
            auto consume = [this, socket, state, reply] {
                if (state->handled)
                    return;
                state->bytes.append(socket->readAll());
                if (state->bytes.size() > MaxRequest) {
                    reply(agentError("message_too_large", "Request exceeds 16 MiB."));
                    return;
                }
                const auto newline = state->bytes.indexOf('\n');
                if (newline < 0)
                    return;
                if (newline != state->bytes.size() - 1) {
                    reply(agentError("invalid_request",
                                     "Send exactly one JSON object and newline per connection."));
                    return;
                }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(state->bytes.left(newline), &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    reply(agentError("invalid_json", "Expected one JSON object: " + error.errorString()));
                    return;
                }
                try {
                    reply(d->handler(document.object()));
                } catch (const std::exception& error) {
                    reply(agentError("internal_error", QString::fromUtf8(error.what())));
                } catch (...) {
                    reply(agentError("internal_error", "Request failed unexpectedly."));
                }
            };
            connect(socket, &QLocalSocket::readyRead, socket, consume);
            if (socket->bytesAvailable() > 0)
                consume();
        }
    });
}
AgentServer::~AgentServer() {
    d->server.close();
    for (auto* socket : findChildren<QLocalSocket*>()) {
        socket->disconnect(this);
        socket->abort();
    }
}
bool AgentServer::listen(QString& error) {
    if (d->server.isListening())
        return true;
    // Never unlink a listening process's endpoint. Only a connection refusal
    // (or an already absent socket) demonstrates a stale filesystem socket.
    // Probe BEFORE listen: Qt's UserAccessOption can replace an existing socket
    // by renaming its permission-controlled temporary socket over that path.
    QLocalSocket probe;
    probe.connectToServer(d->endpoint);
    if (probe.waitForConnected(150) || (probe.error() != QLocalSocket::ConnectionRefusedError &&
                                        probe.error() != QLocalSocket::ServerNotFoundError)) {
        error = "An agent endpoint is already active: " + d->endpoint;
        return false;
    }
    if (probe.error() == QLocalSocket::ConnectionRefusedError && !QLocalServer::removeServer(d->endpoint)) {
        error = "Could not remove a stale agent endpoint: " + d->endpoint;
        return false;
    }
    if (!d->server.listen(d->endpoint)) {
        error = d->server.errorString();
        return false;
    }
    return true;
}
QJsonObject sendAgentRequest(const QJsonObject& request, int timeoutMs, const QString& endpoint) {
    if (timeoutMs < 1 || timeoutMs > 300000)
        return agentError("invalid_timeout", "Timeout must be between 1 and 300000 ms.");
    const auto message = encoded(request);
    if (message.size() > MaxRequest)
        return agentError("message_too_large", "Request exceeds 16 MiB.");
    QElapsedTimer elapsed;
    elapsed.start();
    auto remaining = [&] { return std::max(0, timeoutMs - int(elapsed.elapsed())); };
    QLocalSocket socket;
    socket.setReadBufferSize(MaxResponse + 1);
    socket.connectToServer(endpoint.isEmpty() ? agentServerName() : endpoint);
    if (!socket.waitForConnected(remaining())) {
        const bool absent = socket.error() == QLocalSocket::ServerNotFoundError ||
                            socket.error() == QLocalSocket::ConnectionRefusedError;
        return agentError(absent ? "not_running" : "connection_failed",
                          "Cannot connect to Ferro. Start its window in this session. " +
                              socket.errorString());
    }
    if (socket.write(message) != message.size())
        return agentError("connection_failed", "Could not send the complete request.");
    while (socket.bytesToWrite() > 0)
        if (!remaining() || !socket.waitForBytesWritten(remaining()))
            return agentError("connection_failed", "Failed while sending request: " + socket.errorString());
    QByteArray response;
    for (;;) {
        response.append(socket.readAll());
        if (response.size() > MaxResponse)
            return agentError("message_too_large", "Response exceeds 64 MiB.");
        const auto newline = response.indexOf('\n');
        if (newline >= 0) {
            if (newline != response.size() - 1)
                return agentError("invalid_response", "Unexpected bytes after response.");
            return decode(response.left(newline));
        }
        if (!remaining())
            return agentError("timeout", "Timed out waiting for Ferro. Inspect before retrying an edit.");
        if (!socket.waitForReadyRead(remaining())) {
            if (socket.bytesAvailable() > 0)
                continue;
            return agentError(elapsed.elapsed() >= timeoutMs ? "timeout" : "connection_failed",
                              "Ferro did not return a complete response. Inspect before retrying an edit. " +
                                  socket.errorString());
        }
    }
}
} // namespace fc
