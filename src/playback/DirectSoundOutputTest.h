#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>

namespace asmr {

struct DirectSoundTestResult {
    QString stage;
    quint32 status = 0;
};

DirectSoundTestResult testDirectSoundOutput(const QByteArray &endpointId, quintptr window,
                                           const QByteArray &stereoPcm, int sampleRate,
                                           const std::atomic_bool &cancelled);

} // namespace asmr
