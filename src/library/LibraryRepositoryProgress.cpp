#include "library/LibraryRepository.h"
#include "library/SqliteWriteTransaction.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <limits>

namespace asmr {
namespace {
template<typename Result>
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
QVariant nullableInteger(std::optional<qint64> value)
{
    return value ? QVariant::fromValue(*value) : QVariant(QMetaType::fromType<qint64>());
}
}

LibraryPlaybackProgressResult LibraryRepository::playbackProgress(qint64 audioFileId)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibraryPlaybackProgressResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    if (audioFileId <= 0)
        return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::InvalidPlaybackProgress,
            m_databasePath, QStringLiteral("Playback progress requires a positive stable audio file ID."));

    // One statement distinguishes an invalid endpoint, a never-written audio
    // record, a clear tombstone and a current recovery value without a read race.
    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral(
        "SELECT a.type, p.audio_file_id, p.position_ms, p.duration_ms, p.updated_at_ms, p.revision, "
        "typeof(p.position_ms), typeof(p.duration_ms), typeof(p.updated_at_ms), typeof(p.revision), typeof(a.type) "
        "FROM work_files a LEFT JOIN track_playback_progress p ON p.audio_file_id = a.id WHERE a.id = ?")))
        return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            sqlMessage(QStringLiteral("Prepare playback progress query"), query.lastError()));
    query.addBindValue(audioFileId);
    if (!query.exec())
        return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            sqlMessage(QStringLiteral("Read playback progress"), query.lastError()));
    if (!query.next()) {
        if (query.lastError().isValid())
            return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                sqlMessage(QStringLiteral("Read playback progress endpoint"), query.lastError()));
        return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::InvalidPlaybackProgress,
            m_databasePath, QStringLiteral("The playback progress audio file ID does not exist."));
    }
    if (query.value(10).toString() != QStringLiteral("integer") || query.value(0).toLongLong() != 0)
        return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::InvalidPlaybackProgress,
            m_databasePath, QStringLiteral("Playback progress can only belong to an audio file."));

    LibraryPlaybackProgressResult result;
    if (!query.value(1).isNull()) {
        const bool cleared = query.value(2).isNull();
        const bool unknownDuration = query.value(3).isNull();
        bool positionOk = cleared;
        const qint64 position = cleared ? 0 : query.value(2).toLongLong(&positionOk);
        bool durationOk = unknownDuration;
        const qint64 duration = unknownDuration ? 0 : query.value(3).toLongLong(&durationOk);
        bool updatedOk = false, revisionOk = false;
        const qint64 updated = query.value(4).toLongLong(&updatedOk);
        const qint64 revision = query.value(5).toLongLong(&revisionOk);
        if (!positionOk || !durationOk || !updatedOk || !revisionOk
            || position < 0 || duration < 0 || updated < 0 || revision <= 0
            || (cleared && !unknownDuration)
            || (!cleared && query.value(6).toString() != QStringLiteral("integer"))
            || (!unknownDuration && query.value(7).toString() != QStringLiteral("integer"))
            || query.value(8).toString() != QStringLiteral("integer")
            || query.value(9).toString() != QStringLiteral("integer"))
            return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::InvalidSchema,
                m_databasePath, QStringLiteral("Stored playback progress has invalid integer values or clear state."));
        result.revision = revision;
        if (!cleared)
            result.progress = LibraryPlaybackProgress{audioFileId, position,
                unknownDuration ? std::nullopt : std::optional<qint64>(duration), updated, revision};
    }
    if (query.next() || query.lastError().isValid())
        return failure<LibraryPlaybackProgressResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            sqlMessage(QStringLiteral("Read playback progress rows"), query.lastError()));
    result.success = true;
    return result;
}

LibraryPlaybackProgressWriteResult LibraryRepository::savePlaybackProgress(
    qint64 audioFileId, qint64 positionMs, std::optional<qint64> durationMs, qint64 expectedRevision)
{
    return writePlaybackProgressChange(audioFileId, positionMs, durationMs, expectedRevision);
}

LibraryPlaybackProgressWriteResult LibraryRepository::clearPlaybackProgress(qint64 audioFileId,
                                                                           qint64 expectedRevision)
{
    return writePlaybackProgressChange(audioFileId, std::nullopt, std::nullopt, expectedRevision);
}

LibraryPlaybackProgressWriteResult LibraryRepository::writePlaybackProgressChange(
    qint64 audioFileId, std::optional<qint64> positionMs,
    std::optional<qint64> durationMs, qint64 expectedRevision)
{
    const auto ready = checkReady();
    if (!ready.success) {
        LibraryPlaybackProgressWriteResult result;
        result.diagnostics = ready.diagnostics;
        return result;
    }
    if (audioFileId <= 0 || expectedRevision < 0
        || (positionMs && *positionMs < 0) || (durationMs && *durationMs < 0))
        return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::InvalidPlaybackProgress,
            m_databasePath, QStringLiteral("Playback progress requires a positive audio ID and nonnegative position, duration and revision."));

    // Take the writer before observing the revision and endpoint. A separate
    // repository/import cannot change either between this check and the write.
    SqliteWriteTransaction transaction(m_database, QStringLiteral("Playback progress transaction"),
                                       m_writeTransactionOptions);
    if (!transaction.begin())
        return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            transaction.errorMessage());
    auto result = [&]() -> LibraryPlaybackProgressWriteResult {
        const auto current = playbackProgress(audioFileId);
        if (!current.success) {
            LibraryPlaybackProgressWriteResult failed;
            failed.diagnostics = current.diagnostics;
            return failed;
        }
        if (current.revision != expectedRevision)
            return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::StalePlaybackProgress,
                m_databasePath, QStringLiteral("Playback progress changed since revision %1; this stale operation was not applied.")
                    .arg(expectedRevision));
        if (current.revision == std::numeric_limits<qint64>::max())
            return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::InvalidPlaybackProgress,
                m_databasePath, QStringLiteral("Playback progress revision is exhausted; no write was applied."));
        const qint64 nextRevision = current.revision + 1;
        const qint64 updated = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
        if (updated < 0)
            return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::InvalidPlaybackProgress,
                m_databasePath, QStringLiteral("The current UTC clock cannot be stored as a nonnegative update time."));
        QSqlQuery query(m_database);
        if (!query.prepare(QStringLiteral(
            "INSERT INTO track_playback_progress(audio_file_id, position_ms, duration_ms, updated_at_ms, revision) "
            "VALUES(?, ?, ?, ?, ?) ON CONFLICT(audio_file_id) DO UPDATE SET "
            "position_ms = excluded.position_ms, duration_ms = excluded.duration_ms, "
            "updated_at_ms = excluded.updated_at_ms, revision = excluded.revision "
            "WHERE track_playback_progress.revision = ?")))
            return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                sqlMessage(QStringLiteral("Prepare playback progress write"), query.lastError()));
        query.addBindValue(audioFileId);
        query.addBindValue(nullableInteger(positionMs));
        query.addBindValue(nullableInteger(durationMs));
        query.addBindValue(updated);
        query.addBindValue(nextRevision);
        query.addBindValue(expectedRevision);
        if (!query.exec())
            return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
                sqlMessage(QStringLiteral("Write playback progress"), query.lastError()));
        if (query.numRowsAffected() != 1)
            return failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::DatabaseError,
                m_databasePath, QStringLiteral("Playback progress write did not change exactly one record."));
        LibraryPlaybackProgressWriteResult written;
        written.success = true;
        written.revision = nextRevision;
        return written;
    }(); // No query survives the callback into commit/rollback/connection recovery.
    if (result.success && transaction.commit())
        return result;
    if (result.success)
        result = failure<LibraryPlaybackProgressWriteResult>(LibraryDiagnosticCode::DatabaseError, m_databasePath,
            transaction.errorMessage());
    result.revision = 0;
    if (!transaction.rollback()) {
        result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, m_databasePath,
            transaction.errorMessage()});
        // rollback() ends helper ownership even on failure, so connection
        // recovery cannot leave its destructor touching a removed connection.
        releaseConnection();
    }
    return result;
}
} // namespace asmr
