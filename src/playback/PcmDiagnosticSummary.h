#pragma once

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QJsonObject>
#include <QVector>

namespace asmr {

// Value-only statistics for a single PCM format/window. Callers split windows
// before adding a different format; a mismatched buffer is rejected defensively.
// This object never retains PCM data and is owned by the diagnostics worker.
class PcmDiagnosticSummary final {
public:
    void addBuffer(const QAudioBuffer &buffer);
    [[nodiscard]] QAudioFormat format() const { return m_format; }
    [[nodiscard]] bool hasFormat() const { return m_format.isValid(); }
    [[nodiscard]] QJsonObject snapshot() const;
    void reset();

private:
    struct Channel {
        double peak = 0;
        double sumSquares = 0;
        qint64 finiteSamples = 0;
        qint64 nonFiniteSamples = 0;
    };
    QAudioFormat m_format;
    QVector<Channel> m_channels;
    qint64 m_buffers = 0;
    qint64 m_frames = 0;
    qint64 m_invalidBuffers = 0;
    qint64 m_endMarkers = 0;
};

} // namespace asmr
