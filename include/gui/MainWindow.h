
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "config/VaultSettings.h"

class QLabel;
class QPushButton;
class QLineEdit;
class QProgressBar;
class QThread;

namespace bv {

class FileSelectionWidget;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void startBackup();
    void chooseVaultFolder();

    void setupUi();
    void setupStyles();
    void updateVaultLocation();

    VaultSettings vaultSettings;

    QLabel *vaultPathLabel;
    QPushButton *chooseVaultButton;

    // Backup integration
    FileSelectionWidget *fileSelectionWidget;
    QLineEdit *snapshotNameInput;
    QPushButton *startBackupButton;
    QProgressBar *backupProgressBar;

    // Background backup thread
    QThread *backupThread;
};

}

#endif
