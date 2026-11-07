#pragma once

#include <QString>
#include <QtGlobal>

namespace asmr {

struct SubtitleEntry
{
    // Active over [startMs, endMs); equal endpoints form an empty interval.
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
};

} // namespace asmr
