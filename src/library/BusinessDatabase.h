#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QVector>
namespace asmr {
struct BusinessTableSchema { QString name; QString sql; };
// v5 adds translations; v6 references; v7 categories; v8 category artwork.
inline constexpr int businessSchemaVersion = 8;
// Passing 4 through 7 selects immutable historical DDL for upgrade validation.
[[nodiscard]] QVector<BusinessTableSchema> businessTableSchemas(int version = businessSchemaVersion);
// Worker-owned scoped connection. Validated v4-v7 upgrades are additive only.
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
