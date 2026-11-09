#include "library/LibraryRepository.h"
#include "library/BusinessDatabase.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <filesystem>
#include <system_error>
#include <utility>

namespace asmr {
namespace {

constexpr int schemaVersion = businessSchemaVersion;
constexpr int applicationId = 1095978322; // ASCII ASMR.

const QString worksSchema = QStringLiteral(
    "CREATE TABLE works (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "root_path TEXT NOT NULL, root_key TEXT NOT NULL UNIQUE, "
    "display_name TEXT NOT NULL, imported_at_ms INTEGER NOT NULL "
    "CHECK(imported_at_ms >= 0))");
const QString filesSchema = QStringLiteral(
    "CREATE TABLE work_files (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "work_id INTEGER NOT NULL REFERENCES works(id) ON DELETE CASCADE, "
    "relative_path TEXT NOT NULL, path_key TEXT NOT NULL, "
    "file_name TEXT NOT NULL, extension TEXT NOT NULL, "
    "type INTEGER NOT NULL CHECK(type IN (0,1)), "
    "missing INTEGER NOT NULL CHECK(missing IN (0,1)), "
    "UNIQUE(work_id, path_key))");

const QString bindingsSchema = QStringLiteral(
    "CREATE TABLE subtitle_bindings ("
    "audio_file_id INTEGER NOT NULL CHECK(audio_file_id > 0) "
    "REFERENCES work_files(id) ON DELETE CASCADE, "
    "subtitle_file_id INTEGER NOT NULL CHECK(subtitle_file_id > 0) "
    "REFERENCES work_files(id) ON DELETE CASCADE, "
    "source INTEGER NOT NULL CHECK(source IN (0,1)), "
    "PRIMARY KEY(audio_file_id, subtitle_file_id))");
const QString preferencesSchema = QStringLiteral(
    "CREATE TABLE track_subtitle_preferences ("
    "audio_file_id INTEGER PRIMARY KEY CHECK(audio_file_id > 0) "
    "REFERENCES work_files(id) ON DELETE CASCADE, "
    "subtitle_file_id INTEGER CHECK(subtitle_file_id IS NULL OR subtitle_file_id > 0), "
    "FOREIGN KEY(audio_file_id, subtitle_file_id) "
    "REFERENCES subtitle_bindings(audio_file_id, subtitle_file_id) ON DELETE RESTRICT)");

// A NULL position is a cleared progress tombstone. Its retained revision
// prevents a save based on the state before clear from restoring stale data.
const QString playbackProgressSchema = QStringLiteral(
    "CREATE TABLE track_playback_progress ("
    "audio_file_id INTEGER PRIMARY KEY CHECK(audio_file_id > 0) "
    "REFERENCES work_files(id) ON DELETE CASCADE, "
    "position_ms INTEGER CHECK(position_ms IS NULL OR "
    "(typeof(position_ms) = 'integer' AND position_ms >= 0)), "
    "duration_ms INTEGER CHECK(duration_ms IS NULL OR "
    "(typeof(duration_ms) = 'integer' AND duration_ms >= 0)), "
    "updated_at_ms INTEGER NOT NULL "
    "CHECK(typeof(updated_at_ms) = 'integer' AND updated_at_ms >= 0), "
    "revision INTEGER NOT NULL CHECK(typeof(revision) = 'integer' AND revision > 0), "
    "CHECK(position_ms IS NOT NULL OR duration_ms IS NULL))");

struct SchemaObject {
    QString type;
    QString name;
    QString sql;
};

const QVector<SchemaObject> subtitleSchemaObjects = {
    {QStringLiteral("index"), QStringLiteral("subtitle_bindings_by_subtitle"),
     QStringLiteral("CREATE INDEX subtitle_bindings_by_subtitle "
                    "ON subtitle_bindings(subtitle_file_id, audio_file_id)")},
    {QStringLiteral("trigger"), QStringLiteral("subtitle_bindings_validate_insert"),
     QStringLiteral(
         "CREATE TRIGGER subtitle_bindings_validate_insert BEFORE INSERT ON subtitle_bindings "
         "WHEN NOT EXISTS (SELECT 1 FROM work_files a JOIN work_files s "
         "ON s.id = NEW.subtitle_file_id WHERE a.id = NEW.audio_file_id "
         "AND a.type = 0 AND s.type = 1 AND a.work_id = s.work_id) "
         "BEGIN SELECT RAISE(ABORT, 'Invalid subtitle binding endpoints'); END")},
    {QStringLiteral("trigger"), QStringLiteral("subtitle_bindings_validate_update"),
     QStringLiteral(
         "CREATE TRIGGER subtitle_bindings_validate_update BEFORE UPDATE ON subtitle_bindings "
         "WHEN (OLD.source = 1 AND NEW.source = 0) OR NOT EXISTS "
         "(SELECT 1 FROM work_files a JOIN work_files s ON s.id = NEW.subtitle_file_id "
         "WHERE a.id = NEW.audio_file_id AND a.type = 0 AND s.type = 1 "
         "AND a.work_id = s.work_id) "
         "BEGIN SELECT RAISE(ABORT, 'Invalid subtitle binding or manual source downgrade'); END")},
    {QStringLiteral("trigger"), QStringLiteral("track_subtitle_preferences_validate_insert"),
     QStringLiteral(
         "CREATE TRIGGER track_subtitle_preferences_validate_insert "
         "BEFORE INSERT ON track_subtitle_preferences "
         "WHEN NOT EXISTS (SELECT 1 FROM work_files "
         "WHERE id = NEW.audio_file_id AND type = 0) "
         "BEGIN SELECT RAISE(ABORT, 'Subtitle preference requires an audio file'); END")},
    {QStringLiteral("trigger"), QStringLiteral("track_subtitle_preferences_validate_update"),
     QStringLiteral(
         "CREATE TRIGGER track_subtitle_preferences_validate_update "
         "BEFORE UPDATE ON track_subtitle_preferences "
         "WHEN NOT EXISTS (SELECT 1 FROM work_files "
         "WHERE id = NEW.audio_file_id AND type = 0) "
         "BEGIN SELECT RAISE(ABORT, 'Subtitle preference requires an audio file'); END")},
    {QStringLiteral("trigger"), QStringLiteral("work_files_preserve_subtitle_identity"),
     QStringLiteral(
         "CREATE TRIGGER work_files_preserve_subtitle_identity "
         "BEFORE UPDATE OF work_id, type ON work_files "
         "WHEN (NEW.work_id != OLD.work_id OR NEW.type != OLD.type) "
         "AND (EXISTS (SELECT 1 FROM subtitle_bindings "
         "WHERE audio_file_id = OLD.id OR subtitle_file_id = OLD.id) "
         "OR EXISTS (SELECT 1 FROM track_subtitle_preferences WHERE audio_file_id = OLD.id)) "
         "BEGIN SELECT RAISE(ABORT, 'Cannot change a referenced subtitle binding identity'); END")},
};

const QVector<SchemaObject> playbackProgressSchemaObjects = {
    {QStringLiteral("trigger"), QStringLiteral("track_playback_progress_validate_insert"),
     QStringLiteral(
         "CREATE TRIGGER track_playback_progress_validate_insert "
         "BEFORE INSERT ON track_playback_progress "
         "WHEN NEW.audio_file_id <= 0 OR NOT EXISTS (SELECT 1 FROM work_files "
         "WHERE id = NEW.audio_file_id AND type = 0) "
         "BEGIN SELECT RAISE(ABORT, 'Playback progress requires a positive audio file ID'); END")},
    {QStringLiteral("trigger"), QStringLiteral("track_playback_progress_validate_update"),
     QStringLiteral(
         "CREATE TRIGGER track_playback_progress_validate_update "
         "BEFORE UPDATE ON track_playback_progress "
         "WHEN NEW.audio_file_id <= 0 OR NOT EXISTS (SELECT 1 FROM work_files "
         "WHERE id = NEW.audio_file_id AND type = 0) "
         "BEGIN SELECT RAISE(ABORT, 'Playback progress requires a positive audio file ID'); END")},
    {QStringLiteral("trigger"), QStringLiteral("work_files_preserve_playback_progress_identity"),
     QStringLiteral(
         "CREATE TRIGGER work_files_preserve_playback_progress_identity "
         "BEFORE UPDATE OF work_id, type ON work_files "
         "WHEN (NEW.work_id != OLD.work_id OR NEW.type != 0) "
         "AND EXISTS (SELECT 1 FROM track_playback_progress "
         "WHERE audio_file_id = OLD.id) "
         "BEGIN SELECT RAISE(ABORT, 'Cannot change a playback progress audio file identity'); END")},
};

template<typename Result = LibraryOperationResult>
Result failure(LibraryDiagnosticCode code, const QString &path,
               const QString &message)
{
    Result result;
    result.diagnostics.push_back({code, path, message});
    return result;
}

QString sqlMessage(const QString &operation, const QSqlError &error)
{
    return SqliteWriteTransaction::describeError(operation, error);
}

bool scalar(QSqlDatabase &database, const QString &sql, QVariant &value,
            QString &error)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        error = sqlMessage(sql, query.lastError());
        return false;
    }
    value = query.value(0);
    if (query.next() || query.lastError().isValid()) {
        error = QStringLiteral("Unexpected scalar query result: ") + sql;
        return false;
    }
    return true;
}

QString normalizedSeparators(QString path)
{
    return path.replace(QLatin1Char('\\'), QLatin1Char('/'));
}

bool hasUnsafeComponent(const QString &path, bool relative)
{
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const QString &part = parts[index];
        if (part == QStringLiteral(".")) {
            continue;
        }
        if (part == QStringLiteral("..") || part.endsWith(QLatin1Char(' '))
            || part.endsWith(QLatin1Char('.'))) {
            return true;
        }
        if (part.contains(QLatin1Char(':'))
            && (relative || index != 0 || part.size() != 2
                || !part[0].isLetter() || part[1] != QLatin1Char(':'))) {
            return true;
        }
    }
    return false;
}

bool cleanAbsolutePath(const QString &input, QString &cleaned)
{
    const QString path = normalizedSeparators(input);
    if (path.isEmpty() || path.contains(QChar::Null)
        || path.startsWith(QStringLiteral("//?/"))
        || path.startsWith(QStringLiteral("//./"))
        || !QDir::isAbsolutePath(path) || hasUnsafeComponent(path, false)) {
        return false;
    }
#ifdef Q_OS_WIN
    const bool driveQualified = path.size() >= 3
        && ((path[0] >= QLatin1Char('A') && path[0] <= QLatin1Char('Z'))
            || (path[0] >= QLatin1Char('a') && path[0] <= QLatin1Char('z')))
        && path[1] == QLatin1Char(':') && path[2] == QLatin1Char('/');
    const QStringList uncParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const bool uncQualified = path.startsWith(QStringLiteral("//"))
        && !path.startsWith(QStringLiteral("///")) && uncParts.size() >= 2
        && uncParts[0] != QStringLiteral(".") && uncParts[1] != QStringLiteral(".");
    if (!driveQualified && !uncQualified) {
        return false; // Reject current-drive rooted paths and incomplete UNC.
    }
#endif
    cleaned = QDir::cleanPath(path);
    return !cleaned.startsWith(QStringLiteral(":"));
}

bool cleanRelativePath(const QString &input, QString &cleaned)
{
    const QString path = normalizedSeparators(input);
    if (path.isEmpty() || path.contains(QChar::Null)
        || QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('/'))
        || hasUnsafeComponent(path, true)) {
        return false;
    }
    cleaned = QDir::cleanPath(path);
    return cleaned != QStringLiteral(".") && !cleaned.startsWith(QStringLiteral("../"));
}

std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

// Case folding is deliberately conservative: different spellings sharing a key
// are accepted only when the filesystem can prove both identify the same file.
// In particular, a missing old spelling is not proof of a case-only rename.
bool sameExistingFile(const QString &first, const QString &second)
{
    std::error_code error;
    const bool same = std::filesystem::equivalent(nativePath(first), nativePath(second), error);
    return !error && same;
}

QString keyFor(const QString &path)
{
    return path.toCaseFolded();
}

struct ImportFile {
    QString relativePath;
    QString absolutePath;
    QString pathKey;
    QString fileName;
    QString extension;
    LibraryFileType type;
};

LibraryOperationResult prepareImportFiles(const DirectoryScanResult &scan,
                                         const QString &displayName,
                                         QString &rootPath,
                                         QVector<ImportFile> &incoming, bool allowZeroAudio = false)
{
    if (!scan.isComplete()) {
        return failure(LibraryDiagnosticCode::RejectedScan, scan.rootDirectory,
                       QStringLiteral("Only complete directory scans may update the library."));
    }
    if (!allowZeroAudio && (scan.audioFiles.isEmpty() || scan.status != DirectoryScanStatus::Completed)) {
        return failure(LibraryDiagnosticCode::NoAudioFiles, scan.rootDirectory,
                       QStringLiteral("A complete scan must contain at least one audio file."));
    }
    if (!cleanAbsolutePath(scan.rootDirectory, rootPath) || displayName.contains(QChar::Null)) {
        return failure(LibraryDiagnosticCode::InvalidScanRecord, scan.rootDirectory,
                       QStringLiteral("Invalid work root path or display name."));
    }
    QSet<QString> incomingKeys;
    const auto collect = [&](const QVector<DiscoveredFile> &source, LibraryFileType expectedType)
        -> LibraryOperationResult {
        for (const auto &file : source) {
            QString relative;
            QString absolute;
            if (!cleanRelativePath(file.relativePath, relative)
                || !cleanAbsolutePath(file.absolutePath, absolute)
                || absolute != QDir::cleanPath(rootPath + QLatin1Char('/') + relative)
                || file.type != expectedType || file.fileName != QFileInfo(relative).fileName()
                || file.extension != QFileInfo(relative).suffix().toLower()
                || (expectedType == LibraryFileType::Audio
                        ? file.extension != QStringLiteral("mp3") && file.extension != QStringLiteral("wav")
                        : file.extension != QStringLiteral("srt") && file.extension != QStringLiteral("vtt")
                              && file.extension != QStringLiteral("lrc"))) {
                return failure(LibraryDiagnosticCode::InvalidScanRecord, file.absolutePath,
                               QStringLiteral("File paths, name, extension or type are inconsistent or unsupported."));
            }
            const QString key = keyFor(relative);
            if (incomingKeys.contains(key)) {
                return failure(LibraryDiagnosticCode::PathConflict, file.absolutePath,
                               QStringLiteral("Multiple scan records have the same case-folded relative path."));
            }
            incomingKeys.insert(key);
            incoming.push_back({relative, absolute, key, file.fileName, file.extension, expectedType});
        }
        return {true, {}};
    };
    for (const auto &check : {collect(scan.audioFiles, LibraryFileType::Audio),
                              collect(scan.subtitleFiles, LibraryFileType::Subtitle)}) {
        if (!check.success) {
            LibraryOperationResult result;
            result.diagnostics = check.diagnostics;
            return result;
        }
    }
    return {true, {}};
}

} // namespace

LibraryRepository::LibraryRepository(QString databasePath)
    : m_databasePath(std::move(databasePath)),
      m_connectionName(QStringLiteral("asmr-library-") + QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_ownerThread(QThread::currentThread())
{
}

LibraryRepository::~LibraryRepository()
{
    if (onOwnerThread()) {
        releaseConnection();
    } else {
        // This violates the documented lifetime contract. Never close or remove
        // a SQL connection from the wrong thread; its owner must outlive us.
        qWarning("LibraryRepository must be destroyed on its construction thread");
    }
}

bool LibraryRepository::onOwnerThread() const
{
    return QThread::currentThread() == m_ownerThread;
}

bool LibraryRepository::isOpen() const
{
    return onOwnerThread() && m_database.isValid() && m_database.isOpen();
}

LibraryOperationResult LibraryRepository::checkReady() const
{
    if (!onOwnerThread()) {
        return failure(LibraryDiagnosticCode::WrongThread, {},
                       QStringLiteral("Repository access must occur on its construction thread."));
    }
    if (!isOpen()) {
        return failure(LibraryDiagnosticCode::NotOpen, m_databasePath,
                       QStringLiteral("The library database is not open."));
    }
    return {true, {}};
}

void LibraryRepository::releaseConnection()
{
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

LibraryOperationResult LibraryRepository::close()
{
    if (!onOwnerThread()) {
        return failure(LibraryDiagnosticCode::WrongThread, {},
                       QStringLiteral("Close the repository on its construction thread."));
    }
    releaseConnection();
    return {true, {}};
}

LibraryOperationResult LibraryRepository::open()
{
    if (!onOwnerThread()) {
        return failure(LibraryDiagnosticCode::WrongThread, {},
                       QStringLiteral("Open the repository on its construction thread."));
    }
    if (isOpen()) {
        return {true, {}};
    }
    if (m_databasePath.isEmpty() || m_databasePath.contains(QChar::Null)
        || (m_databasePath.startsWith(QLatin1Char(':'))
            && m_databasePath != QStringLiteral(":memory:"))) {
        return failure(LibraryDiagnosticCode::InvalidDatabasePath, m_databasePath,
                       QStringLiteral("A local database file path is required."));
    }
    // Also releases a closed connection left after a failed transaction.
    releaseConnection();
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath == QStringLiteral(":memory:")
                                   ? m_databasePath
                                   : QFileInfo(m_databasePath).absoluteFilePath());
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_database.open()) {
        const auto result = failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                    sqlMessage(QStringLiteral("Open database"), m_database.lastError()));
        releaseConnection();
        return result;
    }
    QString error;
    {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
            error = sqlMessage(QStringLiteral("Enable foreign keys"), query.lastError());
        }
    }
    QVariant foreignKeys;
    if (!error.isEmpty()
        || !scalar(m_database, QStringLiteral("PRAGMA foreign_keys"), foreignKeys, error)
        || foreignKeys.toInt() != 1) {
        const auto result = failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                    error.isEmpty() ? QStringLiteral("Foreign keys could not be enabled.") : error);
        releaseConnection();
        return result;
    }
    const LibraryOperationResult result = prepareSchema();
    if (!result.success) {
        releaseConnection();
    }
    return result;
}

LibraryOperationResult LibraryRepository::prepareSchema(bool writing)
{
    SqliteWriteTransaction transaction(m_database, QStringLiteral("Schema"), m_writeTransactionOptions);
    if (!(writing ? transaction.begin() : m_database.transaction())) {
        return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                       writing ? transaction.errorMessage()
                               : sqlMessage(QStringLiteral("Begin schema transaction"), m_database.lastError()));
    }
    const auto reject = [this, writing, &transaction](LibraryDiagnosticCode code, const QString &message) {
        auto result = failure(code, m_databasePath, message);
        if (!(writing ? transaction.rollback() : m_database.rollback())) {
            result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                          writing ? transaction.errorMessage()
                                                  : sqlMessage(QStringLiteral("Rollback schema"), m_database.lastError())});
        }
        return result;
    };
    QVariant versionValue;
    QVariant idValue;
    QString error;
    if (!scalar(m_database, QStringLiteral("PRAGMA user_version"), versionValue, error)
        || !scalar(m_database, QStringLiteral("PRAGMA application_id"), idValue, error)) {
        return reject(LibraryDiagnosticCode::DatabaseError, error);
    }
    const int version = versionValue.toInt();
    const int id = idValue.toInt();
    if (version < 0 || version > schemaVersion) {
        return reject(LibraryDiagnosticCode::UnsupportedSchemaVersion,
                      QStringLiteral("Unsupported database schema version: %1.").arg(version));
    }
    if (version > 0 && version < 4) {
        return reject(LibraryDiagnosticCode::UnsupportedSchemaVersion,
            QStringLiteral("旧开发数据库需要先备份再重建（当前格式 v%1）；未迁移或清空原数据。").arg(schemaVersion));
    }
    if (!writing && version != schemaVersion) {
        // Creation and additive v4-v7 upgrades release this read snapshot,
        // acquire the writer, then re-read and validate the entire schema.
        // A concurrent opener may already have completed the same operation.
        if (!m_database.rollback())
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("End schema inspection"), m_database.lastError()));
        return prepareSchema(true);
    }
    QHash<QString, QString> tables;
    QHash<QString, QString> auxiliaryObjects;
    {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("SELECT type, name, sql FROM sqlite_master "
                                       "WHERE name NOT GLOB 'sqlite_*'"))) {
            return reject(LibraryDiagnosticCode::DatabaseError,
                          sqlMessage(QStringLiteral("Read schema"), query.lastError()));
        }
        while (query.next()) {
            const QString type = query.value(0).toString();
            const QString name = query.value(1).toString();
            const QString sql = query.value(2).toString();
            if (type == QStringLiteral("table") || type == QStringLiteral("view")) {
                tables.insert(name, sql);
            } else {
                auxiliaryObjects.insert(type + QLatin1Char(':') + name, sql);
            }
        }
        if (query.lastError().isValid()) {
            return reject(LibraryDiagnosticCode::DatabaseError,
                          sqlMessage(QStringLiteral("Read schema rows"), query.lastError()));
        }
    }
    if (version == 0) {
        if (id != 0 || !tables.isEmpty() || !auxiliaryObjects.isEmpty()) {
            return reject(LibraryDiagnosticCode::InvalidSchema,
                          QStringLiteral("Refusing to initialize a nonempty or foreign database."));
        }
    } else {
        const auto expectedBusinessTables = businessTableSchemas(version);
        if (id != applicationId || tables.size() != 5 + expectedBusinessTables.size()
            || tables.value(QStringLiteral("works")).simplified() != worksSchema.simplified()
            || tables.value(QStringLiteral("work_files")).simplified() != filesSchema.simplified()) {
            return reject(LibraryDiagnosticCode::InvalidSchema,
                          QStringLiteral("Database identity or version %1 schema does not match ASMR Player.").arg(version));
        }
        if (tables.value(QStringLiteral("subtitle_bindings")).simplified() != bindingsSchema.simplified()
            || tables.value(QStringLiteral("track_subtitle_preferences")).simplified() != preferencesSchema.simplified()) {
            return reject(LibraryDiagnosticCode::InvalidSchema,
                          QStringLiteral("Version %1 subtitle tables do not match ASMR Player.").arg(version));
        }
        for (const auto &object : subtitleSchemaObjects) {
            if (auxiliaryObjects.value(object.type + QLatin1Char(':') + object.name).simplified()
                != object.sql.simplified()) {
                return reject(LibraryDiagnosticCode::InvalidSchema,
                              QStringLiteral("Required subtitle schema object does not match: %1.").arg(object.name));
            }
        }
        if (tables.value(QStringLiteral("track_playback_progress")).simplified()
            != playbackProgressSchema.simplified()) {
            return reject(LibraryDiagnosticCode::InvalidSchema,
                          QStringLiteral("Playback progress table does not match ASMR Player."));
        }
        for (const auto &object : playbackProgressSchemaObjects) {
            if (auxiliaryObjects.value(object.type + QLatin1Char(':') + object.name).simplified()
                != object.sql.simplified()) {
                return reject(LibraryDiagnosticCode::InvalidSchema,
                              QStringLiteral("Required playback progress schema object does not match: %1.").arg(object.name));
            }
        }
        for (const auto &table : expectedBusinessTables) {
            if (tables.value(table.name).simplified() != table.sql.simplified())
                return reject(LibraryDiagnosticCode::InvalidSchema,
                    QStringLiteral("Business table does not match: %1.").arg(table.name));
        }
    }
    if (version == 0) {
        QSqlQuery query(m_database);
        for (const QString &sql : {worksSchema, filesSchema, bindingsSchema,
                                   preferencesSchema, playbackProgressSchema}) {
            if (!query.exec(sql)) {
                return reject(LibraryDiagnosticCode::DatabaseError,
                              sqlMessage(QStringLiteral("Create library tables"), query.lastError()));
            }
        }
        for (const auto &objects : {subtitleSchemaObjects, playbackProgressSchemaObjects}) {
            for (const auto &object : objects) {
                if (!query.exec(object.sql)) {
                    return reject(LibraryDiagnosticCode::DatabaseError,
                                  sqlMessage(QStringLiteral("Create library schema guards"), query.lastError()));
                }
            }
        }
        for (const auto &table : businessTableSchemas()) {
            if (!query.exec(table.sql))
                return reject(LibraryDiagnosticCode::DatabaseError, sqlMessage(QStringLiteral("Create business table"), query.lastError()));
        }
        query.prepare(QStringLiteral("INSERT INTO business_settings(setting_key,value) VALUES('library_uuid',?)"));
        query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!query.exec())
            return reject(LibraryDiagnosticCode::DatabaseError, sqlMessage(QStringLiteral("Create library identity"), query.lastError()));
    }
    // Validate all existing records before adding anything. Missing flags and
    // progress tombstones remain valid; external orphan/corrupt rows do not.
    QVariant invalidRows;
    if (!scalar(m_database, QStringLiteral("SELECT COUNT(*) FROM pragma_foreign_key_check"), invalidRows, error)) {
        return reject(LibraryDiagnosticCode::DatabaseError, error);
    }
    if (invalidRows.toLongLong() != 0) {
        return reject(LibraryDiagnosticCode::InvalidSchema,
                      QStringLiteral("The library database contains foreign key violations."));
    }
    if (!scalar(m_database, QStringLiteral(
                    "SELECT COUNT(*) FROM subtitle_bindings b "
                    "JOIN work_files a ON a.id = b.audio_file_id "
                    "JOIN work_files s ON s.id = b.subtitle_file_id "
                    "WHERE a.type != 0 OR s.type != 1 OR a.work_id != s.work_id "
                    "OR b.audio_file_id <= 0 OR b.subtitle_file_id <= 0 OR b.source NOT IN (0,1)"),
                invalidRows, error)) {
        return reject(LibraryDiagnosticCode::DatabaseError, error);
    }
    if (invalidRows.toLongLong() != 0) {
        return reject(LibraryDiagnosticCode::InvalidSchema,
                      QStringLiteral("The library database contains invalid subtitle binding endpoints."));
    }
    if (!scalar(m_database, QStringLiteral(
                    "SELECT COUNT(*) FROM track_subtitle_preferences p "
                    "JOIN work_files a ON a.id = p.audio_file_id "
                    "WHERE a.type != 0 OR p.audio_file_id <= 0 OR p.subtitle_file_id <= 0"),
                invalidRows, error)) {
        return reject(LibraryDiagnosticCode::DatabaseError, error);
    }
    if (invalidRows.toLongLong() != 0) {
        return reject(LibraryDiagnosticCode::InvalidSchema,
                      QStringLiteral("The library database contains invalid subtitle preferences."));
    }
    if (!scalar(m_database, QStringLiteral(
                    "SELECT COUNT(*) FROM track_playback_progress p "
                    "JOIN work_files a ON a.id = p.audio_file_id "
                    "WHERE a.type != 0 OR p.audio_file_id <= 0 "
                    "OR (p.position_ms IS NOT NULL AND "
                    "(typeof(p.position_ms) != 'integer' OR p.position_ms < 0)) "
                    "OR (p.duration_ms IS NOT NULL AND "
                    "(typeof(p.duration_ms) != 'integer' OR p.duration_ms < 0)) "
                    "OR typeof(p.updated_at_ms) != 'integer' OR p.updated_at_ms < 0 "
                    "OR typeof(p.revision) != 'integer' OR p.revision <= 0 "
                    "OR (p.position_ms IS NULL AND p.duration_ms IS NOT NULL)"),
                invalidRows, error)) {
        return reject(LibraryDiagnosticCode::DatabaseError, error);
    }
    if (invalidRows.toLongLong() != 0) {
        return reject(LibraryDiagnosticCode::InvalidSchema,
                      QStringLiteral("The library database contains invalid playback progress records."));
    }
    if (version >= 4 && version < schemaVersion) {
        // Only missing tables are added. No existing row, UUID, stable
        // ID, AUTOINCREMENT watermark, trigger or index is rewritten.
        QSqlQuery query(m_database);
        for (const auto &table : businessTableSchemas()) {
            if (!tables.contains(table.name) && !query.exec(table.sql))
                return reject(LibraryDiagnosticCode::DatabaseError,
                              sqlMessage(QStringLiteral("Add business storage"), query.lastError()));
        }
    }
    if (version != schemaVersion) {
        QSqlQuery query(m_database);
        if ((version == 0 && !query.exec(QStringLiteral("PRAGMA application_id = %1").arg(applicationId)))
            || !query.exec(QStringLiteral("PRAGMA user_version = %1").arg(schemaVersion))) {
            return reject(LibraryDiagnosticCode::DatabaseError,
                          sqlMessage(QStringLiteral("Update library schema version"), query.lastError()));
        }
    }
    if (!(writing ? transaction.commit() : m_database.commit())) {
        return reject(LibraryDiagnosticCode::DatabaseError,
                      writing ? transaction.errorMessage()
                              : sqlMessage(QStringLiteral("Commit schema"), m_database.lastError()));
    }
    return {true, {}};
}

LibraryWorksResult LibraryRepository::works()
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibraryWorksResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    LibraryWorksResult result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id, root_path, display_name, imported_at_ms FROM works ORDER BY id"))) {
        return failure<LibraryWorksResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                           sqlMessage(QStringLiteral("Read works"), query.lastError()));
    }
    while (query.next()) {
        result.works.push_back({query.value(0).toLongLong(), query.value(1).toString(),
                                query.value(2).toString(), query.value(3).toLongLong()});
    }
    if (query.lastError().isValid()) {
        return failure<LibraryWorksResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                           sqlMessage(QStringLiteral("Read work rows"), query.lastError()));
    }
    result.success = true;
    return result;
}

LibraryFilesResult LibraryRepository::files(qint64 workId)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibraryFilesResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    LibraryFilesResult result;
    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral("SELECT id, work_id, relative_path, file_name, extension, type, missing "
                                       "FROM work_files WHERE work_id = ? ORDER BY relative_path COLLATE BINARY, id"))) {
        return failure<LibraryFilesResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                           sqlMessage(QStringLiteral("Prepare file query"), query.lastError()));
    }
    query.addBindValue(workId);
    if (!query.exec()) {
        return failure<LibraryFilesResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                           sqlMessage(QStringLiteral("Read files"), query.lastError()));
    }
    while (query.next()) {
        result.files.push_back({query.value(0).toLongLong(), query.value(1).toLongLong(),
                                query.value(2).toString(), query.value(3).toString(),
                                query.value(4).toString(),
                                query.value(5).toInt() == 0 ? LibraryFileType::Audio : LibraryFileType::Subtitle,
                                query.value(6).toBool()});
    }
    if (query.lastError().isValid()) {
        return failure<LibraryFilesResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                           sqlMessage(QStringLiteral("Read file rows"), query.lastError()));
    }
    result.success = true;
    return result;
}

LibraryOperationResult LibraryRepository::validateScan(const DirectoryScanResult &scan,
                                                      const QString &displayName)
{
    QString rootPath;
    QVector<ImportFile> incoming;
    return prepareImportFiles(scan, displayName, rootPath, incoming);
}

LibraryOperationResult LibraryRepository::updateWorkTitle(
    const LibraryWork &expected, const QString &title,
    const std::function<LibraryOperationResult()> &applyRelated,
    const std::function<LibraryOperationResult()> &rollbackRelated)
{
    const auto ready = checkReady();
    if (!ready.success)
        return ready;
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || title.contains(QChar::Null)
        || title.contains(QLatin1Char('\r')) || title.contains(QLatin1Char('\n'))
        || title.contains(QChar::LineSeparator) || title.contains(QChar::ParagraphSeparator)) {
        return failure(LibraryDiagnosticCode::InvalidWorkTitle, expected.rootPath,
                       QStringLiteral("A work title must be nonblank, single-line text without NUL characters."));
    }
    QString cleanedRoot;
    if (expected.id <= 0 || expected.importedAtMs < 0
        || !cleanAbsolutePath(expected.rootPath, cleanedRoot)
        || expected.displayName.contains(QChar::Null)) {
        return failure(LibraryDiagnosticCode::InvalidWorkSnapshot, expected.rootPath,
                       QStringLiteral("A work title edit requires an unchanged persisted work identity and old title."));
    }

    bool relatedApplied = false;
    auto result = writeSubtitleChange([&]() -> LibraryOperationResult {
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral(
                "UPDATE works SET display_name = ? WHERE id = ? AND root_path = ? "
                "AND imported_at_ms = ? AND display_name = ?"))) {
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("Prepare work title edit"), query.lastError()));
        }
        query.addBindValue(trimmed);
        query.addBindValue(expected.id);
        query.addBindValue(expected.rootPath);
        query.addBindValue(expected.importedAtMs);
        query.addBindValue(expected.displayName);
        if (!query.exec()) {
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("Update work title"), query.lastError()));
        }
        if (query.numRowsAffected() != 1) {
            return failure(LibraryDiagnosticCode::StaleWorkSnapshot, expected.rootPath,
                           QStringLiteral("The work identity or title changed; the edit was not saved."));
        }
        query.finish();
        if (!applyRelated)
            return {true, {}};
        auto related = applyRelated();
        if (!related.success) {
            if (related.diagnostics.isEmpty()) {
                related.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, expected.rootPath,
                    QStringLiteral("Related work settings could not be saved; the title edit was rolled back.")});
            }
            return related;
        }
        relatedApplied = true;
        return related;
    });
    if (result.success || !relatedApplied)
        return result;

    if (!rollbackRelated) {
        result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, expected.rootPath,
            QStringLiteral("The title was not committed, but related work settings were saved without a rollback handler.")});
        return result;
    }
    const auto restored = rollbackRelated();
    if (!restored.success) {
        result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, expected.rootPath,
            QStringLiteral("The title was not committed and restoring related work settings also failed; those settings may differ from the original values.")});
        result.diagnostics += restored.diagnostics;
    }
    return result;
}

LibraryImportResult LibraryRepository::importScan(const DirectoryScanResult &scan,
                                                 const QString &displayName)
{
    return synchronizeScan(scan, displayName, nullptr);
}

LibraryImportResult LibraryRepository::refreshExistingWork(
    const LibraryWorkSnapshotResult &expected, const DirectoryScanResult &scan)
{
    return synchronizeScan(scan, {}, &expected);
}

LibraryImportResult LibraryRepository::synchronizeScan(
    const DirectoryScanResult &scan, const QString &displayName,
    const LibraryWorkSnapshotResult *expected)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibraryImportResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    QString rootPath;
    QVector<ImportFile> incoming;
    const auto validation = prepareImportFiles(scan, displayName, rootPath, incoming, expected != nullptr);
    if (!validation.success) {
        LibraryImportResult result;
        result.diagnostics = validation.diagnostics;
        return result;
    }
    if (expected && (!expected->success || expected->work.id <= 0)) {
        return failure<LibraryImportResult>(LibraryDiagnosticCode::InvalidWorkSnapshot, m_databasePath,
                                            QStringLiteral("Refresh requires a successful complete work snapshot."));
    }
    SqliteWriteTransaction transaction(m_database, QStringLiteral("Import"), m_writeTransactionOptions);
    if (!transaction.begin()) {
        return failure<LibraryImportResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                            transaction.errorMessage());
    }
    bool releaseOnExit = false;
    const auto cleanup = qScopeGuard([&] {
        if (releaseOnExit) {
            releaseConnection();
        }
    });
    const auto reject = [this, &releaseOnExit, &transaction](LibraryDiagnosticCode code, const QString &path, const QString &message) {
        const bool rolledBack = transaction.rollback();
        auto result = failure<LibraryImportResult>(code, path, message);
        if (!rolledBack) {
            result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                          transaction.errorMessage()});
            // The scope guard runs after every local query has been destroyed.
            // Never close/remove a connection while these queries are alive.
            releaseOnExit = true;
        }
        return result;
    };
    const auto sqlReject = [&](const QString &operation, const QSqlError &error) {
        return reject(LibraryDiagnosticCode::DatabaseError, m_databasePath, sqlMessage(operation, error));
    };
    if (expected) {
        const auto current = workSnapshot(expected->work.id);
        if (!current.success) {
            const auto code = !current.diagnostics.isEmpty()
                && current.diagnostics.front().code == LibraryDiagnosticCode::InvalidWorkSnapshot
                    ? LibraryDiagnosticCode::StaleWorkSnapshot : LibraryDiagnosticCode::DatabaseError;
            return reject(code, expected->work.rootPath,
                          QStringLiteral("The expected work is no longer readable; refresh was not applied."));
        }
        if (current.work != expected->work || current.files != expected->files) {
            return reject(LibraryDiagnosticCode::StaleWorkSnapshot, expected->work.rootPath,
                          QStringLiteral("The work changed during scanning; refresh was not applied."));
        }
        if (rootPath != current.work.rootPath) {
            return reject(LibraryDiagnosticCode::PathConflict, rootPath,
                          QStringLiteral("An existing-work refresh must retain the original root path."));
        }
    }
    LibraryImportResult result;
    QString previousRoot;
    QString previousName;
    {
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral("SELECT id, root_path, display_name FROM works WHERE root_key = ?"))) {
            return sqlReject(QStringLiteral("Prepare work lookup"), query.lastError());
        }
        query.addBindValue(keyFor(rootPath));
        if (!query.exec()) {
            return sqlReject(QStringLiteral("Find work"), query.lastError());
        }
        if (query.next()) {
            result.workId = query.value(0).toLongLong();
            previousRoot = query.value(1).toString();
            previousName = query.value(2).toString();
        }
        if (query.next() || query.lastError().isValid()) {
            return sqlReject(QStringLiteral("Read work lookup"), query.lastError());
        }
    }
    if (expected && result.workId != expected->work.id) {
        return reject(LibraryDiagnosticCode::StaleWorkSnapshot, rootPath,
                      QStringLiteral("The existing work path identity changed; refresh was not applied."));
    }
    if (result.workId != 0 && previousRoot != rootPath && !sameExistingFile(previousRoot, rootPath)) {
        return reject(LibraryDiagnosticCode::PathConflict, rootPath,
                      QStringLiteral("The normalized work path matches an existing work, but filesystem identity cannot be confirmed."));
    }
    struct ExistingFile { qint64 id; QString relativePath; LibraryFileType type; };
    QHash<QString, ExistingFile> existing;
    if (result.workId != 0) {
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral("SELECT id, path_key, relative_path, type FROM work_files WHERE work_id = ?"))) {
            return sqlReject(QStringLiteral("Prepare existing files"), query.lastError());
        }
        query.addBindValue(result.workId);
        if (!query.exec()) {
            return sqlReject(QStringLiteral("Read existing files"), query.lastError());
        }
        while (query.next()) {
            existing.insert(query.value(1).toString(),
                            {query.value(0).toLongLong(), query.value(2).toString(),
                             query.value(3).toInt() == 0 ? LibraryFileType::Audio : LibraryFileType::Subtitle});
        }
        if (query.lastError().isValid()) {
            return sqlReject(QStringLiteral("Read existing file rows"), query.lastError());
        }
        for (const auto &file : incoming) {
            const auto found = existing.constFind(file.pathKey);
            if (found != existing.cend()
                && (found->type != file.type
                    || (found->relativePath != file.relativePath
                        && !sameExistingFile(QDir::cleanPath(previousRoot + QLatin1Char('/') + found->relativePath),
                                             file.absolutePath)))) {
                return reject(LibraryDiagnosticCode::PathConflict, file.absolutePath,
                              QStringLiteral("An existing file has the same normalized path, but its identity or type cannot be safely preserved."));
            }
        }
    }
    QString name = displayName.isEmpty() ? previousName : displayName;
    if (name.isEmpty()) {
        name = QFileInfo(rootPath).fileName();
        if (name.isEmpty()) {
            name = rootPath;
        }
    }
    {
        QSqlQuery query(m_database);
        if (result.workId == 0) {
            if (!query.prepare(QStringLiteral("INSERT INTO works(root_path, root_key, display_name, imported_at_ms) "
                                               "VALUES(?, ?, ?, ?)"))) {
                return sqlReject(QStringLiteral("Prepare work insert"), query.lastError());
            }
            query.addBindValue(rootPath);
            query.addBindValue(keyFor(rootPath));
            query.addBindValue(name);
            query.addBindValue(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
            if (!query.exec()) {
                return sqlReject(QStringLiteral("Insert work"), query.lastError());
            }
            result.workId = query.lastInsertId().toLongLong();
            result.created = true;
        } else {
            if (!query.prepare(QStringLiteral("UPDATE works SET root_path = ?, display_name = ? WHERE id = ?"))) {
                return sqlReject(QStringLiteral("Prepare work update"), query.lastError());
            }
            query.addBindValue(rootPath);
            query.addBindValue(name);
            query.addBindValue(result.workId);
            if (!query.exec()) {
                return sqlReject(QStringLiteral("Update work"), query.lastError());
            }
        }
    }
    {
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral("UPDATE work_files SET missing = 1 WHERE work_id = ?"))) {
            return sqlReject(QStringLiteral("Prepare missing markers"), query.lastError());
        }
        query.addBindValue(result.workId);
        if (!query.exec()) {
            return sqlReject(QStringLiteral("Mark old files missing"), query.lastError());
        }
    }
    {
        QSqlQuery insert(m_database);
        QSqlQuery update(m_database);
        if (!insert.prepare(QStringLiteral("INSERT INTO work_files(work_id, relative_path, path_key, file_name, extension, type, missing) "
                                            "VALUES(?, ?, ?, ?, ?, ?, 0)"))) {
            return sqlReject(QStringLiteral("Prepare file insert"), insert.lastError());
        }
        if (!update.prepare(QStringLiteral("UPDATE work_files SET relative_path = ?, file_name = ?, extension = ?, missing = 0 WHERE id = ?"))) {
            return sqlReject(QStringLiteral("Prepare file update"), update.lastError());
        }
        for (const auto &file : incoming) {
            const auto found = existing.constFind(file.pathKey);
            if (found == existing.cend()) {
                insert.bindValue(0, result.workId);
                insert.bindValue(1, file.relativePath);
                insert.bindValue(2, file.pathKey);
                insert.bindValue(3, file.fileName);
                insert.bindValue(4, file.extension);
                insert.bindValue(5, file.type == LibraryFileType::Audio ? 0 : 1);
                if (!insert.exec()) {
                    return sqlReject(QStringLiteral("Insert file"), insert.lastError());
                }
                ++result.insertedFiles;
            } else {
                update.bindValue(0, file.relativePath);
                update.bindValue(1, file.fileName);
                update.bindValue(2, file.extension);
                update.bindValue(3, found->id);
                if (!update.exec()) {
                    return sqlReject(QStringLiteral("Update file"), update.lastError());
                }
                ++result.updatedFiles;
            }
        }
    }
    {
        QSet<QString> presentPaths;
        for (const auto &file : incoming) presentPaths.insert(file.pathKey);
        QVector<LibraryFile> absentSubtitles;
        for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
            const auto &file = it.value();
            if (file.type == LibraryFileType::Subtitle && !presentPaths.contains(it.key()))
                absentSubtitles.append({file.id, result.workId, file.relativePath,
                    QFileInfo(file.relativePath).fileName(), QFileInfo(file.relativePath).suffix().toLower(),
                    LibraryFileType::Subtitle, true});
        }
        const auto pruned = removeConfirmedMissingSubtitleRows(rootPath, absentSubtitles);
        if (!pruned.success) {
            const auto diagnostic = pruned.diagnostics.front();
            return reject(diagnostic.code, diagnostic.path, diagnostic.message);
        }
    }
    {
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral("SELECT COUNT(*) FROM work_files WHERE work_id = ? AND missing = 1"))) {
            return sqlReject(QStringLiteral("Prepare missing count"), query.lastError());
        }
        query.addBindValue(result.workId);
        if (!query.exec() || !query.next()) {
            return sqlReject(QStringLiteral("Count missing files"), query.lastError());
        }
        result.missingFiles = query.value(0).toLongLong();
        if (query.next() || query.lastError().isValid()) {
            return sqlReject(QStringLiteral("Read missing count"), query.lastError());
        }
    }
    if (!transaction.commit()) {
        return reject(LibraryDiagnosticCode::DatabaseError, m_databasePath, transaction.errorMessage());
    }
    result.success = true;
    return result;
}

LibraryImportResult LibraryRepository::removeMissingWork(
    const LibraryWorkSnapshotResult &expected,
    const std::function<LibraryOperationResult()> &verifyAvailability)
{
    return removeExpectedWork(expected, verifyAvailability);
}

LibraryImportResult LibraryRepository::removeWork(
    const LibraryWorkSnapshotResult &expected,
    const std::function<LibraryOperationResult()> &beforeRemoval)
{
    return removeExpectedWork(expected, beforeRemoval);
}

LibraryImportResult LibraryRepository::removeExpectedWork(
    const LibraryWorkSnapshotResult &expected,
    const std::function<LibraryOperationResult()> &beforeRemoval)
{
    LibraryImportResult result;
    const auto written = writeSubtitleChange([&]() -> LibraryOperationResult {
        if (!expected.success || expected.work.id <= 0) {
            return failure(LibraryDiagnosticCode::InvalidWorkSnapshot, m_databasePath,
                           QStringLiteral("Work removal requires a successful complete work snapshot."));
        }
        const auto current = workSnapshot(expected.work.id);
        if (!current.success) {
            const auto code = !current.diagnostics.isEmpty()
                && current.diagnostics.front().code == LibraryDiagnosticCode::InvalidWorkSnapshot
                    ? LibraryDiagnosticCode::StaleWorkSnapshot : LibraryDiagnosticCode::DatabaseError;
            return failure(code, expected.work.rootPath,
                           QStringLiteral("The expected work is no longer readable; removal was not applied."));
        }
        if (current.work != expected.work || current.files != expected.files) {
            return failure(LibraryDiagnosticCode::StaleWorkSnapshot, expected.work.rootPath,
                           QStringLiteral("The work snapshot changed; removal was not applied."));
        }
        // External-deletion callers recheck availability here because a prior
        // check can become stale while BEGIN IMMEDIATE waits. Explicit user
        // removal needs no filesystem check. No callback or write is replayed.
        if (beforeRemoval) {
            const auto verification = beforeRemoval();
            if (!verification.success)
                return verification;
        }
        // A selected subtitle uses ON DELETE RESTRICT on the binding. Clear
        // every preference belonging to this work first, including explicit
        // None rows; deleting works then cascades files, bindings and progress.
        QSqlQuery preferences(m_database);
        if (!preferences.prepare(QStringLiteral(
                "DELETE FROM track_subtitle_preferences WHERE audio_file_id IN "
                "(SELECT id FROM work_files WHERE work_id = ?)"))) {
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("Prepare work preference removal"), preferences.lastError()));
        }
        preferences.addBindValue(expected.work.id);
        if (!preferences.exec()) {
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("Remove work preferences"), preferences.lastError()));
        }
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral("DELETE FROM works WHERE id = ?"))) {
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("Prepare work removal"), query.lastError()));
        }
        query.addBindValue(expected.work.id);
        if (!query.exec()) {
            return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                           sqlMessage(QStringLiteral("Remove work"), query.lastError()));
        }
        if (query.numRowsAffected() != 1) {
            return failure(LibraryDiagnosticCode::StaleWorkSnapshot, expected.work.rootPath,
                           QStringLiteral("The expected work was not removed; the transaction was rolled back."));
        }
        // Do not reset sqlite_sequence: stale asynchronous requests for old
        // work/file IDs must never address a later same-path import.
        result.workId = expected.work.id;
        result.removed = true;
        return {true, {}};
    });
    if (!written.success) {
        LibraryImportResult failed;
        failed.diagnostics = written.diagnostics;
        return failed;
    }
    result.success = true;
    return result;
}
} // namespace asmr
