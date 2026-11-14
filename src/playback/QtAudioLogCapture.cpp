#include "playback/QtAudioLogCapture.h"

#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>

#include <cstring>

namespace asmr {
namespace {
QMutex captureMutex;
QtAudioLogCapture *activeCapture = nullptr;
QtMessageHandler previousHandler = nullptr;

QString utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
}

QtAudioLogCapture::~QtAudioLogCapture()
{
    stop();
}

bool QtAudioLogCapture::start()
{
    const QMutexLocker lock(&captureMutex);
    if (activeCapture) return activeCapture == this;
    m_started = utcNow();
    m_stopped.clear();
    m_messages = {};
    m_omitted = 0;
    activeCapture = this;
    previousHandler = qInstallMessageHandler(&QtAudioLogCapture::handleMessage);
    return true;
}

void QtAudioLogCapture::stop()
{
    const QMutexLocker lock(&captureMutex);
    if (activeCapture != this) return;
    qInstallMessageHandler(previousHandler);
    activeCapture = nullptr;
    m_stopped = utcNow();
}

bool QtAudioLogCapture::isActive() const
{
    const QMutexLocker lock(&captureMutex);
    return activeCapture == this;
}

QJsonObject QtAudioLogCapture::snapshot() const
{
    const QMutexLocker lock(&captureMutex);
    if (m_started.isEmpty()) return {};
    return {{QStringLiteral("started_utc"), m_started},
            {QStringLiteral("stopped_utc"), m_stopped},
            {QStringLiteral("messages"), m_messages},
            {QStringLiteral("omitted"), m_omitted}};
}

void QtAudioLogCapture::handleMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QtMessageHandler forward;
    {
        const QMutexLocker lock(&captureMutex);
        forward = previousHandler;
        const char *category = context.category;
        if (activeCapture && (type == QtWarningMsg || type == QtCriticalMsg) && category
            && (std::strcmp(category, "qt.multimedia.audiooutput") == 0
                || std::strcmp(category, "qt.multimedia.ffmpeg.audiorenderer") == 0)) {
            if (activeCapture->m_messages.size() < 128) {
                activeCapture->m_messages.append(QJsonObject{
                    {QStringLiteral("utc"), utcNow()},
                    {QStringLiteral("category"), QString::fromLatin1(category)},
                    {QStringLiteral("level"), type == QtWarningMsg ? QStringLiteral("warning") : QStringLiteral("critical")},
                    {QStringLiteral("message"), message.left(1024)}});
            } else {
                ++activeCapture->m_omitted;
            }
        }
    }
    if (forward) forward(type, context, message);
}

} // namespace asmr
