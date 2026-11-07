#pragma once

#include <QIcon>
#include <QString>

class QApplication;

namespace asmr {

[[nodiscard]] QString applicationDisplayName();
[[nodiscard]] QIcon applicationIcon();
// Transparent white brand mark for the dark in-application surfaces.
[[nodiscard]] QIcon applicationMarkIcon();

// Configure the display brand and persisted QSettings/standard-path identity.
// Call before constructing windows or opening application data. Old identities
// are neither migrated nor used as fallback storage.
void configureApplicationBranding(QApplication &application);

} // namespace asmr
