#pragma once
#include "MySqlRepository.hpp"
class DatabaseSettingsStore {
public:
    static QString defaultPath();
    static DatabaseSettings load(const QString& path = QString());
    static QString save(const DatabaseSettings&, const QString& path = QString());
    static QString validate(const DatabaseSettings&);
};
