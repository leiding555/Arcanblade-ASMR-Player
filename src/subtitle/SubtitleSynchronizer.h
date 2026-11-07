#pragma once

#include "subtitle/SubtitleEntry.h"

#include <QList>

namespace asmr {

class SubtitleSynchronizer final
{
public:
    // Replace the stored values without sorting or modifying the caller's list.
    void setEntries(const QList<SubtitleEntry> &entries);
    void clear();

    // Return all entries satisfying startMs <= positionMs < endMs, in input
    // order, including duplicates. Zero-duration and reversed ranges never match.
    // Times are signed milliseconds and are compared without clamping.
    // Each query is independent of previous queries. Returned values can be
    // modified or retained after reloading/clearing this synchronizer.
    [[nodiscard]] QList<SubtitleEntry> activeEntries(qint64 positionMs) const;

private:
    QList<SubtitleEntry> m_entries;
};

} // namespace asmr
