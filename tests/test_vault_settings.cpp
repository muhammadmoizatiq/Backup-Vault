
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

#include "config/VaultSettings.h"

using namespace bv;

static int failures = 0;

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::cout << "FAIL: " << message << std::endl;
        ++failures;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    app.setOrganizationName("BackupVaultTests");
    app.setApplicationName("VaultSettingsTest");

    // Use a temporary settings location for testing.
    QTemporaryDir settingsDir;

    if (!settingsDir.isValid()) {
        std::cout << "Cannot create temporary directory." << std::endl;
        return 1;
    }

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        settingsDir.path()
    );

    VaultSettings settings;

    // Test 1: Default vault location.
    check(
        !settings.vaultRoot().isEmpty(),
        "Default vault location should not be empty."
    );

    // Test 2: Save a custom vault location.
    QString testPath = QDir(settingsDir.path())
                           .filePath("MyTestVault");

    check(
        settings.setVaultRoot(testPath),
        "Should save the custom vault location."
    );

    // Test 3: Retrieve the saved location using a new object.
    VaultSettings anotherSettings;

    check(
        anotherSettings.vaultRoot() == QDir::cleanPath(testPath),
        "Should retrieve the previously saved location."
    );

    // Test 4: Reject an empty location.
    check(
        !settings.setVaultRoot("   "),
        "Should reject an empty location."
    );

    // Final result.
    if (failures == 0) {
        std::cout << "All VaultSettings tests passed!" << std::endl;
        return 0;
    }

    std::cout << failures << " test(s) failed." << std::endl;
    return 1;
}
