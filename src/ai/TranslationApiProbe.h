#pragma once

#include "ai/TranslationApiSettings.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
QT_END_NAMESPACE

namespace asmr {

enum class TranslationApiProbeState { Succeeded, Failed, Cancelled, TimedOut };

// Deliberately excludes raw service responses, URLs and credentials.
struct TranslationApiProbeResult {
    TranslationApiProbeState state = TranslationApiProbeState::Failed;
    QString message;
    int statusCode = 0;
    qint64 elapsedMs = 0;
    qint64 promptTokens = -1;
    qint64 completionTokens = -1;
};

class TranslationApiProbe final : public QObject {
    Q_OBJECT
public:
    explicit TranslationApiProbe(QObject *parent = nullptr);
    ~TranslationApiProbe() override;

    // Only returns false when another probe is active. Validation failures are
    // reported asynchronously via finished(), without sending a request.
    bool start(const TranslationApiSettings &settings, const QString &apiKey);
    void cancel();
    [[nodiscard]] bool isBusy() const { return m_busy; }
    [[nodiscard]] TranslationApiProbeResult result() const { return m_result; }
    [[nodiscard]] static QString sampleText();
    [[nodiscard]] static QByteArray fixedRequestBody(const TranslationApiSettings &settings);

signals:
    void busyChanged(bool busy);
    void finished(asmr::TranslationApiProbeResult result);

private:
    void readAvailable(QNetworkReply *reply, quint64 generation);
    void replyFinished(QNetworkReply *reply, quint64 generation);
    void complete(TranslationApiProbeResult result);

    QNetworkAccessManager *m_manager = nullptr;
    QTimer *m_timeout = nullptr;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_response;
    qsizetype m_maximumResponseBytes = 64 * 1024;
    QElapsedTimer m_elapsed;
    TranslationApiProbeResult m_result;
    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_completing = false;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::TranslationApiProbeState)
Q_DECLARE_METATYPE(asmr::TranslationApiProbeResult)
