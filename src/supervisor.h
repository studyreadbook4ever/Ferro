#pragma once
#include "project.h"
#include <QObject>
#include <QStringList>
#include <memory>
#include <vector>

namespace fc {
class Supervisor : public QObject {
    Q_OBJECT
  public:
    explicit Supervisor(QObject* parent = nullptr, int workers = 2, int recycleAfter = 32,
                        int renderTimeoutMs = 60000);
    ~Supervisor() override;
    void render(const Project& project);
    void reload();
    void stop();
    QStringList statusLines() const;
    int generation() const;
    int restartCount() const;
    QList<qint64> workerPids() const;
  signals:
    void mixReady(const std::vector<float>& samples, const fc::Project& project);
    void statusChanged();
    void errorOccurred(const QString& message);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace fc
