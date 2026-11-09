#include "library/BusinessDatabase.h"
#include "library/LibraryRepository.h"
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <utility>
namespace asmr {
QVector<BusinessTableSchema> businessTableSchemas(int version)
{
    QVector<BusinessTableSchema> tables{
        {QStringLiteral("scan_roots"), QStringLiteral("CREATE TABLE scan_roots (root_key TEXT PRIMARY KEY, root_path TEXT NOT NULL, ordinal INTEGER NOT NULL UNIQUE CHECK(ordinal >= 0))")},
        {QStringLiteral("business_settings"), QStringLiteral("CREATE TABLE business_settings (setting_key TEXT PRIMARY KEY, value TEXT NOT NULL)")},
        {QStringLiteral("work_artwork"), QStringLiteral("CREATE TABLE work_artwork (work_id INTEGER PRIMARY KEY REFERENCES works(id) ON DELETE CASCADE, root_key TEXT NOT NULL, imported_at_ms INTEGER NOT NULL CHECK(imported_at_ms >= 0), relative_path TEXT NOT NULL DEFAULT '', managed_file TEXT NOT NULL DEFAULT '', CHECK((length(relative_path) > 0 AND managed_file = '') OR (relative_path = '' AND length(managed_file) > 0)))")},
        {QStringLiteral("ai_tasks"), QStringLiteral("CREATE TABLE ai_tasks (task_id TEXT PRIMARY KEY CHECK(length(task_id) > 0), mode TEXT NOT NULL CHECK(mode IN ('offline-direct','offline-asr-ja','cloud-translation')), queue_order INTEGER NOT NULL CHECK(queue_order >= 0), state TEXT NOT NULL CHECK(state IN ('pending','running','interrupted','succeeded','failed','cancelled')), data_json TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)")},
        {QStringLiteral("ai_task_attempts"), QStringLiteral("CREATE TABLE ai_task_attempts (attempt_id TEXT PRIMARY KEY CHECK(length(attempt_id) > 0), task_id TEXT NOT NULL REFERENCES ai_tasks(task_id) ON DELETE CASCADE, state TEXT NOT NULL CHECK(state IN ('running','interrupted','succeeded','failed','cancelled')), parameters_json TEXT NOT NULL, result_json TEXT NOT NULL, started_at TEXT NOT NULL, finished_at TEXT NOT NULL)")}
    };
    if (version >= 5) {
        tables.append({QStringLiteral("translation_checkpoints"), QStringLiteral("CREATE TABLE translation_checkpoints (fingerprint TEXT PRIMARY KEY, manifest_json TEXT NOT NULL, state TEXT NOT NULL CHECK(state IN ('pending','running','interrupted','translated','publishing','published','failed','cancelled')), result_path TEXT NOT NULL DEFAULT '', result_sha256 TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL)")});
        tables.append({QStringLiteral("translation_batches"), QStringLiteral("CREATE TABLE translation_batches (fingerprint TEXT NOT NULL REFERENCES translation_checkpoints(fingerprint) ON DELETE CASCADE, batch_index INTEGER NOT NULL CHECK(batch_index >= 0), first_id INTEGER NOT NULL CHECK(first_id > 0), entry_count INTEGER NOT NULL CHECK(entry_count > 0 AND entry_count <= 40), state TEXT NOT NULL CHECK(state IN ('pending','running','interrupted','completed','failed','cancelled')), items_json TEXT NOT NULL DEFAULT '', items_sha256 TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL, PRIMARY KEY(fingerprint,batch_index))")});
    }
    if (version >= 6) {
        tables.append({QStringLiteral("work_reference_scans"), QStringLiteral("CREATE TABLE work_reference_scans (work_id INTEGER PRIMARY KEY REFERENCES works(id) ON DELETE CASCADE, root_key TEXT NOT NULL, imported_at_ms INTEGER NOT NULL CHECK(imported_at_ms >= 0), synchronized_at TEXT NOT NULL)")});
        tables.append({QStringLiteral("work_references"), QStringLiteral("CREATE TABLE work_references (id INTEGER PRIMARY KEY AUTOINCREMENT, work_id INTEGER NOT NULL REFERENCES works(id) ON DELETE CASCADE, relative_path TEXT NOT NULL, path_key TEXT NOT NULL, format TEXT NOT NULL, source TEXT NOT NULL CHECK(source IN ('file','external-subtitle','generated-asr')), audio_relative_path TEXT NOT NULL DEFAULT '', completed_at TEXT NOT NULL DEFAULT '', source_hash TEXT NOT NULL DEFAULT '', encoding TEXT NOT NULL DEFAULT '', encoding_inferred INTEGER NOT NULL CHECK(encoding_inferred IN (0,1)), extractor_version TEXT NOT NULL DEFAULT '', content_version TEXT NOT NULL DEFAULT '', content_json TEXT NOT NULL DEFAULT '{}', state TEXT NOT NULL CHECK(state IN ('ready','no-text','failed','missing')), error TEXT NOT NULL DEFAULT '', warning TEXT NOT NULL DEFAULT '', missing INTEGER NOT NULL CHECK(missing IN (0,1)), UNIQUE(work_id,path_key))")});
    }
    if (version >= 7) {
        tables.append({QStringLiteral("categories"), QStringLiteral("CREATE TABLE categories (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE CHECK(length(name) > 0))")});
        tables.append({QStringLiteral("work_categories"), QStringLiteral("CREATE TABLE work_categories (category_id INTEGER NOT NULL REFERENCES categories(id) ON DELETE CASCADE, work_id INTEGER NOT NULL REFERENCES works(id) ON DELETE CASCADE, added_at_ms INTEGER NOT NULL CHECK(typeof(added_at_ms) = 'integer' AND added_at_ms >= 0), PRIMARY KEY(category_id,work_id))")});
    }
    if (version >= 8) {
        tables.append({QStringLiteral("category_artwork"), QStringLiteral("CREATE TABLE category_artwork (category_id INTEGER PRIMARY KEY REFERENCES categories(id) ON DELETE CASCADE, managed_file TEXT NOT NULL CHECK(length(managed_file) > 0))")});
    }
    return tables;
}
BusinessDatabase::BusinessDatabase(QString path)
    : m_path(std::move(path)), m_connectionName(QStringLiteral("business-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
BusinessDatabase::~BusinessDatabase()
{
    if (m_database.isValid()) { m_database.close(); m_database = {}; QSqlDatabase::removeDatabase(m_connectionName); }
}
bool BusinessDatabase::open(QString *error)
{
    if (error) error->clear();
    if (m_database.isOpen()) return true;
    if (m_path.isEmpty() || (!QFileInfo(m_path).isAbsolute() && m_path != QStringLiteral(":memory:"))) {
        if (error) *error = QStringLiteral("Business storage requires an absolute database path.");
        return false;
    }
    if (m_path != QStringLiteral(":memory:") && !QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the business database directory.");
        return false;
    }
    // Repository owns authoritative schema and integrity validation.
    if (m_path == QStringLiteral(":memory:")) {
        if (error) *error = QStringLiteral("Business stores require a file-backed database.");
        return false;
    }
    {
        LibraryRepository bootstrap(m_path);
        const auto opened = bootstrap.open();
        if (!opened.success) {
            QStringList details;
            for (const auto &diagnostic : opened.diagnostics) details.append(diagnostic.message);
            if (error) *error = details.join(QLatin1Char('\n'));
            return false;
        }
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_path);
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_database.open()) { if (error) *error = m_database.lastError().text(); return false; }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("PRAGMA foreign_keys=ON"))) {
        if (error) *error = query.lastError().text();
        m_database.close();
        return false;
    }
    return true;
}
} // namespace asmr
