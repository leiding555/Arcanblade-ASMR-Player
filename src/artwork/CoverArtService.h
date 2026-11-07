#pragma once

#include "artwork/CoverArtLoader.h"

#include <QFuture>
#include <QObject>

#include <functional>
#include <optional>

namespace asmr {
class CoverArtServiceTests;
class MainWindowArtworkTests;

class CoverArtService final : public QObject {
    Q_OBJECT
public:
    explicit CoverArtService(QObject *parent = nullptr);
    ~CoverArtService() override;
    [[nodiscard]] quint64 request(const CoverArtRequest &request);
    void shutdown();
    [[nodiscard]] bool isIdle() const;

signals:
    void resultReady(quint64 requestId, const asmr::CoverArtResult &result);
    void shutdownFinished();

private:
    friend class CoverArtServiceTests;
    friend class MainWindowArtworkTests;
    struct Pending { quint64 id; CoverArtRequest request; };
    void start(Pending pending);
    void finishShutdownIfReady();
    quint64 m_generation = 0;
    bool m_active = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    std::optional<Pending> m_pending;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::shared_ptr<CoverArtCache> m_cache;
    QFuture<CoverArtResult> m_lifetime;
    std::shared_ptr<const std::function<void()>> m_beforeLoadTestHook;
};
} // namespace asmr
