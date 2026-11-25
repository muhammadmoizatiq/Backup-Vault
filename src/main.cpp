#include <QApplication>
#include "gui/FileSelectionWidget.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    bv::FileSelectionWidget widget;
    widget.resize(700, 400);
    widget.setWindowTitle("BackupVault - File Selection");
    widget.show();

    return app.exec();
}