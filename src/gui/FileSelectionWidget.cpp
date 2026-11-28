#include "gui/FileSelectionWidget.h"

#include <QAbstractItemModel>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace bv {

FileSelectionWidget::FileSelectionWidget(QWidget *parent)
    : QWidget(parent),
      pathList(new QListWidget(this)),
      addFolderButton(new QPushButton("Add Folder", this)),
      addFilesButton(new QPushButton("Add Files", this)),
      removeButton(new QPushButton("Remove Selected", this))
{
    pathList->setMinimumHeight(130);
    pathList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    pathList->setIconSize(QSize(20, 20));
    pathList->setTextElideMode(Qt::ElideMiddle);
    pathList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    addFolderButton->setObjectName("secondaryButton");
    addFilesButton->setObjectName("secondaryButton");
    removeButton->setObjectName("dangerButton");
    removeButton->setEnabled(false);

    addFolderButton->setMinimumHeight(40);
    addFilesButton->setMinimumHeight(40);
    removeButton->setMinimumHeight(40);

    addFolderButton->setCursor(Qt::PointingHandCursor);
    addFilesButton->setCursor(Qt::PointingHandCursor);
    removeButton->setCursor(Qt::PointingHandCursor);

    QLabel *countLabel = new QLabel("No items added yet", this);
    countLabel->setObjectName("cardDescription");

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(10);

    buttonLayout->addWidget(addFolderButton, 1);
    buttonLayout->addWidget(addFilesButton, 1);
    buttonLayout->addWidget(removeButton, 1);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(10);

    mainLayout->addWidget(pathList, 1);
    mainLayout->addWidget(countLabel);
    mainLayout->addLayout(buttonLayout);

    auto onListChanged = [this, countLabel]() {
        const int n = pathList->count();

        countLabel->setText(
            n == 0 ? QString("No items added yet")
                   : QString("%1 item%2 selected")
                         .arg(n)
                         .arg(n == 1 ? "" : "s"));

        emit pathsChanged();
    };

    connect(pathList->model(), &QAbstractItemModel::rowsInserted,
            this, onListChanged);

    connect(pathList->model(), &QAbstractItemModel::rowsRemoved,
            this, onListChanged);

    connect(pathList, &QListWidget::itemSelectionChanged, this, [this]() {
        removeButton->setEnabled(!pathList->selectedItems().isEmpty());
    });

    connect(addFolderButton, &QPushButton::clicked,
            this, &FileSelectionWidget::addFolder);

    connect(addFilesButton, &QPushButton::clicked,
            this, &FileSelectionWidget::addFiles);

    connect(removeButton, &QPushButton::clicked,
            this, &FileSelectionWidget::removeSelected);
}

void FileSelectionWidget::addFolder()
{
    QString folderPath = QFileDialog::getExistingDirectory(
        this, "Select Folder");

    if (!folderPath.isEmpty()) {
        addPath(folderPath);
    }
}

void FileSelectionWidget::addFiles()
{
    QStringList filePaths = QFileDialog::getOpenFileNames(
        this, "Select Files");

    for (const QString &filePath : filePaths) {
        addPath(filePath);
    }
}

void FileSelectionWidget::addPath(const QString &path)
{
    if (paths.contains(path)) {
        QMessageBox::information(
            this,
            "Already Selected",
            "This file or folder is already selected.");
        return;
    }

    paths.append(path);

    static QFileIconProvider iconProvider;

    auto *item = new QListWidgetItem(
        iconProvider.icon(QFileInfo(path)), path);

    item->setToolTip(path);
    pathList->addItem(item);
}

void FileSelectionWidget::removeSelected()
{
    int row = pathList->currentRow();

    if (row < 0) {
        return;
    }

    delete pathList->takeItem(row);
    paths.removeAt(row);
}

QStringList FileSelectionWidget::selectedPaths() const
{
    return paths;
}

}
