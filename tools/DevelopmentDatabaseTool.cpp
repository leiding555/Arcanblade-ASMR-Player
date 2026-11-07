#include "library/LibraryRepository.h"
#include "library/BusinessDatabase.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUuid>
namespace {
QString digest(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout), err(stderr);
    const auto args = app.arguments();
    if (args.size() != 5 || args[1] != QStringLiteral("--backup-and-rebuild") || args[4] != QStringLiteral("--development-only")) {
        err << "Usage: DevelopmentDatabaseTool --backup-and-rebuild <absolute library.sqlite> <absolute backup directory> --development-only\n";
        return 2;
    }
    const QString original = QDir::cleanPath(args[2]), backupRoot = QDir::cleanPath(args[3]);
    if (!QFileInfo(original).isAbsolute() || !QFileInfo(original).isFile()
        || QFileInfo(original).isSymLink() || QFileInfo(original).isJunction()
        || !QFileInfo(backupRoot).isAbsolute() || !QDir().mkpath(backupRoot)) {
        err << "Invalid database/backup location. No reset performed.\n"; return 3;
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString backup = QDir(backupRoot).filePath(QStringLiteral("library-before-v%1-").arg(asmr::businessSchemaVersion) + token + QStringLiteral(".sqlite"));
    QString error;
    {
        auto source = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("development-reset-source"));
        source.setDatabaseName(original); source.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1000"));
        if (!source.open()) error = source.lastError().text();
        if (error.isEmpty()) {
            QSqlQuery query(source);
            if (!query.exec(QStringLiteral("PRAGMA application_id")) || !query.next() || query.value(0).toInt() != 1095978322)
                error = QStringLiteral("Not an owned ASMR library.");
            query.finish();
            if (error.isEmpty() && (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()
                || query.value(0).toInt() < 1 || query.value(0).toInt() > 3))
                error = QStringLiteral("Only an explicitly selected v1-v3 development library can be rebuilt.");
            query.finish();
            if (error.isEmpty() && (!query.exec(QStringLiteral("PRAGMA quick_check")) || !query.next()
                || query.value(0).toString() != QStringLiteral("ok")))
                error = QStringLiteral("Database integrity check failed; original retained.");
            query.finish();
            if (error.isEmpty()) {
                query.prepare(QStringLiteral("VACUUM INTO ?")); query.addBindValue(backup);
                if (!query.exec()) error = query.lastError().text();
            }
        }
        source.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("development-reset-source"));
    if (!error.isEmpty()) { err << error << "\nNo reset performed.\n"; return 4; }
    const QString backupHash = digest(backup);
    if (backupHash.isEmpty()) { err << "Could not verify backup; original retained.\n"; return 5; }
    {
        auto verify = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("development-reset-verify"));
        verify.setDatabaseName(backup);
        if (!verify.open()) error = verify.lastError().text();
        else {
            QSqlQuery check(verify);
            if (!check.exec(QStringLiteral("PRAGMA integrity_check")) || !check.next()
                || check.value(0).toString() != QStringLiteral("ok"))
                error = QStringLiteral("Backup verification failed.");
        }
        verify.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("development-reset-verify"));
    if (!error.isEmpty()) { err << error << "\nOriginal retained.\n"; return 6; }
    // A live writer or an uncheckpointed sidecar must never be separated from its DB.
    if (QFileInfo::exists(original + QStringLiteral("-wal")) || QFileInfo::exists(original + QStringLiteral("-journal"))
        || QFileInfo::exists(original + QStringLiteral("-shm"))) {
        err << "Close all database users before rebuilding. Verified backup: " << backup << "\n"; return 7;
    }
    const QString staged = original + QStringLiteral(".new-v%1-").arg(asmr::businessSchemaVersion) + token;
    {
        asmr::LibraryRepository fresh(staged);
        const auto result = fresh.open();
        if (!result.success) {
            for (const auto &diagnostic : result.diagnostics) err << diagnostic.message << "\n";
            return 8;
        }
    }
    const QString archived = original + QStringLiteral(".archived-v3-") + token;
    if (!QFile::rename(original, archived)) { err << "Could not archive original. Verified backup: " << backup << "\n"; return 9; }
    if (!QFile::rename(staged, original)) {
        const bool restored = QFile::rename(archived, original);
        err << "Could not install fresh database; original restoration: " << (restored ? "succeeded" : "FAILED")
            << ". Backup: " << backup << "; archive: " << archived << "\n"; return 10;
    }
    const QJsonObject record{{"database", original}, {"backup", backup}, {"backup_sha256", backupHash},
        {"archived_original", archived}, {"new_schema", asmr::businessSchemaVersion}, {"media_touched", false}};
    const auto bytes = QJsonDocument(record).toJson();
    QFile audit(QDir(backupRoot).filePath(QStringLiteral("rebuild-") + token + QStringLiteral(".json")));
    if (!audit.open(QIODevice::WriteOnly | QIODevice::NewOnly) || audit.write(bytes) != bytes.size() || !audit.flush()) {
        err << "Database rebuilt; backup retained, but audit file could not be saved.\n"; out << bytes; return 11;
    }
    out << bytes;
    return 0;
}
