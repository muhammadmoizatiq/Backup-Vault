#include <QPointer>
#include <algorithm>
#include "core/BackupEngine.h"
#include "database/DatabaseManager.h"
#include "storage/ChunkStorage.h"
#include "core/ChunkIndex.h"

#include <QScrollArea>
#include <QThread>
#include <QMetaObject>

#include <QLineEdit>
#include <QProgressBar>
#include "gui/MainWindow.h"

#include "gui/FileSelectionWidget.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace bv {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      vaultPathLabel(nullptr),
      chooseVaultButton(nullptr),
      fileSelectionWidget(nullptr),
      snapshotNameInput(nullptr),
      startBackupButton(nullptr),
      backupProgressBar(nullptr),
      backupThread(nullptr)
{
    setupUi();
    setupStyles();
    updateVaultLocation();

    setWindowTitle("BackupVault");
    resize(1150, 820);
    setMinimumSize(900, 520);
}

MainWindow::~MainWindow()
{
    if (backupThread != nullptr) {
        backupThread->wait();
        delete backupThread;
        backupThread = nullptr;
    }
}

void MainWindow::setupUi()
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QHBoxLayout *windowLayout = new QHBoxLayout(centralWidget);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->setSpacing(0);

    // ---------------- Sidebar ----------------

    QFrame *sidebar = new QFrame;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(220);

    QVBoxLayout *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(22, 28, 22, 22);
    sidebarLayout->setSpacing(20);

    QLabel *brandLabel = new QLabel("◆  BackupVault");
    brandLabel->setObjectName("brandLabel");

    QLabel *brandSubtitle = new QLabel("SECURE BACKUP MANAGER");
    brandSubtitle->setObjectName("brandSubtitle");

    sidebarLayout->addWidget(brandLabel);
    sidebarLayout->addWidget(brandSubtitle);
    sidebarLayout->addSpacing(30);

    QLabel *navigationLabel = new QLabel("WORKSPACE");
    navigationLabel->setObjectName("sectionLabel");
    sidebarLayout->addWidget(navigationLabel);

    QLabel *dashboardLabel = new QLabel("▣   Dashboard");
    dashboardLabel->setObjectName("activeNavigation");
    sidebarLayout->addWidget(dashboardLabel);

    QLabel *backupLabel = new QLabel("▤   Backups");
    backupLabel->setObjectName("navigationLabel");
    sidebarLayout->addWidget(backupLabel);

    QLabel *restoreLabel = new QLabel("↻   Restore");
    restoreLabel->setObjectName("navigationLabel");
    sidebarLayout->addWidget(restoreLabel);

    QLabel *settingsLabel = new QLabel("⚙   Settings");
    settingsLabel->setObjectName("navigationLabel");
    sidebarLayout->addWidget(settingsLabel);

    sidebarLayout->addStretch();

    QLabel *versionLabel = new QLabel("BackupVault  •  v0.1");
    versionLabel->setObjectName("versionLabel");
    sidebarLayout->addWidget(versionLabel);

    // ---------------- Main content ----------------

    QWidget *content = new QWidget;
    content->setObjectName("content");

    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(32, 24, 32, 20);   // was 36, 30, 36, 26
contentLayout->setSpacing(16);

    // Header
    QHBoxLayout *headerLayout = new QHBoxLayout;

    QVBoxLayout *headingLayout = new QVBoxLayout;
    headingLayout->setSpacing(5);

    QLabel *pageTitle = new QLabel("Dashboard");
    pageTitle->setObjectName("pageTitle");

    QLabel *pageSubtitle = new QLabel(
        "Manage your storage and prepare your next backup."
    );
    pageSubtitle->setObjectName("pageSubtitle");

    headingLayout->addWidget(pageTitle);
    headingLayout->addWidget(pageSubtitle);

    headerLayout->addLayout(headingLayout);
    headerLayout->addStretch();

    QLabel *statusLabel = new QLabel("●  SYSTEM READY");
    statusLabel->setObjectName("systemStatus");
    headerLayout->addWidget(statusLabel);

    contentLayout->addLayout(headerLayout);

    // Welcome banner
    QFrame *welcomeCard = new QFrame;
    welcomeCard->setObjectName("welcomeCard");

    QVBoxLayout *welcomeLayout = new QVBoxLayout(welcomeCard);
    welcomeLayout->setContentsMargins(24, 16, 24, 16);
    welcomeLayout->setSpacing(8);

    QLabel *welcomeTitle = new QLabel("Your data. Your control.");
    welcomeTitle->setObjectName("welcomeTitle");

    QLabel *welcomeDescription = new QLabel(
        "Securely organize your files and create reliable backups."
    );
    welcomeDescription->setObjectName("welcomeDescription");

    welcomeLayout->addWidget(welcomeTitle);
    welcomeLayout->addWidget(welcomeDescription);

    contentLayout->addWidget(welcomeCard);

    // Vault location card
    QFrame *vaultCard = new QFrame;
    vaultCard->setObjectName("card");

    QVBoxLayout *vaultLayout = new QVBoxLayout(vaultCard);
    vaultLayout->setContentsMargins(22, 16, 22, 16);
    vaultLayout->setSpacing(12);

    QLabel *vaultHeading = new QLabel("Vault storage");
    vaultHeading->setObjectName("cardTitle");

    QLabel *vaultDescription = new QLabel(
        "Choose where BackupVault will store its database and backup chunks."
    );
    vaultDescription->setObjectName("cardDescription");

    QHBoxLayout *vaultPathLayout = new QHBoxLayout;
    vaultPathLayout->setSpacing(12);

    vaultPathLabel = new QLabel;
    vaultPathLabel->setObjectName("vaultPath");
    vaultPathLabel->setWordWrap(true);
    vaultPathLabel->setMinimumHeight(42);

    chooseVaultButton = new QPushButton("Choose location");
    chooseVaultButton->setObjectName("primaryButton");
    chooseVaultButton->setCursor(Qt::PointingHandCursor);

    vaultPathLayout->addWidget(vaultPathLabel, 1);
    vaultPathLayout->addWidget(chooseVaultButton);

    vaultLayout->addWidget(vaultHeading);
    vaultLayout->addWidget(vaultDescription);
    vaultLayout->addLayout(vaultPathLayout);

    contentLayout->addWidget(vaultCard);

    // File selection card
    QFrame *selectionCard = new QFrame;
    selectionCard->setObjectName("card");

    QVBoxLayout *selectionLayout = new QVBoxLayout(selectionCard);
    selectionLayout->setContentsMargins(22, 16, 22, 16);
selectionLayout->setSpacing(10);

    QLabel *selectionHeading = new QLabel("Backup sources");
    selectionHeading->setObjectName("cardTitle");

    QLabel *selectionDescription = new QLabel(
        "Add the files and folders you want to include in your backup."
    );
    selectionDescription->setObjectName("cardDescription");

    fileSelectionWidget = new FileSelectionWidget;

    selectionLayout->addWidget(selectionHeading);
    selectionLayout->addWidget(selectionDescription);
    selectionLayout->addWidget(fileSelectionWidget, 1);

    // Snapshot name input
    snapshotNameInput = new QLineEdit;
    snapshotNameInput->setPlaceholderText("Enter snapshot name...");
    snapshotNameInput->setMinimumHeight(40);

    selectionLayout->addWidget(snapshotNameInput);

    // Start backup button
    startBackupButton = new QPushButton("Start Backup");
    startBackupButton->setObjectName("primaryButton");
    startBackupButton->setMinimumHeight(44);
    startBackupButton->setCursor(Qt::PointingHandCursor);

    selectionLayout->addWidget(startBackupButton);

    // Backup progress bar
    backupProgressBar = new QProgressBar;
    backupProgressBar->setRange(0, 100);
    backupProgressBar->setValue(0);
    backupProgressBar->setTextVisible(true);

    selectionLayout->addWidget(backupProgressBar);

    contentLayout->addWidget(selectionCard, 1);

    // Scroll area: content is never squeezed when the window is short
    QScrollArea *scrollArea = new QScrollArea;
    scrollArea->setObjectName("contentScroll");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidget(content);

    windowLayout->addWidget(sidebar);
    windowLayout->addWidget(scrollArea, 1);

    connect(
        chooseVaultButton,
        &QPushButton::clicked,
        this,
        &MainWindow::chooseVaultFolder
    );

    connect(
        startBackupButton,
        &QPushButton::clicked,
        this,
        &MainWindow::startBackup
    );

    statusBar()->showMessage("Ready");
}

void MainWindow::setupStyles()
{
    setStyleSheet(R"(
        QMainWindow {
            background-color: #F3F6FB;
        }

        QWidget#content {
            background-color: #F3F6FB;
        }

        QScrollArea#contentScroll {
            background-color: #F3F6FB;
            border: none;
        }

        QFrame#sidebar {
            background-color: #17243A;
            border: none;
        }

        QLabel#brandLabel {
            color: #FFFFFF;
            font-size: 21px;
            font-weight: 700;
        }

        QLabel#brandSubtitle {
            color: #91A4BD;
            font-size: 9px;
            font-weight: 600;
            letter-spacing: 1px;
        }

        QLabel#sectionLabel {
            color: #8295AF;
            font-size: 10px;
            font-weight: 700;
        }

        QLabel#activeNavigation {
            background-color: #29466B;
            color: #FFFFFF;
            padding: 12px;
            border-radius: 7px;
            font-size: 13px;
            font-weight: 600;
        }

        QLabel#navigationLabel {
            color: #B6C4D6;
            padding: 11px;
            font-size: 13px;
        }

        QLabel#versionLabel {
            color: #8295AF;
            font-size: 10px;
        }

        QLabel#pageTitle {
            color: #17243A;
            font-size: 27px;
            font-weight: 700;
        }

        QLabel#pageSubtitle {
            color: #718096;
            font-size: 12px;
        }

        QLabel#systemStatus {
            color: #23845B;
            background-color: #E3F5EB;
            padding: 9px 12px;
            border-radius: 12px;
            font-size: 10px;
            font-weight: 700;
        }

        QFrame#welcomeCard {
            background-color: #244B78;
            border: none;
            border-radius: 12px;
        }

        QLabel#welcomeTitle {
            color: #FFFFFF;
            font-size: 21px;
            font-weight: 700;
        }

        QLabel#welcomeDescription {
            color: #D5E4F4;
            font-size: 12px;
        }

        QFrame#card {
            background-color: #FFFFFF;
            border: 1px solid #E4EAF2;
            border-radius: 11px;
        }

        QLabel#cardTitle {
            color: #1D2D44;
            font-size: 16px;
            font-weight: 700;
        }

        QLabel#cardDescription {
            color: #78879A;
            font-size: 11px;
        }

        QLabel#vaultPath {
            background-color: #F3F6FA;
            color: #35465D;
            border: 1px solid #E4EAF2;
            border-radius: 6px;
            padding: 10px;
            font-size: 11px;
        }

        QPushButton#primaryButton {
            background-color: #2869B2;
            color: #FFFFFF;
            border: none;
            border-radius: 6px;
            padding: 11px 17px;
            font-size: 11px;
            font-weight: 600;
        }

        QPushButton#primaryButton:hover {
            background-color: #1F5797;
        }

        QPushButton#primaryButton:pressed {
            background-color: #194879;
        }

        QPushButton#primaryButton:disabled {
            background-color: #A9BDD6;
            color: #EEF3FA;
        }

        QListWidget {
            background-color: #F8FAFD;
            border: 1px solid #E4EAF2;
            border-radius: 7px;
            padding: 7px;
            color: #35465D;
            font-size: 11px;
            outline: none;
        }

        QListWidget::item {
            padding: 9px;
            border-radius: 5px;
        }

        QListWidget::item:selected {
            background-color: #E2EDFA;
            color: #1F5797;
        }

        QPushButton {
            background-color: #EDF2F8;
            color: #34465D;
            border: 1px solid #D5DFEB;
            border-radius: 6px;
            padding: 9px 13px;
            font-size: 12px;
            font-weight: 600;
        }

        QPushButton:hover {
            background-color: #DCE8F5;
            border: 1px solid #B8CCE3;
        }

        QPushButton:pressed {
            background-color: #C9DAED;
        }

        QPushButton:disabled {
            color: #A0ACBB;
        }

        QLineEdit {
            background-color: #FFFFFF;
            border: 1px solid #D5DFEB;
            border-radius: 6px;
            padding: 8px 12px;
            color: #35465D;
            font-size: 12px;
        }

        QLineEdit:focus {
            border: 1px solid #2869B2;
        }

        QProgressBar {
            background-color: #E4EAF2;
            border: none;
            border-radius: 9px;
            min-height: 18px;
            max-height: 18px;
            text-align: center;
            color: #35465D;
            font-size: 10px;
            font-weight: 600;
        }

        QProgressBar::chunk {
            background-color: #7FB0E6;
            border-radius: 9px;
        }

        QStatusBar {
            background-color: #FFFFFF;
            color: #718096;
            border-top: 1px solid #E4EAF2;
            font-size: 10px;
        }
    )");
}

void MainWindow::updateVaultLocation()
{
    const QString path = vaultSettings.vaultRoot();
    vaultPathLabel->setText(path);
}

void MainWindow::chooseVaultFolder()
{
    const QString selectedPath = QFileDialog::getExistingDirectory(
        this,
        "Choose BackupVault Storage Location",
        vaultSettings.vaultRoot(),
        QFileDialog::ShowDirsOnly
    );

    if (selectedPath.isEmpty()) {
        return;
    }

    const QFileInfo directoryInfo(selectedPath);

    if (!directoryInfo.isDir() || !directoryInfo.isWritable()) {
        QMessageBox::warning(
            this,
            "Invalid Vault Location",
            "Please select an existing writable directory."
        );
        return;
    }
    // Ask for confirmation before changing the vault location.
if (selectedPath != vaultSettings.vaultRoot()) {

    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "Change Vault Location?",
        "Changing the vault location does not move your existing backup data.\n\n"
        "Your current database and backup chunks will remain in:\n"
        + vaultSettings.vaultRoot()
        + "\n\nThe new location will be used as a separate vault. "
          "Your existing snapshots will not automatically appear there.\n\n"
          "Do you want to continue?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }
}

    if (!vaultSettings.setVaultRoot(selectedPath)) {
        QMessageBox::critical(
            this,
            "Settings Error",
            "Could not save the vault location."
        );
        return;
    }

    updateVaultLocation();

    statusBar()->showMessage(
        "Vault location updated successfully.",
        5000
    );
}

void MainWindow::startBackup()
{
    // Prevent starting another backup while one is running.
    if (backupThread != nullptr) {
        return;
    }

    // Get selected files and folders.
    const QStringList selectedPaths = fileSelectionWidget->selectedPaths();

    // Get snapshot name.
    const QString snapshotName = snapshotNameInput->text().trimmed();

    // Get vault location.
    const QString vaultRoot = vaultSettings.vaultRoot();

    // Validate selected paths.
    if (selectedPaths.isEmpty()) {
        QMessageBox::warning(
            this,
            "No Backup Sources",
            "Please select at least one file or folder."
        );
        return;
    }

    // Validate snapshot name.
    if (snapshotName.isEmpty()) {
        QMessageBox::warning(
            this,
            "Missing Snapshot Name",
            "Please enter a snapshot name."
        );
        return;
    }

    // Prepare GUI.
    startBackupButton->setEnabled(false);
    backupProgressBar->setValue(0);
    statusBar()->showMessage("Backup started...");

    QPointer<MainWindow> guard(this);

    // Create worker thread.
    QThread *thread = QThread::create(
        [guard, selectedPaths, snapshotName, vaultRoot]()
        {
            // Create database inside the worker thread.
            DatabaseManager database(vaultRoot);

            Status openStatus = database.open();

            if (!openStatus.isOk()) {
                const QString errorMessage = openStatus.message();

                QMetaObject::invokeMethod(
                    guard.data(),
                    [guard, errorMessage]()
                    {
                        if (!guard) return;

                        QMessageBox::critical(
                            guard.data(),
                            "Backup Failed",
                            errorMessage
                        );

                        guard->statusBar()->showMessage("Backup failed.", 5000);
                    },
                    Qt::QueuedConnection
                );

                return;
            }

            // Create backup dependencies inside the worker thread.
            ChunkStorage storage(vaultRoot);
            ChunkIndex index(database);

            BackupEngine engine(
                vaultRoot,
                database,
                storage,
                index
            );

            // Prepare backup request.
            BackupRequest request;
            request.sourcePaths = selectedPaths;
            request.snapshotName = snapshotName;

            // Start backup.
            auto result = engine.createBackup(
                request,
                [guard](const BackupProgress &progress)
                {
                    int percentage = 0;

                    if (progress.totalBytes > 0) {
                        percentage = static_cast<int>(
                            (static_cast<double>(progress.bytesProcessed) /
                             static_cast<double>(progress.totalBytes)) * 100.0
                        );
                    }
                    else if (progress.totalFiles > 0) {
                        percentage = static_cast<int>(
                            (static_cast<double>(progress.filesCompleted) /
                             static_cast<double>(progress.totalFiles)) * 100.0
                        );
                    }

                    percentage = std::clamp(percentage, 0, 100);

                    const QString currentPath = progress.currentPath;

                    if (!guard) return;

                    QMetaObject::invokeMethod(
                        guard.data(),
                        [guard, percentage, currentPath]()
                        {
                            if (!guard) return;

                            guard->backupProgressBar->setValue(percentage);

                            guard->statusBar()->showMessage(
                                currentPath.isEmpty()
                                    ? "Backing up..."
                                    : "Backing up: " + currentPath
                            );
                        },
                        Qt::QueuedConnection
                    );
                }
            );

            // Handle backup failure.
            if (!result.isOk()) {
                const QString errorMessage = result.message();

                QMetaObject::invokeMethod(
                    guard.data(),
                    [guard, errorMessage]()
                    {
                        if (!guard) return;

                        QMessageBox::critical(
                            guard.data(),
                            "Backup Failed",
                            errorMessage
                        );

                        guard->statusBar()->showMessage("Backup failed.", 5000);
                    },
                    Qt::QueuedConnection
                );

                return;
            }

            // Get backup summary.
            const BackupSummary summary = result.value();

            const QString successMessage = QString(
                "Backup completed successfully!\n\n"
                "Files: %1\n"
                "Directories: %2\n"
                "Chunks: %3\n"
                "Snapshot ID: %4"
            )
            .arg(static_cast<qulonglong>(summary.filesBackedUp))
            .arg(static_cast<qulonglong>(summary.directoriesBackedUp))
            .arg(static_cast<qulonglong>(summary.chunksProcessed))
            .arg(static_cast<qlonglong>(summary.snapshotId));

            QMetaObject::invokeMethod(
                guard.data(),
                [guard, successMessage]()
                {
                    if (!guard) return;

                    guard->backupProgressBar->setValue(100);

                    guard->statusBar()->showMessage(
                        "Backup completed successfully.",
                        5000
                    );

                    QMessageBox::information(
                        guard.data(),
                        "Backup Complete",
                        successMessage
                    );
                },
                Qt::QueuedConnection
            );
        }
    );

    backupThread = thread;

    // Handle thread completion.
    connect(
        thread,
        &QThread::finished,
        this,
        [this, thread]()
        {
            if (backupThread == thread) {
                backupThread = nullptr;
            }

            startBackupButton->setEnabled(true);

            thread->deleteLater();
        }
    );

    // Start the worker.
    thread->start();
}

}