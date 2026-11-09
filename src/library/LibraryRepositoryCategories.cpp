#include "library/LibraryRepository.h"
#include "library/ScanRootStore.h"

#include <QDateTime>
#include <QScopeGuard>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace asmr {
namespace {
LibraryOperationResult failed(LibraryDiagnosticCode code, const QString &message)
{
    return {false, {{code, {}, message}}};
}

LibraryOperationResult sqlFailed(const QSqlQuery &query)
{
    return failed(LibraryDiagnosticCode::DatabaseError, query.lastError().text());
}

bool validName(const QString &name)
{
    if (name.isEmpty()) return false;
    for (const auto character : name)
        if (character.category() == QChar::Other_Control
            || character == QChar::LineSeparator || character == QChar::ParagraphSeparator) return false;
    return true;
}

bool validCoverToken(const QString &token)
{
    static const QRegularExpression expression(QStringLiteral(
        "\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(jpg|png|webp|bmp|gif)\\z"));
    return token.isEmpty() || expression.match(token).hasMatch();
}

QString rootKey(const QString &path)
{
    auto key = ScanRootStore::normalizePath(path);
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

LibraryOperationResult requireWork(QSqlDatabase &database, const LibraryWork &expected)
{
    const auto key = rootKey(expected.rootPath);
    if (expected.id <= 0 || expected.importedAtMs < 0 || key.isEmpty())
        return failed(LibraryDiagnosticCode::InvalidWorkSnapshot, QStringLiteral("分类操作的作品身份无效。"));
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT root_key,imported_at_ms FROM works WHERE id=?"));
    query.addBindValue(expected.id);
    if (!query.exec()) return sqlFailed(query);
    if (!query.next() || query.value(0).toString() != key
        || query.value(1).toLongLong() != expected.importedAtMs)
        return failed(LibraryDiagnosticCode::StaleWorkSnapshot, QStringLiteral("作品已删除或身份已变化，分类操作未保存。"));
    return {true, {}};
}

LibraryOperationResult requireCategory(QSqlDatabase &database, const WorkCategory &expected)
{
    if (expected.id <= 0 || !validName(expected.name) || !validCoverToken(expected.managedCoverFile))
        return failed(LibraryDiagnosticCode::InvalidCategory, QStringLiteral("分类身份无效。"));
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT c.name,COALESCE(a.managed_file,'') FROM categories c "
                                 "LEFT JOIN category_artwork a ON a.category_id=c.id WHERE c.id=?"));
    query.addBindValue(expected.id);
    if (!query.exec()) return sqlFailed(query);
    if (!query.next() || query.value(0).toString() != expected.name
        || query.value(1).toString() != expected.managedCoverFile)
        return failed(LibraryDiagnosticCode::StaleCategory, QStringLiteral("分类已删除或名称、封面已变化，请重新打开菜单。"));
    return {true, {}};
}

LibraryOperationResult requireUniqueName(QSqlDatabase &database, const QString &name, qint64 except = 0)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id FROM categories WHERE name=? AND id<>?"));
    query.addBindValue(name); query.addBindValue(except);
    if (!query.exec()) return sqlFailed(query);
    if (query.next())
        return failed(LibraryDiagnosticCode::InvalidCategory, QStringLiteral("已有同名分类，请使用其他名称。"));
    return {true, {}};
}

LibraryOperationResult insertMembership(QSqlDatabase &database, qint64 categoryId, qint64 workId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO work_categories(category_id,work_id,added_at_ms) VALUES(?,?,?) "
                                 "ON CONFLICT(category_id,work_id) DO NOTHING"));
    query.addBindValue(categoryId); query.addBindValue(workId);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return query.exec() ? LibraryOperationResult{true, {}} : sqlFailed(query);
}
} // namespace

LibraryCategorySnapshotResult LibraryRepository::categorySnapshot()
{
    LibraryCategorySnapshotResult result;
    const auto ready = checkReady();
    if (!ready.success) { result.diagnostics = ready.diagnostics; return result; }
    if (!m_database.transaction()) {
        result.diagnostics = failed(LibraryDiagnosticCode::DatabaseError, m_database.lastError().text()).diagnostics;
        return result;
    }
    auto rollback = qScopeGuard([this] { m_database.rollback(); });
    const auto fail = [&](const LibraryOperationResult &error) {
        LibraryCategorySnapshotResult rejected;
        rejected.diagnostics = error.diagnostics;
        return rejected;
    };
    const auto allWorks = works();
    if (!allWorks.success) return fail(allWorks);
    result.works = allWorks.works;
    {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("SELECT c.id,c.name,COALESCE(a.managed_file,'') FROM categories c "
                                       "LEFT JOIN category_artwork a ON a.category_id=c.id ORDER BY c.id"))) return fail(sqlFailed(query));
        while (query.next()) {
            WorkCategory category{query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString()};
            if (category.id <= 0 || category.name != category.name.trimmed() || !validName(category.name)
                || !validCoverToken(category.managedCoverFile))
                return fail(failed(LibraryDiagnosticCode::InvalidSchema, QStringLiteral("已保存的分类名称、封面或身份无效。")));
            result.categories.append(std::move(category));
        }
        if (query.lastError().isValid()) return fail(sqlFailed(query));
    }
    {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("SELECT category_id,work_id,added_at_ms FROM work_categories "
                                       "ORDER BY category_id,added_at_ms DESC,work_id DESC"))) return fail(sqlFailed(query));
        while (query.next())
            result.memberships.append({query.value(0).toLongLong(), query.value(1).toLongLong(), query.value(2).toLongLong()});
        if (query.lastError().isValid()) return fail(sqlFailed(query));
    }
    if (!m_database.commit())
        return fail(failed(LibraryDiagnosticCode::DatabaseError, m_database.lastError().text()));
    rollback.dismiss();
    result.success = true;
    return result;
}

LibraryCategoryWriteResult LibraryRepository::createCategory(const QString &name,
    const std::optional<LibraryWork> &addWork)
{
    LibraryCategoryWriteResult result;
    const auto trimmed = name.trimmed();
    if (!validName(name) || !validName(trimmed)) {
        result.diagnostics = failed(LibraryDiagnosticCode::InvalidCategory,
            QStringLiteral("分类名称不能为空或包含控制字符。")).diagnostics;
        return result;
    }
    WorkCategory created;
    const auto written = writeSubtitleChange([&]() -> LibraryOperationResult {
        if (addWork) {
            const auto checked = requireWork(m_database, *addWork);
            if (!checked.success) return checked;
        }
        const auto unique = requireUniqueName(m_database, trimmed);
        if (!unique.success) return unique;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO categories(name) VALUES(?)"));
        query.addBindValue(trimmed);
        if (!query.exec()) return sqlFailed(query);
        created = {query.lastInsertId().toLongLong(), trimmed};
        query.finish();
        return addWork ? insertMembership(m_database, created.id, addWork->id) : LibraryOperationResult{true, {}};
    });
    result.success = written.success;
    result.diagnostics = written.diagnostics;
    if (written.success) result.category = std::move(created);
    return result;
}

LibraryOperationResult LibraryRepository::renameCategory(const WorkCategory &expected, const QString &name)
{
    return updateCategory(expected, name);
}

LibraryOperationResult LibraryRepository::updateCategory(const WorkCategory &expected, const QString &name,
    const std::optional<QString> &managedCoverFile)
{
    const auto trimmed = name.trimmed();
    if (!validName(name) || !validName(trimmed))
        return failed(LibraryDiagnosticCode::InvalidCategory, QStringLiteral("分类名称不能为空或包含控制字符。"));
    if (managedCoverFile && !validCoverToken(*managedCoverFile))
        return failed(LibraryDiagnosticCode::InvalidCategory, QStringLiteral("分类封面副本标识无效。"));
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        const auto checked = requireCategory(m_database, expected);
        if (!checked.success) return checked;
        const auto unique = requireUniqueName(m_database, trimmed, expected.id);
        if (!unique.success) return unique;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("UPDATE categories SET name=? WHERE id=?"));
        query.addBindValue(trimmed); query.addBindValue(expected.id);
        if (!query.exec()) return sqlFailed(query);
        query.finish();
        if (!managedCoverFile) return {true, {}};
        if (managedCoverFile->isEmpty()) {
            query.prepare(QStringLiteral("DELETE FROM category_artwork WHERE category_id=?"));
            query.addBindValue(expected.id);
        } else {
            query.prepare(QStringLiteral("INSERT INTO category_artwork(category_id,managed_file) VALUES(?,?) "
                                         "ON CONFLICT(category_id) DO UPDATE SET managed_file=excluded.managed_file"));
            query.addBindValue(expected.id); query.addBindValue(*managedCoverFile);
        }
        return query.exec() ? LibraryOperationResult{true, {}} : sqlFailed(query);
    });
}

LibraryOperationResult LibraryRepository::removeCategory(const WorkCategory &expected)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        const auto checked = requireCategory(m_database, expected);
        if (!checked.success) return checked;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM categories WHERE id=?"));
        query.addBindValue(expected.id);
        return query.exec() ? LibraryOperationResult{true, {}} : sqlFailed(query);
    });
}

LibraryOperationResult LibraryRepository::setWorkCategory(const LibraryWork &expectedWork,
    const WorkCategory &expectedCategory, bool present)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        const auto workChecked = requireWork(m_database, expectedWork);
        if (!workChecked.success) return workChecked;
        const auto categoryChecked = requireCategory(m_database, expectedCategory);
        if (!categoryChecked.success) return categoryChecked;
        if (present) return insertMembership(m_database, expectedCategory.id, expectedWork.id);
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM work_categories WHERE category_id=? AND work_id=?"));
        query.addBindValue(expectedCategory.id); query.addBindValue(expectedWork.id);
        return query.exec() ? LibraryOperationResult{true, {}} : sqlFailed(query);
    });
}
} // namespace asmr
