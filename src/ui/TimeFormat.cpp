#include "TimeFormat.h"

#include <algorithm>

namespace asmr {

QString formatTime(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QChar('0'));
}

} // namespace asmr
