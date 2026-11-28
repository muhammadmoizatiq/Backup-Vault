#ifndef FILESELECTIONWIDGET_H
#define FILESELECTIONWIDGET_H

#include <QStringList>
#include <QWidget>

class QListWidget;
class QPushButton;

namespace bv {

class FileSelectionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FileSelectionWidget(QWidget *parent = nullptr);

    QStringList selectedPaths() const;

signals:
    void pathsChanged();

private:
    void addFolder();
    void addFiles();
    void addPath(const QString &path);
    void removeSelected();

    QListWidget *pathList;
    QPushButton *addFolderButton;
    QPushButton *addFilesButton;
    QPushButton *removeButton;

    QStringList paths;
};

}

#endif