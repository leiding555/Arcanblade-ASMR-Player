#include "library/SqliteWriteTransaction.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace asmr {
namespace {
bool isBusy(const QSqlError &error)
{
    bool valid = false;
    const int code = error.nativeErrorCode().toInt(&valid);
    // Extended SQLite codes retain their primary code in the low byte.
    // SQLITE_LOCKED (6) denotes a different programming/shared-cache issue.
    return valid && code >= 0 && (code & 0xff) == 5;
}
}

SqliteWriteTransaction::SqliteWriteTransaction(QSqlDatabase &database, QString operation)
    : SqliteWriteTransaction(database, std::move(operation), Options{})
{
}

SqliteWriteTransaction::SqliteWriteTransaction(QSqlDatabase &database, QString operation, Options options)
    : m_database(database), m_operation(std::move(operation)), m_options(std::move(options))
{
    m_options.lockWaitBudgetMs = std::max(0, m_options.lockWaitBudgetMs);
}

SqliteWriteTransaction::~SqliteWriteTransaction()
{
    if (m_active && !rollback()) {
        qWarning().noquote() << m_error;
        m_database.close();
    }
    restoreBusyTimeout();
}

QString SqliteWriteTransaction::describeError(const QString &operation, const QSqlError &error)
{
    return operation + QStringLiteral(": ") + error.text()
        + QStringLiteral(" [SQLite native=%1]").arg(error.nativeErrorCode());
}

void SqliteWriteTransaction::notify(TestPoint point) const
{
    if (m_options.testHook)
        m_options.testHook(point);
}

bool SqliteWriteTransaction::setBusyTimeout(int milliseconds)
{
    QSqlQuery query(m_database);
    if (query.exec(QStringLiteral("PRAGMA busy_timeout = %1").arg(milliseconds)))
        return true;
    m_error = describeError(m_operation + QStringLiteral(" / configure lock wait"), query.lastError());
    return false;
}

void SqliteWriteTransaction::restoreBusyTimeout()
{
    if (m_previousBusyTimeout < 0)
        return;
    const int previous = std::exchange(m_previousBusyTimeout, -1);
    if (m_database.isOpen()) {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("PRAGMA busy_timeout = %1").arg(previous)))
            qWarning().noquote() << describeError(m_operation + QStringLiteral(" / restore lock wait"), query.lastError());
    }
}

bool SqliteWriteTransaction::begin()
{
    if (m_active) {
        m_error = m_operation + QStringLiteral(" / begin: transaction already active");
        return false;
    }
    m_lockWaitMs = 0;
    const bool success = executeBoundary(false);
    if (success) {
        m_active = true;
        // An unexpected lock inside a large/cache-spilling transaction must
        // fail safely, not start another independent driver timeout or replay
        // a body containing filesystem/settings side effects.
        if (!setBusyTimeout(0)) {
            const auto error = m_error;
            if (!rollback())
                m_database.close();
            m_error = error;
            return false;
        }
        notify(TestPoint::AfterBegin);
    } else {
        restoreBusyTimeout();
    }
    return success;
}

bool SqliteWriteTransaction::commit()
{
    if (!m_active) {
        m_error = m_operation + QStringLiteral(" / commit: no owned transaction");
        return false;
    }
    const bool success = executeBoundary(true);
    if (success) {
        m_active = false;
        restoreBusyTimeout();
        notify(TestPoint::AfterCommit);
    }
    return success;
}

bool SqliteWriteTransaction::executeBoundary(bool committing)
{
    const QString phase = committing ? QStringLiteral("commit") : QStringLiteral("begin immediate");
    const QString statement = committing ? QStringLiteral("COMMIT") : QStringLiteral("BEGIN IMMEDIATE");
    if (m_previousBusyTimeout < 0) {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("PRAGMA busy_timeout")) || !query.next()) {
            m_error = describeError(m_operation + QStringLiteral(" / read lock wait"), query.lastError());
            return false;
        }
        m_previousBusyTimeout = query.value(0).toInt();
    }
    int attempts = 0;
    for (;;) {
        notify(committing ? TestPoint::BeforeCommit : TestPoint::BeforeBegin);
        const qint64 remaining = std::max<qint64>(0, m_options.lockWaitBudgetMs - m_lockWaitMs);
        // Small driver waits allow an early BUSY to share the same finite
        // budget with backoff and COMMIT, instead of multiplying 5-second waits.
        if (!setBusyTimeout(static_cast<int>(std::min<qint64>(50, remaining)))) {
            return false;
        }
        QSqlError error;
        bool success = false;
        QElapsedTimer timer;
        timer.start();
        {
            QSqlQuery query(m_database);
            success = query.exec(statement);
            error = query.lastError();
        }
        m_lockWaitMs += timer.elapsed();
        ++attempts;
        if (success) {
            m_error.clear();
            if (attempts > 1)
                qInfo().noquote() << QStringLiteral("%1 / %2: lock recovered, attempts=%3, lockWaitMs=%4, budgetMs=%5")
                    .arg(m_operation, phase).arg(attempts).arg(m_lockWaitMs).arg(m_options.lockWaitBudgetMs);
            return true;
        }
        m_error = describeError(m_operation + QStringLiteral(" / ") + phase, error)
            + QStringLiteral(" [attempts=%1, lockWaitMs=%2, budgetMs=%3]")
                .arg(attempts).arg(m_lockWaitMs).arg(m_options.lockWaitBudgetMs);
        if (!isBusy(error)) {
            return false;
        }
        notify(committing ? TestPoint::CommitBusy : TestPoint::BeginBusy);
        if (m_lockWaitMs >= m_options.lockWaitBudgetMs) {
            return false;
        }
        const qint64 delay = std::min<qint64>(std::min(50, attempts * 5),
                                            m_options.lockWaitBudgetMs - m_lockWaitMs);
        timer.restart();
        QThread::msleep(static_cast<unsigned long>(delay));
        m_lockWaitMs += timer.elapsed();
    }
}

bool SqliteWriteTransaction::rollback()
{
    if (!m_active)
        return true;
    // Do not retry, replay a body, or keep touching a released connection after
    // rollback fails (e.g. a trigger already issued RAISE(ROLLBACK)).
    m_active = false;
    bool success = false;
    {
        QSqlQuery query(m_database);
        success = query.exec(QStringLiteral("ROLLBACK"));
        if (!success)
            m_error = describeError(m_operation + QStringLiteral(" / rollback"), query.lastError());
    }
    restoreBusyTimeout();
    return success;
}

} // namespace asmr
