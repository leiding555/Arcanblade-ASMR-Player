#pragma once

#include <QAudioBuffer>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace asmr {

struct PlaybackDiagnosticLogOptions {
    QString directory;
    qint64 maximumFileBytes = 5 * 1024 * 1024;
    int backupCount = 3;
    qsizetype maximumEvents = 1024;
    qsizetype maximumPcmBuffers = 128;
    qint64 maximumPcmBytes = 8 * 1024 * 1024;
};

struct PlaybackDiagnosticState;
class PlaybackDiagnosticThread;

// The caller supplies already-frozen values. No database, media or GUI object is
// inspected by the worker. A retained PcmSink is safe after service destruction.
class PlaybackDiagnosticLog final : public QObject {
    Q_OBJECT
public:
    explicit PlaybackDiagnosticLog(PlaybackDiagnosticLogOptions options, QObject *parent = nullptr);
    ~PlaybackDiagnosticLog() override;
    void record(QString event, QJsonObject fields = {}, QJsonObject context = {});
    using PcmSink = std::function<void(const QAudioBuffer &)>;
    [[nodiscard]] PcmSink pcmSink(QJsonObject context);
    void flushPcm(QJsonObject context, QString reason);
    void shutdown();
    [[nodiscard]] bool isShutdownFinished() const;

signals:
    void shutdownFinished();

private:
    friend class PlaybackDiagnosticLogTests;
    enum class TestPoint { BeforeOpen, BeforeWrite, BeforeRotate, BeforeFlush };
    using TestHook = std::function<QString(TestPoint)>;
    PlaybackDiagnosticLog(PlaybackDiagnosticLogOptions options, TestHook hook, QObject *parent);
    std::shared_ptr<PlaybackDiagnosticState> m_state;
    std::unique_ptr<PlaybackDiagnosticThread> m_thread;
};

} // namespace asmr
