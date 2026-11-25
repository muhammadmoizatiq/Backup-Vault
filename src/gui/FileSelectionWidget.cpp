
#include "gui/FileSelectionWidget.h"

#include <QFileDialog>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

namespace bv {

FileSelectionWidget::FileSelectionWidget(QWidget *parent)
    : QWidget(parent)
{
    pathList = new QListWidget(this);

    addFolderButton = new QPushButton("Add Folder", this);
    addFilesButton = new QPushButton("Add Files", this);
    removeButton = new QPushButton("Remove Selected", this);

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(addFolderButton);
    buttonLayout->addWidget(addFilesButton);
    buttonLayout->addWidget(removeButton);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(pathList);
    mainLayout->addLayout(buttonLayout);

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
            this, "Already Selected",
            "This file or folder is already selected.");
        return;
    }

    paths.append(path);
    pathList->addItem(path);
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
