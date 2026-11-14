#include "ApplicationBrand.h"
#include "update/ApplicationVersion.h"

#include <QApplication>
#include <QResource>

// Explicitly reference rcc's entry point so resources in the static window-frame
// library are retained by the linker, including in isolated GUI test programs.
static void initializeBrandResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(application_branding);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace asmr {

QString applicationDisplayName()
{
    return QStringLiteral("Arcanblade");
}

QIcon applicationIcon()
{
    initializeBrandResources();

    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/branding/arcanblade-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

QIcon applicationMarkIcon()
{
    initializeBrandResources();
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/branding/arcanblade-mark-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

void configureApplicationBranding(QApplication &application)
{
    // Establish the new identity before constructing QSettings or data services.
    // Legacy data remains in its original namespace; no migration or fallback.
    application.setOrganizationName(QStringLiteral("Arcanblade"));
    application.setApplicationName(QStringLiteral("Arcanblade"));
    application.setApplicationVersion(applicationBuildVersion());
    application.setApplicationDisplayName(applicationDisplayName());
    application.setWindowIcon(applicationIcon());
}

} // namespace asmr
