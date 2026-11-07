#include "library/ScanRootStore.h"
#include "library/BusinessDatabase.h"
#include "library/SqliteWriteTransaction.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <filesystem>
#include <utility>

namespace asmr {
namespace {
bool overlaps(const QString &left, const QString &right)
{
    const auto prefix = [](const QString &path) { return path.endsWith(u'/') ? path : path + u'/'; };
    return left == right || left.startsWith(prefix(right)) || right.startsWith(prefix(left));
}
ScanRootSnapshot readSnapshot(QSqlDatabase &database)
{
    ScanRootSnapshot result;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT root_path FROM scan_roots ORDER BY ordinal,root_key"))) {
        result.errorMessage = query.lastError().text(); return result;
    }
    while (query.next()) {
        const auto path = ScanRootStore::normalizePath(query.value(0).toString());
        const auto key = path.toCaseFolded();
        if (!path.isEmpty() && std::none_of(result.roots.cbegin(), result.roots.cend(),
            [&](const QString &existing) { return overlaps(key, existing.toCaseFolded()); }))
            result.roots.append(path);
    }
    query.finish();
    if (!query.exec(QStringLiteral("SELECT value FROM business_settings WHERE setting_key='startup_scan_enabled'"))) {
        result.errorMessage = query.lastError().text(); return result;
    }
    if (query.next()) result.startupEnabled = query.value(0).toString() != QStringLiteral("0");
    return result;
}

std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

bool readableDirectory(const std::filesystem::path &path)
{
    std::error_code error;
    const std::filesystem::directory_iterator entries(path, error);
    return !error;
}

bool confirmedMissingRoot(const QString &input)
{
    const auto path = ScanRootStore::normalizePath(input);
    if (path.isEmpty()) return false;
    QString anchor;
    QStringList parts;
#ifdef Q_OS_WIN
    if (path.startsWith(QStringLiteral("//"))) {
        const auto components = path.mid(2).split(u'/', Qt::SkipEmptyParts);
        if (components.size() < 2) return false;
        anchor = QStringLiteral("//") + components[0] + u'/' + components[1];
        parts = components.mid(2);
    } else {
        anchor = path.left(3);
        parts = path.mid(3).split(u'/', Qt::SkipEmptyParts);
    }
#else
    anchor = QStringLiteral("/");
    parts = path.mid(1).split(u'/', Qt::SkipEmptyParts);
#endif
    // An unavailable drive/share itself gives no evidence of deletion.
    if (parts.isEmpty()) return false;
    for (const auto &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.endsWith(u'.') || part.endsWith(u' ')) return false;
#ifdef Q_OS_WIN
        for (const auto character : part)
            if (character.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(character)) return false;
#endif
    }
    const QFileInfo anchorInfo(anchor);
    if (anchorInfo.isSymLink() || anchorInfo.isJunction()) return false;
    std::error_code error;
    auto directory = std::filesystem::canonical(nativePath(anchor), error);
    if (error || !readableDirectory(directory)) return false;
    QString current = anchor;
    for (const auto &part : parts) {
        if (!readableDirectory(directory)) return false;
        current = QDir(current).filePath(part);
        const QFileInfo childInfo(current);
        if (childInfo.isSymLink() || childInfo.isJunction()) return false;
        const auto child = directory / nativePath(part);
        error.clear();
        const auto status = std::filesystem::symlink_status(child, error);
        if (error == std::errc::no_such_file_or_directory
            || (!error && status.type() == std::filesystem::file_type::not_found)) {
            // Recheck both the exact absent component and its real, enumerable
            // parent. A disappearing/retargeted ancestor must not count as absence.
            error.clear();
            const auto parent = std::filesystem::canonical(nativePath(QFileInfo(current).absolutePath()), error);
            if (error || parent != directory || !readableDirectory(parent)) return false;
            error.clear();
            const auto repeated = std::filesystem::symlink_status(child, error);
            if (error != std::errc::no_such_file_or_directory
                && (error || repeated.type() != std::filesystem::file_type::not_found)) return false;
            error.clear();
            const auto checkedParent = std::filesystem::canonical(nativePath(QFileInfo(current).absolutePath()), error);
            return !error && checkedParent == directory && readableDirectory(directory);
        }
        if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) return false;
        directory = child;
    }
    return false;
}

bool dataVersion(QSqlDatabase &database, qint64 &version, QString &error)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA data_version")) || !query.next()) {
        error = query.lastError().text();
        if (error.isEmpty()) error = QStringLiteral("无法读取扫描配置版本。");
        return false;
    }
    version = query.value(0).toLongLong();
    return true;
}
}
ScanRootStore::ScanRootStore(QString databasePath, QSettings *, bool loadImmediately)
    : m_databasePath(std::move(databasePath))
{ if (loadImmediately) reload(); }
ScanRootStore::~ScanRootStore() = default;
bool ScanRootStore::reload()
{
    BusinessDatabase connection(m_databasePath);
    QString error;
    if (!connection.open(&error)) { m_snapshot.errorMessage = error; return false; }
    auto loaded = readSnapshot(connection.database());
    if (!loaded.errorMessage.isEmpty()) { m_snapshot.errorMessage = loaded.errorMessage; return false; }
    m_snapshot = std::move(loaded);
    return true;
}
QString ScanRootStore::normalizePath(const QString &input)
{
    if (input.isEmpty() || input.contains(QChar::Null))
        return {};
    QString path = input;
#ifdef Q_OS_WIN
    // Qt may recognize and strip Windows device prefixes during native path
    // conversion. Inspect the literal spelling before any such conversion.
    path.replace(QChar(0x5c), u'/');
    const bool drive = path.size() >= 3 && path[0].isLetter() && path[1] == u':' && path[2] == u'/';
    const bool unc = path.startsWith(QStringLiteral("//"));
    if (unc) {
        const auto components = path.mid(2).split(u'/', Qt::SkipEmptyParts);
        if (components.size() < 2 || components[0] == QStringLiteral(".")
            || components[0] == QStringLiteral("..") || components[1] == QStringLiteral(".")
            || components[1] == QStringLiteral(".."))
            return {};
        qsizetype depth = 0;
        for (qsizetype index = 2; index < components.size(); ++index) {
            if (components[index] == QStringLiteral("..")) {
                if (depth == 0) return {};
                --depth;
            } else if (components[index] != QStringLiteral(".")) ++depth;
        }
    }
    if ((!drive && !unc) || path.startsWith(QStringLiteral("//?/"))
        || path.startsWith(QStringLiteral("//./")) || path.mid(drive ? 2 : 0).contains(u':')
        || (unc && path.mid(2).split(u'/', Qt::SkipEmptyParts).size() < 2))
        return {};
#else
    if (!QDir::isAbsolutePath(path) || path.startsWith(u':') || path.contains(QStringLiteral("://")))
        return {};
#endif
    const QString cleaned = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    if (unc && (!cleaned.startsWith(QStringLiteral("//"))
        || cleaned.mid(2).split(u'/', Qt::SkipEmptyParts).size() < 2))
        return {};
#endif
    return cleaned;
}
QString ScanRootStore::pathKey(const QString &path) { return normalizePath(path).toCaseFolded(); }

QStringList ScanRootStore::roots() const { return m_snapshot.roots; }
bool ScanRootStore::startupEnabled() const { return m_snapshot.startupEnabled; }
ScanRootChangeResult ScanRootStore::addRoot(const QString &input)
{
    const QString path = normalizePath(input);
    if (path.isEmpty()) return {false, {}, QStringLiteral("请选择明确的绝对目录路径。")};
    BusinessDatabase connection(m_databasePath);
    QString error;
    if (!connection.open(&error)) return {false, path, error};
    auto &database = connection.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Add scan root"));
    if (!transaction.begin()) return {false, path, transaction.errorMessage()};
    auto current = readSnapshot(database);
    if (!current.errorMessage.isEmpty()) return {false, path, current.errorMessage};
    for (const auto &existing : current.roots)
        if (overlaps(path.toCaseFolded(), existing.toCaseFolded()))
            return {false, path, path.toCaseFolded() == existing.toCaseFolded()
                ? QStringLiteral("该扫描目录已经存在。") : QStringLiteral("扫描目录不能互相嵌套：%1").arg(existing)};
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO scan_roots(root_key,root_path,ordinal) "
        "VALUES(?,?,COALESCE((SELECT MAX(ordinal)+1 FROM scan_roots),0))"));
    query.addBindValue(path.toCaseFolded()); query.addBindValue(path);
    if (!query.exec()) return {false, path, query.lastError().text()};
    query.finish();
    if (!transaction.commit()) return {false, path, transaction.errorMessage()};
    current.roots.append(path); m_snapshot = std::move(current);
    return {true, path, {}};
}
ScanRootChangeResult ScanRootStore::removeRoot(const QString &input)
{
    const QString path = normalizePath(input);
    if (path.isEmpty()) return {false, {}, QStringLiteral("扫描目录不存在。")};
    BusinessDatabase connection(m_databasePath);
    QString error;
    if (!connection.open(&error)) return {false, path, error};
    auto &database = connection.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Remove scan root"));
    if (!transaction.begin()) return {false, path, transaction.errorMessage()};
    auto current = readSnapshot(database);
    if (!current.errorMessage.isEmpty()) return {false, path, current.errorMessage};
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM scan_roots WHERE root_key=?"));
    query.addBindValue(path.toCaseFolded());
    if (!query.exec()) return {false, path, query.lastError().text()};
    if (query.numRowsAffected() != 1) return {false, path, QStringLiteral("扫描目录不存在。")};
    query.finish();
    if (!transaction.commit()) return {false, path, transaction.errorMessage()};
    current.roots.removeIf([&](const QString &value) { return value.toCaseFolded() == path.toCaseFolded(); });
    m_snapshot = std::move(current);
    return {true, path, {}};
}

ScanRootPruneResult ScanRootStore::pruneMissingRoots()
{
    BusinessDatabase connection(m_databasePath);
    QString error;
    if (!connection.open(&error)) return {false, {}, error};
    auto &database = connection.database();
    qint64 initialVersion = 0;
    if (!dataVersion(database, initialVersion, error)) return {false, {}, error};
    auto observed = readSnapshot(database);
    if (!observed.errorMessage.isEmpty()) return {false, {}, observed.errorMessage};
    QStringList candidates;
    for (const auto &root : observed.roots)
        if (confirmedMissingRoot(root)) candidates.append(root);
    if (candidates.isEmpty()) {
        m_snapshot = std::move(observed);
        return {true, {}, {}};
    }
    if (m_beforePruneTransactionTestHook) m_beforePruneTransactionTestHook();
    SqliteWriteTransaction transaction(database, QStringLiteral("Prune missing scan roots"));
    if (!transaction.begin()) return {false, {}, transaction.errorMessage()};
    auto current = readSnapshot(database);
    if (!current.errorMessage.isEmpty()) return {false, {}, current.errorMessage};
    qint64 currentVersion = 0;
    if (!dataVersion(database, currentVersion, error)) return {false, {}, error};
    QStringList removed;
    // An external remove/re-add may produce the same root key, path and ordinal.
    // A changed connection-local data_version retires this entire probe instead
    // of applying an obsolete absence observation to a newly registered root.
    if (currentVersion == initialVersion) {
        for (const auto &root : candidates) {
            if (!current.roots.contains(root) || !confirmedMissingRoot(root)) continue;
            QSqlQuery query(database);
            if (!query.prepare(QStringLiteral("DELETE FROM scan_roots WHERE root_key=? AND root_path=?")))
                return {false, {}, query.lastError().text()};
            query.addBindValue(pathKey(root)); query.addBindValue(root);
            if (!query.exec()) return {false, {}, query.lastError().text()};
            if (query.numRowsAffected() == 1) removed.append(root);
        }
    }
    if (!transaction.commit()) return {false, {}, transaction.errorMessage()};
    current.roots.removeIf([&](const QString &root) { return removed.contains(root); });
    m_snapshot = std::move(current);
    return {true, removed, {}};
}

bool ScanRootStore::setStartupEnabled(bool enabled)
{
    BusinessDatabase connection(m_databasePath);
    QString error;
    if (!connection.open(&error)) { m_snapshot.errorMessage = error; return false; }
    auto &database = connection.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Save startup scan setting"));
    if (!transaction.begin()) { m_snapshot.errorMessage = transaction.errorMessage(); return false; }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO business_settings(setting_key,value) VALUES('startup_scan_enabled',?) "
        "ON CONFLICT(setting_key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(enabled ? QStringLiteral("1") : QStringLiteral("0"));
    if (!query.exec()) { m_snapshot.errorMessage = query.lastError().text(); return false; }
    query.finish();
    if (!transaction.commit()) { m_snapshot.errorMessage = transaction.errorMessage(); return false; }
    m_snapshot.startupEnabled = enabled; m_snapshot.errorMessage.clear();
    return true;
}
} // namespace asmr
