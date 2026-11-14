#include "playback/PcmDiagnosticSummary.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace asmr {
namespace {
QString sampleFormatName(QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::UInt8: return QStringLiteral("UInt8");
    case QAudioFormat::Int16: return QStringLiteral("Int16");
    case QAudioFormat::Int32: return QStringLiteral("Int32");
    case QAudioFormat::Float: return QStringLiteral("Float");
    default: return QStringLiteral("Unknown");
    }
}

template <typename T>
T readSample(const char *bytes)
{
    // QAudioBuffer data is interleaved PCM in native byte order. memcpy avoids
    // relying on alignment or aliasing when a test/decoder supplies raw bytes.
    T sample;
    std::memcpy(&sample, bytes, sizeof(sample));
    return sample;
}

double normalizedSample(const char *bytes, QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::UInt8:
        return (double(readSample<quint8>(bytes)) - 128.0) / 128.0;
    case QAudioFormat::Int16:
        return double(readSample<qint16>(bytes)) / 32768.0;
    case QAudioFormat::Int32:
        return double(readSample<qint32>(bytes)) / 2147483648.0;
    case QAudioFormat::Float:
        return double(readSample<float>(bytes));
    default:
        return 0; // The caller rejects unknown formats before reading.
    }
}
}

void PcmDiagnosticSummary::addBuffer(const QAudioBuffer &buffer)
{
    // Qt sends an empty buffer at stream end. It is not evidence that frames
    // were decoded, and it does not supersede QMediaPlayer::EndOfMedia.
    if (buffer.byteCount() == 0) {
        ++m_endMarkers;
        return;
    }
    const auto incoming = buffer.format();
    const auto sampleFormat = incoming.sampleFormat();
    const int bytesPerSample = incoming.bytesPerSample();
    const int channels = incoming.channelCount();
    const auto frames = buffer.frameCount();
    if (!buffer.isValid() || !incoming.isValid() || channels <= 0 || frames <= 0
        || bytesPerSample <= 0
        || (sampleFormat != QAudioFormat::UInt8 && sampleFormat != QAudioFormat::Int16
            && sampleFormat != QAudioFormat::Int32 && sampleFormat != QAudioFormat::Float)
        || buffer.byteCount() % incoming.bytesPerFrame() != 0
        || (hasFormat() && (incoming != m_format
            || incoming.channelConfig() != m_format.channelConfig()))) {
        ++m_invalidBuffers;
        return;
    }
    if (!hasFormat()) {
        m_format = incoming;
        m_channels.resize(channels);
    }

    const char *samples = buffer.constData<char>();
    for (qsizetype frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            const auto sample = normalizedSample(samples, sampleFormat);
            samples += bytesPerSample;
            auto &statistics = m_channels[channel];
            if (!std::isfinite(sample)) {
                ++statistics.nonFiniteSamples;
                continue;
            }
            statistics.peak = std::max(statistics.peak, std::abs(sample));
            statistics.sumSquares += sample * sample;
            ++statistics.finiteSamples;
        }
    }
    ++m_buffers;
    m_frames += frames;
}

QJsonObject PcmDiagnosticSummary::snapshot() const
{
    QJsonArray perChannel;
    for (const auto &channel : m_channels) {
        const QJsonValue rms = channel.finiteSamples > 0
            ? QJsonValue(std::sqrt(channel.sumSquares / double(channel.finiteSamples)))
            : QJsonValue(QJsonValue::Null);
        perChannel.append(QJsonObject{
            {QStringLiteral("peak"), channel.peak},
            {QStringLiteral("rms"), rms},
            {QStringLiteral("finite_samples"), channel.finiteSamples},
            {QStringLiteral("non_finite_samples"), channel.nonFiniteSamples}});
    }
    return {{QStringLiteral("buffers"), m_buffers},
            {QStringLiteral("frames"), m_frames},
            {QStringLiteral("sample_rate"), hasFormat() ? m_format.sampleRate() : 0},
            {QStringLiteral("channels"), m_channels.size()},
            {QStringLiteral("sample_format"), sampleFormatName(m_format.sampleFormat())},
            {QStringLiteral("per_channel"), perChannel},
            {QStringLiteral("invalid_buffers"), m_invalidBuffers},
            {QStringLiteral("end_markers"), m_endMarkers}};
}

void PcmDiagnosticSummary::reset()
{
    *this = PcmDiagnosticSummary();
}

} // namespace asmr
