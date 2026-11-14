#include "update/ApplicationVersion.h"

#ifndef ARCANBLADE_VERSION
#error ARCANBLADE_VERSION must be supplied by the version target.
#endif

namespace asmr {
QString applicationBuildVersion()
{
    return QStringLiteral(ARCANBLADE_VERSION);
}
} // namespace asmr
