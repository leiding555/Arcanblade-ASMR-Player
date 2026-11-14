#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QtLogging>

namespace asmr {

class QtAudioLogCapture final
{
public:
    QtAudioLogCapture() = default;
    ~QtAudioLogCapture();
    Q_DISABLE_COPY_MOVE(QtAudioLogCapture)

    bool start();
    void stop();
    [[nodiscard]] bool isActive() const;
    [[nodiscard]] QJsonObject snapshot() const;

private:
    static void handleMessage(QtMsgType type, const QMessageLogContext &context, const QString &message);
    QString m_started;
    QString m_stopped;
    QJsonArray m_messages;
    qint64 m_omitted = 0;
};

} // namespace asmr
