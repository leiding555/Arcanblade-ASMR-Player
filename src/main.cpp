#include "ui/MainWindow.h"
#include "ui/ApplicationBrand.h"
#include "ui/InputControlPolicy.h"
#include "update/ApplicationVersion.h"
#include "update/UpdatePackage.h"
#include "update/UpdateInstaller.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    asmr::configureApplicationBranding(app);
    asmr::InputControlPolicy::install();

#ifdef Q_OS_WIN
    const auto programDirectory = QDir::toNativeSeparators(app.applicationDirPath());
    if (asmr::updater::installationInProgress(std::filesystem::path(programDirectory.toStdWString()))) {
        QMessageBox::information(nullptr, QStringLiteral("正在更新"),
            QStringLiteral("Arcanblade 正在安装更新，请稍后重新打开软件。"));
        return 0;
    }
#endif

    asmr::MainWindow window;
    window.show();
    asmr::confirmUpdateStartup(app.arguments(), asmr::applicationBuildVersion());
    // Network checks belong to application startup, so constructing a window
    // alone (including isolated UI tests) never starts an external request.
    QTimer::singleShot(1500, &window, &asmr::MainWindow::startAutomaticUpdateCheck);
    return app.exec();
}
