#include "subtitle/SubtitleSynchronizer.h"

namespace asmr {

void SubtitleSynchronizer::setEntries(const QList<SubtitleEntry> &entries)
{
    m_entries = entries;
}

void SubtitleSynchronizer::clear()
{
    m_entries.clear();
}

QList<SubtitleEntry> SubtitleSynchronizer::activeEntries(qint64 positionMs) const
{
    QList<SubtitleEntry> active;
    for (const auto &entry : m_entries) {
        if (entry.startMs <= positionMs && positionMs < entry.endMs)
            active.append(entry);
    }
    return active;
}

} // namespace asmr
