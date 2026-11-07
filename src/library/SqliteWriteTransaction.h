#pragma once

#include <QSqlDatabase>
#include <QString>

#include <functional>

class QSqlError;

namespace asmr {

// Connection-local transaction ownership. Never replays the transaction body.
// Construct, use and destroy on the connection's owning worker thread.
class SqliteWriteTransaction final {
public:
    enum class TestPoint { BeforeBegin, BeginBusy, AfterBegin, BeforeCommit, CommitBusy, AfterCommit };
    struct Options {
        int lockWaitBudgetMs = 5000;
        std::function<void(TestPoint)> testHook;
    };

    SqliteWriteTransaction(QSqlDatabase &database, QString operation);
    SqliteWriteTransaction(QSqlDatabase &database, QString operation, Options options);
    ~SqliteWriteTransaction();
    SqliteWriteTransaction(const SqliteWriteTransaction &) = delete;
    SqliteWriteTransaction &operator=(const SqliteWriteTransaction &) = delete;

    bool begin();
    bool commit();
    // Even a failed rollback relinquishes ownership: the caller must then
    // release the connection after its local queries have been destroyed.
    bool rollback();
    QString errorMessage() const { return m_error; }
    static QString describeError(const QString &operation, const QSqlError &error);

private:
    bool executeBoundary(bool committing);
    bool setBusyTimeout(int milliseconds);
    void restoreBusyTimeout();
    void notify(TestPoint point) const;

    QSqlDatabase &m_database;
    QString m_operation;
    Options m_options;
    QString m_error;
    qint64 m_lockWaitMs = 0;
    int m_previousBusyTimeout = -1;
    bool m_active = false;
};

} // namespace asmr
