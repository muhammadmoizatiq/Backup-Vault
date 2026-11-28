
#ifndef VAULTSETTINGS_H
#define VAULTSETTINGS_H

#include <QString>

namespace bv {

class VaultSettings
{
public:
    QString vaultRoot() const;
    bool setVaultRoot(const QString &path);
};

}

#endif
