#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QVector>
namespace asmr {
struct BusinessTableSchema { QString name; QString sql; };
// v4 -> v5 adds translation storage; v6 adds local work-reference caches.
inline constexpr int businessSchemaVersion = 6;
// Passing 4 or 5 selects that immutable historical DDL for upgrade validation.
[[nodiscard]] QVector<BusinessTableSchema> businessTableSchemas(int version = businessSchemaVersion);
// Worker-owned scoped connection. Validated v4/v5 upgrades are additive only.
class BusinessDatabase final {
public:
    explicit BusinessDatabase(QString path);
    ~BusinessDatabase();
    BusinessDatabase(const BusinessDatabase &) = delete;
    BusinessDatabase &operator=(const BusinessDatabase &) = delete;
    bool open(QString *error = nullptr);
    QSqlDatabase &database() { return m_database; }
private:
    QString m_path, m_connectionName;
    QSqlDatabase m_database;
};
} // namespace asmr
