
#include <QApplication>
#include "gui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setOrganizationName("Bro Code");
    app.setApplicationName("BackupVault");

    bv::MainWindow window;
    window.show();

    return app.exec();
}
