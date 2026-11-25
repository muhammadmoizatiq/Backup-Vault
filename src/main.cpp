#include <QApplication>
#include <QLabel>

// Placeholder entry point. Moiz replaces this with gui/MainWindow.
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QLabel label("BackupVault");
    label.resize(320, 120);
    label.setAlignment(Qt::AlignCenter);
    label.show();
    return app.exec();
}
