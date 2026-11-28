
#include "config/VaultSettings.h"

#include <QSettings>
#include <QStandardPaths>
#include <QDir>

namespace bv {

QString VaultSettings::vaultRoot() const
{
    QSettings settings;

    QString path = settings.value("vault/root").toString();

    if (path.isEmpty()) {
        path = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation
        );

        path = QDir(path).filePath("Vault");
    }

    return QDir::cleanPath(path);
}

bool VaultSettings::setVaultRoot(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        return false;
    }

    QSettings settings;
    settings.setValue("vault/root", QDir::cleanPath(path));
    settings.sync();

    return settings.status() == QSettings::NoError;
}

}
