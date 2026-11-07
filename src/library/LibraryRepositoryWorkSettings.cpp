#include "library/LibraryRepository.h"
#include "library/ScanRootStore.h"

#include <QDir>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace asmr {
namespace {
QString rootKey(const QString &root)
{
    auto key = ScanRootStore::normalizePath(root);
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}
LibraryOperationResult failed(LibraryDiagnosticCode code, const QString &path, const QString &message)
{ return {false, {{code, path, message}}}; }
LibraryOperationResult sqlFailed(const QString &path, const QSqlQuery &query)
{ return failed(LibraryDiagnosticCode::DatabaseError, path, query.lastError().text()); }
bool validArtwork(const QString &relative, const QString &managed)
{
    if (!relative.isEmpty() && !managed.isEmpty()) return false;
    if (!managed.isEmpty()) {
        static const QRegularExpression token(QStringLiteral(
            "\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(jpg|png|webp|bmp|gif)\\z"));
        return token.match(managed).hasMatch();
    }
    if (relative.isEmpty()) return true;
    if (relative.contains(QChar::Null) || relative.contains(u'\\')
        || relative.contains(u':') || QDir::isAbsolutePath(relative)) return false;
    for (const auto &part : relative.split(u'/', Qt::KeepEmptyParts)) {
        if (part.isEmpty() || part == "." || part == "..") return false;
#ifdef Q_OS_WIN
        if (part.endsWith(u'.') || part.endsWith(u' ')) return false;
#endif
    }
    return true;
}
LibraryOperationResult saveArtwork(QSqlDatabase &database, const LibraryWork &identity,
    const QString &relative, const QString &managed)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT root_key, imported_at_ms FROM works WHERE id = ?"));
    query.addBindValue(identity.id);
    if (!query.exec()) return sqlFailed(identity.rootPath, query);
    if (!query.next() || query.value(0).toString() != rootKey(identity.rootPath)
        || query.value(1).toLongLong() != identity.importedAtMs)
        return failed(LibraryDiagnosticCode::StaleWorkSnapshot, identity.rootPath,
            QStringLiteral("作品已删除或身份已变化，封面设置未保存。"));
    query.finish();
    if (relative.isEmpty() && managed.isEmpty()) {
        query.prepare(QStringLiteral("DELETE FROM work_artwork WHERE work_id = ?"));
        query.addBindValue(identity.id);
    } else {
        query.prepare(QStringLiteral("INSERT INTO work_artwork(work_id,root_key,imported_at_ms,relative_path,managed_file) "
            "VALUES(?,?,?,?,?) ON CONFLICT(work_id) DO UPDATE SET root_key=excluded.root_key,"
            "imported_at_ms=excluded.imported_at_ms,relative_path=excluded.relative_path,managed_file=excluded.managed_file"));
        query.addBindValue(identity.id); query.addBindValue(rootKey(identity.rootPath));
        query.addBindValue(identity.importedAtMs); query.addBindValue(relative.isNull() ? QStringLiteral("") : relative);
        query.addBindValue(managed.isNull() ? QStringLiteral("") : managed);
    }
    return query.exec() ? LibraryOperationResult{true, {}} : sqlFailed(identity.rootPath, query);
}
}

LibraryWorkArtworkResult LibraryRepository::workArtwork(const LibraryWork &identity)
{
    LibraryWorkArtworkResult result;
    const auto ready = checkReady();
    if (!ready.success) { result.diagnostics = ready.diagnostics; return result; }
    if (identity.id <= 0 || identity.importedAtMs < 0 || rootKey(identity.rootPath).isEmpty()) {
        result.diagnostics = failed(LibraryDiagnosticCode::InvalidWorkSnapshot, identity.rootPath,
            QStringLiteral("手动封面作品标识无效。")).diagnostics;
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT a.relative_path,a.managed_file FROM work_artwork a JOIN works w "
        "ON w.id=a.work_id AND w.root_key=a.root_key AND w.imported_at_ms=a.imported_at_ms "
        "WHERE a.work_id=? AND a.root_key=? AND a.imported_at_ms=?"));
    query.addBindValue(identity.id); query.addBindValue(rootKey(identity.rootPath));
    query.addBindValue(identity.importedAtMs);
    if (!query.exec()) { result.diagnostics = sqlFailed(m_databasePath, query).diagnostics; return result; }
    if (query.next()) {
        result.relativePath = query.value(0).toString(); result.managedFile = query.value(1).toString();
        if (!validArtwork(result.relativePath, result.managedFile)) {
            result.relativePath.clear(); result.managedFile.clear();
            result.diagnostics = failed(LibraryDiagnosticCode::InvalidSchema, m_databasePath,
                QStringLiteral("已保存的封面选择无效。")).diagnostics;
            return result;
        }
    }
    result.success = true;
    return result;
}

LibraryOperationResult LibraryRepository::setWorkArtwork(const LibraryWork &identity,
    const QString &relative, const QString &managed)
{
    const auto ready = checkReady();
    if (!ready.success) return ready;
    if (identity.id <= 0 || identity.importedAtMs < 0 || rootKey(identity.rootPath).isEmpty()
        || !validArtwork(relative, managed))
        return failed(LibraryDiagnosticCode::InvalidWorkSnapshot, identity.rootPath,
            QStringLiteral("手动封面作品标识或选择无效。"));
    return writeSubtitleChange([&] { return saveArtwork(m_database, identity, relative, managed); });
}

LibraryOperationResult LibraryRepository::updateWorkTitleAndArtwork(const LibraryWork &expected,
    const QString &title, const QString &relative, const QString &managed,
    const std::function<LibraryOperationResult()> &beforeSave)
{
    if (!validArtwork(relative, managed))
        return failed(LibraryDiagnosticCode::InvalidWorkSnapshot, expected.rootPath,
            QStringLiteral("手动封面选择无效。"));
    // updateWorkTitle already owns this connection's transaction and performs
    // the complete identity + old-title compare-and-set before this callback.
    // Compensation is empty because SQL rollback already restores both tables.
    return updateWorkTitle(expected, title, [&] {
        if (beforeSave) {
            const auto check = beforeSave();
            if (!check.success) return check;
        }
        return saveArtwork(m_database, expected, relative, managed);
    }, [] { return LibraryOperationResult{true, {}}; });
}
} // namespace asmr
