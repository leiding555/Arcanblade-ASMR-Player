#pragma once

#include "artwork/ManualCoverStore.h"

#include <QHostAddress>
#include <QObject>

#include <memory>

class QThread;

namespace asmr {

class MobilePlaybackService final : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Listening, Stopping };
    enum class PairingCodeState { Unavailable, Ready, Used, Expired };
    struct PairingCodeInfo {
        PairingCodeState state = PairingCodeState::Unavailable;
        QString code;
    };
    explicit MobilePlaybackService(ManualCoverSettings library, QObject *parent = nullptr);
    ~MobilePlaybackService() override;

    void start(const QHostAddress &address, int prefixLength);
    void stop();
    void renewPairingCode();
    State state() const { return m_state; }
    bool isIdle() const { return m_state == State::Stopped; }
    QString address() const { return m_address; }
    PairingCodeInfo pairingCodeInfo() const;
    QString pairingCode() const;
    QString error() const { return m_error; }

signals:
    void changed();

private:
    struct Run;
    ManualCoverSettings m_library;
    std::shared_ptr<Run> m_run;
    QThread *m_worker = nullptr;
    State m_state = State::Stopped;
    QString m_address;
    QString m_error;
};

} // namespace asmr
