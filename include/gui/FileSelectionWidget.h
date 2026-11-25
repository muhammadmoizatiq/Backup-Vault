
#ifndef FILESELECTIONWIDGET_H
#define FILESELECTIONWIDGET_H

#include <QWidget>
#include <QStringList>

class QListWidget;
class QPushButton;

namespace bv {

class FileSelectionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FileSelectionWidget(QWidget *parent = nullptr);

    QStringList selectedPaths() const;

private slots:
    void addFolder();
    void addFiles();
    void removeSelected();

private:
    void addPath(const QString &path);

    QListWidget *pathList;
    QPushButton *addFolderButton;
    QPushButton *addFilesButton;
    QPushButton *removeButton;

    QStringList paths;
};

}

#endif
