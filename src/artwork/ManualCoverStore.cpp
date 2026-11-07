#include "artwork/ManualCoverStore.h"
#include "artwork/ManagedCoverFile.h"
#include "library/BusinessDatabase.h"
#include "library/LibraryRepository.h"
#include "library/ScanRootStore.h"
#include "library/SqliteWriteTransaction.h"

#include <QDir>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace asmr {
namespace {
LibraryWork work(const ManualCoverIdentity &identity)
{ return {identity.workId, identity.rootPath, {}, identity.importedAtMs}; }
QString rootKey(const QString &root)
{
    auto key = ScanRootStore::normalizePath(root);
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}
bool valid(const ManualCoverIdentity &identity)
{ return identity.workId > 0 && identity.importedAtMs >= 0 && !rootKey(identity.rootPath).isEmpty(); }
ManualCoverStoreResult converted(const LibraryOperationResult &result,
    const QString &relative = {}, const QString &managed = {})
{
    QStringList errors;
    for (const auto &diagnostic : result.diagnostics) errors.append(diagnostic.message);
    return {result.success, result.success ? relative : QString{}, errors.join(u'\n'),
        result.success ? managed : QString{}};
}
ManualCoverStoreResult failure(const QString &message) { return {false, {}, message}; }
}

QString ManualCoverStore::normalizeRelativePath(const QString &input)
{
    if (input.isEmpty() || input.contains(QChar::Null)) return {};
    auto path = input;
    path.replace(u'\\', u'/');
    if (QDir::isAbsolutePath(path) || path.startsWith(u'/') || path.contains(u':')) return {};
    for (const auto &part : path.split(u'/', Qt::KeepEmptyParts)) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) return {};
#ifdef Q_OS_WIN
        if (part.endsWith(u'.') || part.endsWith(u' ')) return {};
#endif
    }
    return path;
}

ManualCoverStoreResult ManualCoverStore::read(
    const ManualCoverSettings &settings, const ManualCoverIdentity &identity)
{
    if (!valid(identity)) return failure(QStringLiteral("手动封面作品标识无效。"));
    LibraryRepository repository(settings.databasePath);
    const auto opened = repository.open();
    if (!opened.success) return converted(opened);
    const auto result = repository.workArtwork(work(identity));
    return converted(result, result.relativePath, result.managedFile);
}
ManualCoverStoreResult ManualCoverStore::write(const ManualCoverSettings &settings,
    const ManualCoverIdentity &identity, const QString &relative)
{
    const auto path = normalizeRelativePath(relative);
    if (!valid(identity) || (!relative.isEmpty() && path.isEmpty()))
        return failure(QStringLiteral("手动封面作品标识或相对路径无效。"));
    LibraryRepository repository(settings.databasePath);
    const auto opened = repository.open();
    return opened.success ? converted(repository.setWorkArtwork(work(identity), path), path) : converted(opened);
}
ManualCoverStoreResult ManualCoverStore::writeManaged(const ManualCoverSettings &settings,
    const ManualCoverIdentity &identity, const QString &token)
{
    if (!valid(identity) || !ManagedCoverFile::validToken(token))
        return failure(QStringLiteral("手动封面作品标识或副本标识无效。"));
    LibraryRepository repository(settings.databasePath);
    const auto opened = repository.open();
    return opened.success ? converted(repository.setWorkArtwork(work(identity), {}, token), {}, token) : converted(opened);
}
ManualCoverStoreResult ManualCoverStore::remove(const ManualCoverSettings &settings, qint64 workId)
{
    if (workId <= 0) return failure(QStringLiteral("手动封面作品标识无效。"));
    BusinessDatabase connection(settings.databasePath);
    QString error;
    if (!connection.open(&error)) return failure(error);
    auto &database = connection.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Remove manual cover record"));
    if (!transaction.begin()) return failure(transaction.errorMessage());
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM work_artwork WHERE work_id=?"));
    query.addBindValue(workId);
    if (!query.exec()) return failure(query.lastError().text());
    query.finish();
    if (!transaction.commit()) return failure(transaction.errorMessage());
    // Business record deletion never removes the original or managed images.
    return {true, {}, {}};
}
ManualCoverStoreResult ManualCoverStore::prune(
    const ManualCoverSettings &settings, const QVector<ManualCoverIdentity> &works)
{
    QMap<qint64, ManualCoverIdentity> identities;
    for (const auto &identity : works) {
        if (!valid(identity) || identities.contains(identity.workId))
            return failure(QStringLiteral("封面清理需要完整且有效的作品列表。"));
        identities.insert(identity.workId, identity);
    }
    BusinessDatabase connection(settings.databasePath);
    QString error;
    if (!connection.open(&error)) return failure(error);
    auto &database = connection.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Prune manual cover records"));
    if (!transaction.begin()) return failure(transaction.errorMessage());
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT id,root_key,imported_at_ms FROM works")))
        return failure(query.lastError().text());
    qsizetype seen = 0;
    while (query.next()) {
        const auto expected = identities.constFind(query.value(0).toLongLong());
        if (expected == identities.cend() || rootKey(expected->rootPath) != query.value(1).toString()
            || expected->importedAtMs != query.value(2).toLongLong())
            return failure(QStringLiteral("作品列表已变化，封面记录未清理。"));
        ++seen;
    }
    query.finish();
    if (seen != identities.size()) return failure(QStringLiteral("作品列表已变化，封面记录未清理。"));
    if (!query.exec(QStringLiteral("DELETE FROM work_artwork WHERE NOT EXISTS(SELECT 1 FROM works w "
        "WHERE w.id=work_artwork.work_id AND w.root_key=work_artwork.root_key "
        "AND w.imported_at_ms=work_artwork.imported_at_ms)")))
        return failure(query.lastError().text());
    query.finish();
    if (!transaction.commit()) return failure(transaction.errorMessage());
    return {true, {}, {}};
}
} // namespace asmr
