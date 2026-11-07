#include "ui/MainWindow.h"
#include "ui/ApplicationBrand.h"
#include "ui/InputControlPolicy.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    asmr::configureApplicationBranding(app);
    asmr::InputControlPolicy::install();

    asmr::MainWindow window;
    window.show();
    return app.exec();
}
