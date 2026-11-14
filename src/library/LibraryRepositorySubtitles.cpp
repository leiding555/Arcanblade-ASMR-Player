#include "library/LibraryRepository.h"
#include "library/SqliteWriteTransaction.h"

#include <algorithm>
#include <filesystem>

#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <initializer_list>

namespace asmr {
namespace {

std::filesystem::path nativeSubtitlePath(const QString &path)
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

template<typename Result = LibraryOperationResult>
Result failure(LibraryDiagnosticCode code, const QString &path, const QString &message)
{
    Result result;
    result.diagnostics.push_back({code, path, message});
    return result;
}

QString sqlMessage(const QString &operation, const QSqlError &error)
{
    return SqliteWriteTransaction::describeError(operation, error);
}

struct BindingFile {
    qint64 workId = 0;
    bool missing = false;
};

LibraryOperationResult findBindingFile(QSqlDatabase &database, const QString &databasePath,
                                       qint64 fileId, LibraryFileType expectedType,
                                       BindingFile &file)
{
    if (fileId <= 0) {
        return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, databasePath,
                       QStringLiteral("Subtitle operations require positive stable file IDs."));
    }
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral("SELECT work_id, type, missing FROM work_files WHERE id = ?"))) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Prepare subtitle endpoint lookup"), query.lastError()));
    }
    query.addBindValue(fileId);
    if (!query.exec()) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Find subtitle endpoint"), query.lastError()));
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                           sqlMessage(QStringLiteral("Read subtitle endpoint"), query.lastError()));
        }
        return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, databasePath,
                       QStringLiteral("File ID %1 does not exist in the library.").arg(fileId));
    }
    const int type = query.value(1).toInt();
    file.workId = query.value(0).toLongLong();
    file.missing = query.value(2).toBool();
    if (query.next() || query.lastError().isValid()) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Read subtitle endpoint rows"), query.lastError()));
    }
    const int expected = expectedType == LibraryFileType::Audio ? 0 : 1;
    if (type != expected) {
        return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, databasePath,
                       QStringLiteral("File ID %1 has the wrong type for this subtitle operation.").arg(fileId));
    }
    return {true, {}};
}

LibraryOperationResult validateBindingPair(QSqlDatabase &database, const QString &databasePath,
                                          qint64 audioFileId, qint64 subtitleFileId,
                                          bool requirePresent = false)
{
    BindingFile audio;
    const auto audioResult = findBindingFile(database, databasePath, audioFileId,
                                            LibraryFileType::Audio, audio);
    if (!audioResult.success)
        return audioResult;
    BindingFile subtitle;
    const auto subtitleResult = findBindingFile(database, databasePath, subtitleFileId,
                                               LibraryFileType::Subtitle, subtitle);
    if (!subtitleResult.success)
        return subtitleResult;
    if (audio.workId != subtitle.workId) {
        return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, databasePath,
                       QStringLiteral("An audio file and its subtitle must belong to the same work."));
    }
    if (requirePresent && (audio.missing || subtitle.missing)) {
        return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, databasePath,
                       QStringLiteral("A new subtitle confirmation requires audio and subtitle records marked present by the current scan."));
    }
    return {true, {}};
}

LibraryOperationResult executeChange(QSqlDatabase &database, const QString &databasePath,
                                     const QString &operation, const QString &statement,
                                     std::initializer_list<QVariant> values)
{
    QSqlQuery query(database);
    if (!query.prepare(statement)) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Prepare ") + operation, query.lastError()));
    }
    for (const auto &value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(operation, query.lastError()));
    }
    return {true, {}};
}

LibraryOperationResult requireBinding(QSqlDatabase &database, const QString &databasePath,
                                      qint64 audioFileId, qint64 subtitleFileId)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral("SELECT 1 FROM subtitle_bindings "
                                      "WHERE audio_file_id = ? AND subtitle_file_id = ?"))) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Prepare bound subtitle lookup"), query.lastError()));
    }
    query.addBindValue(audioFileId);
    query.addBindValue(subtitleFileId);
    if (!query.exec()) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Find bound subtitle"), query.lastError()));
    }
    const bool exists = query.next();
    if ((exists && query.next()) || query.lastError().isValid()) {
        return failure(LibraryDiagnosticCode::DatabaseError, databasePath,
                       sqlMessage(QStringLiteral("Read bound subtitle"), query.lastError()));
    }
    if (!exists) {
        return failure(LibraryDiagnosticCode::SubtitleNotBound, databasePath,
                       QStringLiteral("A preferred subtitle must already be bound to this audio file."));
    }
    return {true, {}};
}

SubtitleAvailability availability(bool audioMissing, bool subtitleMissing)
{
    if (audioMissing)
        return subtitleMissing ? SubtitleAvailability::BothMissing : SubtitleAvailability::AudioMissing;
    return subtitleMissing ? SubtitleAvailability::SubtitleMissing : SubtitleAvailability::Available;
}

} // namespace

bool LibraryRepository::confirmedMissingSubtitle(const QString &rootPath, const LibraryFile &file)
{
    if (file.id <= 0 || file.workId <= 0 || file.type != LibraryFileType::Subtitle)
        return false;
    QString root = QDir::fromNativeSeparators(rootPath);
    const QString relative = QDir::fromNativeSeparators(file.relativePath);
    if (!QDir::isAbsolutePath(root) || root.contains(QChar::Null)
        || root.startsWith(QStringLiteral("//?/")) || root.startsWith(QStringLiteral("//./"))
        || relative.isEmpty() || relative.contains(QChar::Null) || QDir::isAbsolutePath(relative))
        return false;
    const QFileInfo rootInfo(root);
    if (rootInfo.isSymLink() || rootInfo.isJunction()) return false;
    const auto parts = relative.split(QLatin1Char('/'));
    for (const auto &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return false;
        for (const auto character : part)
            if (character.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(character)) return false;
    }

    std::error_code error;
    const auto canonicalRoot = std::filesystem::canonical(nativeSubtitlePath(root), error);
    if (error || !readableDirectory(canonicalRoot)) return false;
    auto directory = canonicalRoot;
    QString path = root;
    for (qsizetype index = 0; index < parts.size(); ++index) {
        if (!readableDirectory(directory)) return false;
        path = QDir(path).filePath(parts.at(index));
        const auto child = directory / nativeSubtitlePath(parts.at(index));
        error.clear();
        const auto status = std::filesystem::symlink_status(child, error);
        if (error == std::errc::no_such_file_or_directory
            || (!error && status.type() == std::filesystem::file_type::not_found)) {
            error.clear();
            const auto currentRoot = std::filesystem::canonical(nativeSubtitlePath(root), error);
            return !error && currentRoot == canonicalRoot && readableDirectory(canonicalRoot)
                && readableDirectory(directory);
        }
        if (error || std::filesystem::is_symlink(status) || QFileInfo(path).isJunction()) return false;
        if (index + 1 == parts.size() || !std::filesystem::is_directory(status)) return false;
        directory = child;
    }
    return false;
}

LibraryOperationResult LibraryRepository::removeConfirmedMissingSubtitleRows(
    const QString &rootPath, const QVector<LibraryFile> &files)
{
    for (const auto &file : files) {
        if (!confirmedMissingSubtitle(rootPath, file)) continue;
        const auto cleared = executeChange(m_database, m_databasePath,
            QStringLiteral("Clear removed subtitle defaults"),
            QStringLiteral("DELETE FROM track_subtitle_preferences WHERE subtitle_file_id = ?"), {file.id});
        if (!cleared.success) return cleared;
        const auto removed = executeChange(m_database, m_databasePath,
            QStringLiteral("Remove confirmed missing subtitle"),
            QStringLiteral("DELETE FROM work_files WHERE id = ? AND work_id = ? AND type = 1"), {file.id, file.workId});
        if (!removed.success) return removed;
    }
    return {true, {}};
}

LibraryOperationResult LibraryRepository::pruneMissingSubtitles(const LibraryWorkSnapshotResult &expected)
{
    const auto ready = checkReady();
    if (!ready.success) return ready;
    if (!expected.success || expected.work.id <= 0)
        return failure(LibraryDiagnosticCode::InvalidWorkSnapshot, m_databasePath,
            QStringLiteral("Subtitle cleanup requires a successful work snapshot."));
    QVector<LibraryFile> missing;
    for (const auto &file : expected.files)
        if (file.workId == expected.work.id && confirmedMissingSubtitle(expected.work.rootPath, file)) missing.append(file);
    if (missing.isEmpty()) return {true, {}};
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        const auto current = workSnapshot(expected.work.id);
        if (!current.success) return {false, current.diagnostics};
        if (current.work.rootPath != expected.work.rootPath || current.work.importedAtMs != expected.work.importedAtMs)
            return failure(LibraryDiagnosticCode::StaleWorkSnapshot, m_databasePath,
                QStringLiteral("The work identity changed before subtitle cleanup."));
        QVector<LibraryFile> present;
        for (const auto &file : missing) {
            const auto found = std::find_if(current.files.cbegin(), current.files.cend(),
                [&](const auto &value) { return value.id == file.id; });
            if (found == current.files.cend()) continue;
            if (found->workId != file.workId || found->relativePath != file.relativePath || found->type != file.type
                || found->fileName != file.fileName || found->extension != file.extension)
                return failure(LibraryDiagnosticCode::StaleWorkSnapshot, m_databasePath,
                    QStringLiteral("A subtitle identity changed before cleanup."));
            present.append(*found);
        }
        return removeConfirmedMissingSubtitleRows(current.work.rootPath, present);
    });
}


LibraryOperationResult LibraryRepository::writeSubtitleChange(
    const std::function<LibraryOperationResult()> &change)
{
    const auto ready = checkReady();
    if (!ready.success)
        return ready;
    // Reserve the writer before every endpoint/preference lookup, including
    // three-state preference writes. Never upgrade a read transaction later.
    SqliteWriteTransaction transaction(m_database, QStringLiteral("Subtitle transaction"),
                                       m_writeTransactionOptions);
    if (!transaction.begin()) {
        return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                       transaction.errorMessage());
    }

    // All queries belong to the callback and are destroyed before rollback or
    // connection recovery. No active query retains a removed connection.
    auto result = change();
    if (result.success && transaction.commit())
        return result;
    if (result.success) {
        result = failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                         transaction.errorMessage());
    }
    if (!transaction.rollback()) {
        result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                      transaction.errorMessage()});
        // RAISE(ROLLBACK), for example, may already have ended the transaction.
        // The caller can explicitly reopen before further repository work.
        releaseConnection();
    }
    return result;
}

LibraryWorkSnapshotResult LibraryRepository::workSnapshot(qint64 workId)
{
    return readWorkSnapshot(workId, false);
}

LibraryWorkBrowseResult LibraryRepository::workBrowseSnapshot(qint64 workId)
{
    return readWorkSnapshot(workId, true);
}

LibraryWorkBrowseResult LibraryRepository::readWorkSnapshot(qint64 workId, bool includeSubtitleSummary)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibraryWorkBrowseResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    if (workId <= 0) {
        return failure<LibraryWorkBrowseResult>(LibraryDiagnosticCode::InvalidWorkSnapshot, m_databasePath,
                                                  QStringLiteral("A work snapshot requires a positive stable work ID."));
    }
    QSqlQuery query(m_database);
    const QString subtitleSummary = includeSubtitleSummary ? QStringLiteral(
        ", CASE WHEN f.type = 0 AND f.missing = 0 THEN EXISTS ("
        "SELECT 1 FROM subtitle_bindings b JOIN work_files s ON s.id = b.subtitle_file_id "
        "WHERE b.audio_file_id = f.id AND s.work_id = w.id AND s.type = 1 AND s.missing = 0"
        ") ELSE 0 END") : QString();
    // The file rows and their binding summaries are read by one SQLite SELECT,
    // so another connection cannot interleave a different committed snapshot.
    if (!query.prepare(QStringLiteral(
            "SELECT w.id, w.root_path, w.display_name, w.imported_at_ms, "
            "f.id, f.work_id, f.relative_path, f.file_name, f.extension, f.type, f.missing")
            + subtitleSummary + QStringLiteral(" "
            "FROM works w LEFT JOIN work_files f ON f.work_id = w.id "
            "WHERE w.id = ? ORDER BY f.relative_path COLLATE BINARY, f.id"))) {
        return failure<LibraryWorkBrowseResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                  sqlMessage(QStringLiteral("Prepare work snapshot"), query.lastError()));
    }
    query.addBindValue(workId);
    if (!query.exec()) {
        return failure<LibraryWorkBrowseResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                  sqlMessage(QStringLiteral("Read work snapshot"), query.lastError()));
    }
    LibraryWorkBrowseResult result;
    bool foundWork = false;
    while (query.next()) {
        if (!foundWork) {
            result.work = {query.value(0).toLongLong(), query.value(1).toString(),
                           query.value(2).toString(), query.value(3).toLongLong()};
            foundWork = true;
        }
        if (query.value(4).isNull())
            continue; // Keep the identity of a work with no file rows.
        const int type = query.value(9).toInt();
        const int missing = query.value(10).toInt();
        if (query.value(4).toLongLong() <= 0 || query.value(5).toLongLong() != workId
            || (type != 0 && type != 1) || (missing != 0 && missing != 1)) {
            return failure<LibraryWorkBrowseResult>(LibraryDiagnosticCode::InvalidSchema, m_databasePath,
                                                      QStringLiteral("Stored work snapshot has an invalid file identity, type or missing flag."));
        }
        result.files.push_back({query.value(4).toLongLong(), query.value(5).toLongLong(),
                                query.value(6).toString(), query.value(7).toString(),
                                query.value(8).toString(),
                                type == 0 ? LibraryFileType::Audio : LibraryFileType::Subtitle,
                                missing != 0});
        if (includeSubtitleSummary && query.value(11).toBool())
            result.subtitleAudioFileIds.insert(query.value(4).toLongLong());
    }
    if (query.lastError().isValid()) {
        return failure<LibraryWorkBrowseResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                  sqlMessage(QStringLiteral("Read work snapshot rows"), query.lastError()));
    }
    if (!foundWork) {
        return failure<LibraryWorkBrowseResult>(LibraryDiagnosticCode::InvalidWorkSnapshot, m_databasePath,
                                                  QStringLiteral("Work ID %1 does not exist in the library.").arg(workId));
    }
    result.success = true;
    return result;
}

LibraryAutoBindingResult LibraryRepository::addAutoSubtitleBindings(
    const LibraryWorkSnapshotResult &expectedSnapshot,
    const QVector<AutoSubtitleBinding> &bindings)
{
    LibraryAutoBindingResult counts;
    const auto written = writeSubtitleChange([&]() -> LibraryOperationResult {
        if (!expectedSnapshot.success || expectedSnapshot.work.id <= 0) {
            return failure(LibraryDiagnosticCode::InvalidWorkSnapshot, m_databasePath,
                           QStringLiteral("Automatic binding requires a successful complete work snapshot."));
        }
        // This JOIN is read only after BEGIN IMMEDIATE has acquired the writer.
        // Include missing historical records and every raw work/file field, so
        // added, missing, restored, renamed or reassigned records reject stale
        // suggestions. User bindings/preferences intentionally are not compared.
        const auto current = workSnapshot(expectedSnapshot.work.id);
        if (!current.success) {
            for (const auto &diagnostic : current.diagnostics) {
                if (diagnostic.code == LibraryDiagnosticCode::InvalidWorkSnapshot) {
                    return failure(LibraryDiagnosticCode::StaleWorkSnapshot, m_databasePath,
                                   QStringLiteral("The work no longer exists; automatic suggestions were not applied."));
                }
            }
            LibraryOperationResult failed;
            failed.diagnostics = current.diagnostics;
            return failed;
        }
        if (current.work != expectedSnapshot.work || current.files != expectedSnapshot.files) {
            return failure(LibraryDiagnosticCode::StaleWorkSnapshot, m_databasePath,
                           QStringLiteral("The work snapshot changed; automatic suggestions were not applied."));
        }

        QHash<qint64, const LibraryFile *> filesById;
        for (const auto &file : current.files)
            filesById.insert(file.id, &file);
        QVector<AutoSubtitleBinding> uniqueBindings;
        QSet<QPair<qint64, qint64>> seen;
        // Validate the entire deduplicated batch before the first mutation.
        for (const auto &binding : bindings) {
            const QPair<qint64, qint64> pair{binding.audioFileId, binding.subtitleFileId};
            if (seen.contains(pair))
                continue;
            seen.insert(pair);
            const auto *audio = filesById.value(binding.audioFileId, nullptr);
            const auto *subtitle = filesById.value(binding.subtitleFileId, nullptr);
            if (binding.audioFileId <= 0 || binding.subtitleFileId <= 0
                || !audio || !subtitle || audio->workId != current.work.id
                || subtitle->workId != current.work.id
                || audio->type != LibraryFileType::Audio
                || subtitle->type != LibraryFileType::Subtitle || audio->missing || subtitle->missing) {
                return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                               QStringLiteral("Automatic binding requires present audio and subtitle IDs in the expected work."));
            }
            uniqueBindings.push_back(binding);
        }
        for (const auto &binding : uniqueBindings) {
            bool exists = false;
            int source = 0;
            {
                QSqlQuery query(m_database);
                if (!query.prepare(QStringLiteral("SELECT source FROM subtitle_bindings "
                                                  "WHERE audio_file_id = ? AND subtitle_file_id = ?"))) {
                    return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                   sqlMessage(QStringLiteral("Prepare automatic binding lookup"), query.lastError()));
                }
                query.addBindValue(binding.audioFileId);
                query.addBindValue(binding.subtitleFileId);
                if (!query.exec()) {
                    return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                   sqlMessage(QStringLiteral("Find automatic binding"), query.lastError()));
                }
                exists = query.next();
                if (exists)
                    source = query.value(0).toInt();
                if ((exists && query.next()) || query.lastError().isValid()) {
                    return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                   sqlMessage(QStringLiteral("Read automatic binding"), query.lastError()));
                }
            }
            if (exists) {
                if (source != static_cast<int>(SubtitleBindingSource::Auto)
                    && source != static_cast<int>(SubtitleBindingSource::Manual)) {
                    return failure(LibraryDiagnosticCode::InvalidSchema, m_databasePath,
                                   QStringLiteral("Stored automatic binding has an invalid source."));
                }
                ++counts.existingBindings;
                if (source == static_cast<int>(SubtitleBindingSource::Manual))
                    ++counts.manualBindingsPreserved;
                continue; // Never issue an UPDATE, including for Manual sources.
            }
            QSqlQuery query(m_database);
            if (!query.prepare(QStringLiteral("INSERT INTO subtitle_bindings "
                                              "(audio_file_id, subtitle_file_id, source) VALUES (?, ?, ?)"))) {
                return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                               sqlMessage(QStringLiteral("Prepare automatic binding insert"), query.lastError()));
            }
            query.addBindValue(binding.audioFileId);
            query.addBindValue(binding.subtitleFileId);
            query.addBindValue(static_cast<int>(SubtitleBindingSource::Auto));
            if (!query.exec()) {
                return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                               sqlMessage(QStringLiteral("Insert automatic binding"), query.lastError()));
            }
            if (query.numRowsAffected() != 1) {
                return failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                               QStringLiteral("The automatic binding insert did not create exactly one relationship."));
            }
            ++counts.insertedBindings;
        }
        return {true, {}};
    });
    if (!written.success) {
        LibraryAutoBindingResult failed;
        failed.diagnostics = written.diagnostics;
        return failed;
    }
    counts.success = true;
    return counts;
}

LibraryOperationResult LibraryRepository::bindSubtitle(
    qint64 audioFileId, qint64 subtitleFileId, SubtitleBindingSource source,
    SubtitleBindingAvailabilityPolicy availabilityPolicy)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        if (source != SubtitleBindingSource::Auto && source != SubtitleBindingSource::Manual) {
            return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                           QStringLiteral("Unknown subtitle binding source."));
        }
        if (availabilityPolicy != SubtitleBindingAvailabilityPolicy::AllowMissing
            && availabilityPolicy != SubtitleBindingAvailabilityPolicy::RequirePresent) {
            return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                           QStringLiteral("Unknown subtitle binding availability policy."));
        }
        const auto valid = validateBindingPair(m_database, m_databasePath, audioFileId, subtitleFileId,
            availabilityPolicy == SubtitleBindingAvailabilityPolicy::RequirePresent);
        if (!valid.success)
            return valid;
        return executeChange(m_database, m_databasePath, QStringLiteral("Bind subtitle"),
                             QStringLiteral("INSERT INTO subtitle_bindings(audio_file_id, subtitle_file_id, source) "
                                            "VALUES(?, ?, ?) ON CONFLICT(audio_file_id, subtitle_file_id) "
                                            "DO UPDATE SET source = CASE WHEN subtitle_bindings.source = 1 "
                                            "THEN 1 ELSE excluded.source END"),
                             {audioFileId, subtitleFileId, static_cast<int>(source)});
    });
}

LibrarySubtitleBindingsResult LibraryRepository::subtitleBindings(qint64 audioFileId)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibrarySubtitleBindingsResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    if (audioFileId <= 0) {
        return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                                                       QStringLiteral("Subtitle operations require positive stable file IDs."));
    }
    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral("SELECT a.work_id, a.type, a.missing, b.subtitle_file_id, b.source, "
                                      "s.work_id, s.type, s.missing FROM work_files a "
                                      "LEFT JOIN subtitle_bindings b ON b.audio_file_id = a.id "
                                      "LEFT JOIN work_files s ON s.id = b.subtitle_file_id "
                                      "WHERE a.id = ? ORDER BY b.subtitle_file_id"))) {
        return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                       sqlMessage(QStringLiteral("Prepare subtitle bindings query"), query.lastError()));
    }
    query.addBindValue(audioFileId);
    if (!query.exec()) {
        return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                       sqlMessage(QStringLiteral("Read subtitle bindings"), query.lastError()));
    }
    LibrarySubtitleBindingsResult result;
    bool foundAudio = false;
    while (query.next()) {
        foundAudio = true;
        if (query.value(1).toInt() != 0) {
            return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                                                           QStringLiteral("File ID %1 is not an audio file.").arg(audioFileId));
        }
        if (query.value(3).isNull())
            continue; // An existing audio file with no persisted relationships.
        const qint64 workId = query.value(0).toLongLong();
        const qint64 subtitleFileId = query.value(3).toLongLong();
        const int source = query.value(4).toInt();
        if (subtitleFileId <= 0 || query.value(4).isNull() || query.value(5).isNull()
            || query.value(5).toLongLong() != workId || query.value(6).toInt() != 1
            || (source != 0 && source != 1)) {
            return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::InvalidSchema, m_databasePath,
                                                           QStringLiteral("Stored subtitle binding has invalid endpoints or source."));
        }
        result.bindings.push_back({workId, audioFileId, subtitleFileId,
                                   static_cast<SubtitleBindingSource>(source),
                                   availability(query.value(2).toBool(), query.value(7).toBool())});
    }
    if (query.lastError().isValid()) {
        return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                       sqlMessage(QStringLiteral("Read subtitle binding rows"), query.lastError()));
    }
    if (!foundAudio) {
        return failure<LibrarySubtitleBindingsResult>(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                                                       QStringLiteral("File ID %1 does not exist in the library.").arg(audioFileId));
    }
    result.success = true;
    return result;
}

LibraryOperationResult LibraryRepository::removeSubtitleBinding(qint64 audioFileId, qint64 subtitleFileId)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        const auto valid = validateBindingPair(m_database, m_databasePath, audioFileId, subtitleFileId);
        if (!valid.success)
            return valid;
        const auto cleared = executeChange(m_database, m_databasePath, QStringLiteral("Clear removed subtitle preference"),
                                          QStringLiteral("DELETE FROM track_subtitle_preferences "
                                                         "WHERE audio_file_id = ? AND subtitle_file_id = ?"),
                                          {audioFileId, subtitleFileId});
        if (!cleared.success)
            return cleared;
        return executeChange(m_database, m_databasePath, QStringLiteral("Remove subtitle binding"),
                             QStringLiteral("DELETE FROM subtitle_bindings "
                                            "WHERE audio_file_id = ? AND subtitle_file_id = ?"),
                             {audioFileId, subtitleFileId});
    });
}

LibraryOperationResult LibraryRepository::setPreferredSubtitle(
    qint64 audioFileId, qint64 subtitleFileId, SubtitlePreferenceBindingPolicy bindingPolicy)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        if (bindingPolicy != SubtitlePreferenceBindingPolicy::ConfirmManual
            && bindingPolicy != SubtitlePreferenceBindingPolicy::PreserveBindingSource) {
            return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                           QStringLiteral("Unknown preferred subtitle binding policy."));
        }
        const auto valid = validateBindingPair(m_database, m_databasePath, audioFileId, subtitleFileId);
        if (!valid.success)
            return valid;
        const auto bound = requireBinding(m_database, m_databasePath, audioFileId, subtitleFileId);
        if (!bound.success)
            return bound;
        if (bindingPolicy == SubtitlePreferenceBindingPolicy::ConfirmManual) {
            const auto confirmed = executeChange(m_database, m_databasePath, QStringLiteral("Confirm preferred subtitle binding"),
                                                QStringLiteral("UPDATE subtitle_bindings SET source = ? "
                                                               "WHERE audio_file_id = ? AND subtitle_file_id = ?"),
                                                {static_cast<int>(SubtitleBindingSource::Manual), audioFileId, subtitleFileId});
            if (!confirmed.success)
                return confirmed;
        }
        return executeChange(m_database, m_databasePath, QStringLiteral("Set preferred subtitle"),
                             QStringLiteral("INSERT INTO track_subtitle_preferences(audio_file_id, subtitle_file_id) "
                                            "VALUES(?, ?) ON CONFLICT(audio_file_id) "
                                            "DO UPDATE SET subtitle_file_id = excluded.subtitle_file_id"),
                             {audioFileId, subtitleFileId});
    });
}

LibraryOperationResult LibraryRepository::useSubtitle(qint64 audioFileId, qint64 subtitleFileId)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        const auto valid = validateBindingPair(m_database, m_databasePath, audioFileId, subtitleFileId, true);
        if (!valid.success)
            return valid;
        const auto bound = executeChange(m_database, m_databasePath, QStringLiteral("Bind selected subtitle"),
            QStringLiteral("INSERT INTO subtitle_bindings(audio_file_id, subtitle_file_id, source) "
                           "VALUES(?, ?, ?) ON CONFLICT(audio_file_id, subtitle_file_id) DO NOTHING"),
            {audioFileId, subtitleFileId, static_cast<int>(SubtitleBindingSource::Manual)});
        if (!bound.success)
            return bound;
        return executeChange(m_database, m_databasePath, QStringLiteral("Use selected subtitle"),
            QStringLiteral("INSERT INTO track_subtitle_preferences(audio_file_id, subtitle_file_id) "
                           "VALUES(?, ?) ON CONFLICT(audio_file_id) "
                           "DO UPDATE SET subtitle_file_id = excluded.subtitle_file_id"),
            {audioFileId, subtitleFileId});
    });
}

LibraryOperationResult LibraryRepository::setNoSubtitlePreference(qint64 audioFileId)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        BindingFile audio;
        const auto valid = findBindingFile(m_database, m_databasePath, audioFileId,
                                          LibraryFileType::Audio, audio);
        if (!valid.success)
            return valid;
        return executeChange(m_database, m_databasePath, QStringLiteral("Set no-subtitle preference"),
                             QStringLiteral("INSERT INTO track_subtitle_preferences(audio_file_id, subtitle_file_id) "
                                            "VALUES(?, NULL) ON CONFLICT(audio_file_id) "
                                            "DO UPDATE SET subtitle_file_id = NULL"),
                             {audioFileId});
    });
}

LibraryOperationResult LibraryRepository::clearSubtitlePreference(qint64 audioFileId)
{
    return writeSubtitleChange([&]() -> LibraryOperationResult {
        BindingFile audio;
        const auto valid = findBindingFile(m_database, m_databasePath, audioFileId,
                                          LibraryFileType::Audio, audio);
        if (!valid.success)
            return valid;
        return executeChange(m_database, m_databasePath, QStringLiteral("Clear subtitle preference"),
                             QStringLiteral("DELETE FROM track_subtitle_preferences WHERE audio_file_id = ?"),
                             {audioFileId});
    });
}

LibrarySubtitlePreferenceResult LibraryRepository::subtitlePreference(qint64 audioFileId)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibrarySubtitlePreferenceResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    if (audioFileId <= 0) {
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                                                         QStringLiteral("Subtitle operations require positive stable file IDs."));
    }
    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral("SELECT a.work_id, a.type, a.missing, p.audio_file_id, p.subtitle_file_id, "
                                      "b.audio_file_id, s.work_id, s.type, s.missing FROM work_files a "
                                      "LEFT JOIN track_subtitle_preferences p ON p.audio_file_id = a.id "
                                      "LEFT JOIN subtitle_bindings b ON b.audio_file_id = p.audio_file_id "
                                      "AND b.subtitle_file_id = p.subtitle_file_id "
                                      "LEFT JOIN work_files s ON s.id = p.subtitle_file_id WHERE a.id = ?"))) {
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                         sqlMessage(QStringLiteral("Prepare subtitle preference query"), query.lastError()));
    }
    query.addBindValue(audioFileId);
    if (!query.exec()) {
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                         sqlMessage(QStringLiteral("Read subtitle preference"), query.lastError()));
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                             sqlMessage(QStringLiteral("Read subtitle preference row"), query.lastError()));
        }
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                                                         QStringLiteral("File ID %1 does not exist in the library.").arg(audioFileId));
    }
    if (query.value(1).toInt() != 0) {
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                                                         QStringLiteral("File ID %1 is not an audio file.").arg(audioFileId));
    }
    LibrarySubtitlePreferenceResult result;
    result.preference.audioFileId = audioFileId;
    if (!query.value(3).isNull()) {
        if (query.value(4).isNull()) {
            result.preference.state = SubtitlePreferenceState::None;
        } else {
            const qint64 subtitleFileId = query.value(4).toLongLong();
            if (subtitleFileId <= 0 || query.value(5).isNull() || query.value(6).isNull()
                || query.value(6).toLongLong() != query.value(0).toLongLong()
                || query.value(7).toInt() != 1) {
                return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::InvalidSchema, m_databasePath,
                                                                 QStringLiteral("Stored preferred subtitle has invalid endpoints or no binding."));
            }
            result.preference.state = SubtitlePreferenceState::Subtitle;
            result.preference.subtitleFileId = subtitleFileId;
            result.preference.availability = availability(query.value(2).toBool(), query.value(8).toBool());
        }
    }
    if (query.next()) {
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::InvalidSchema, m_databasePath,
                                                         QStringLiteral("An audio file has multiple stored preferences."));
    }
    if (query.lastError().isValid()) {
        return failure<LibrarySubtitlePreferenceResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                                         sqlMessage(QStringLiteral("Read subtitle preference rows"), query.lastError()));
    }
    result.success = true;
    return result;
}

LibraryTrackSubtitlesResult LibraryRepository::trackSubtitles(qint64 workId, qint64 audioFileId)
{
    LibraryTrackSubtitlesResult result;
    const auto ready = checkReady();
    if (!ready.success) {
        result.diagnostics = ready.diagnostics;
        return result;
    }
    if (!m_database.transaction()) {
        result.diagnostics = failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            sqlMessage(QStringLiteral("Begin track subtitle snapshot"), m_database.lastError())).diagnostics;
        return result;
    }
    // Finish each query before commit/rollback/connection recovery.
    const auto read = [&]() -> LibraryOperationResult {
        const auto snapshot = workSnapshot(workId);
        if (!snapshot.success)
            return {false, snapshot.diagnostics};
        const auto audio = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
            [audioFileId](const auto &file) { return file.id == audioFileId; });
        if (audio == snapshot.files.cend() || audio->type != LibraryFileType::Audio) {
            return failure(LibraryDiagnosticCode::InvalidSubtitleBinding, m_databasePath,
                           QStringLiteral("Audio ID does not belong to the requested work."));
        }
        const auto bindings = subtitleBindings(audioFileId);
        const auto preference = subtitlePreference(audioFileId);
        if (!bindings.success || !preference.success)
            return {false, bindings.diagnostics + preference.diagnostics};
        result.work = snapshot.work;
        result.audio = *audio;
        result.files = snapshot.files;
        result.bindings = bindings.bindings;
        result.preference = preference.preference;
        return {true, {}};
    }();
    if (read.success && m_database.commit()) {
        result.success = true;
        return result;
    }
    LibraryTrackSubtitlesResult failed;
    failed.diagnostics = read.diagnostics;
    if (read.success)
        failed.diagnostics = failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            sqlMessage(QStringLiteral("Commit track subtitle snapshot"), m_database.lastError())).diagnostics;
    if (!m_database.rollback()) {
        failed.diagnostics += failure(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            sqlMessage(QStringLiteral("Rollback track subtitle snapshot"), m_database.lastError())).diagnostics;
        releaseConnection();
    }
    return failed;
}

} // namespace asmr
