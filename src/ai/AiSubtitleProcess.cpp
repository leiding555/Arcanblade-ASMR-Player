#include "ai/AiSubtitleProcess.h"
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <algorithm>
#include <vector>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "ai/WindowsConsoleHostCache.h"
#endif
namespace asmr {
namespace {
constexpr qint64 ProcessLogLimit = 4 * 1024 * 1024;
constexpr qsizetype TailLimit = 16 * 1024;
#ifdef Q_OS_WIN
class ProcessJob final {
public:
    ~ProcessJob()
    {
        if (m_attributes)
            DeleteProcThreadAttributeList(m_attributes);
        if (m_job)
            CloseHandle(m_job);
    }
    bool initialize(QString *error)
    {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job) {
            *error = QStringLiteral("Could not create Windows process job (%1).").arg(GetLastError());
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            *error = QStringLiteral("Could not configure Windows process job (%1).").arg(GetLastError());
            return false;
        }
        wchar_t systemDirectory[MAX_PATH + 1]{};
        const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (!systemLength || systemLength >= MAX_PATH) {
            *error = QStringLiteral("Could not determine the Windows system console-host path (%1).").arg(GetLastError());
            return false;
        }
        m_consoleHosts = std::make_unique<detail::WindowsConsoleHostCache>(
            QDir(QString::fromWCharArray(systemDirectory, int(systemLength))).filePath(QStringLiteral("conhost.exe")));
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) {
            *error = QStringLiteral("Windows process job attributes are unavailable (%1).").arg(GetLastError());
            return false;
        }
        m_storage.resize(bytes);
        auto *attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
            *error = QStringLiteral("Could not initialize process job attributes (%1).").arg(GetLastError());
            return false;
        }
        m_attributes = attributes;
        if (!UpdateProcThreadAttribute(m_attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                &m_job, sizeof(m_job), nullptr, nullptr)) {
            *error = QStringLiteral("Windows cannot attach the process tree at creation (%1).").arg(GetLastError());
            return false;
        }
        return true;
    }
    void attachAtCreation(QProcess &process)
    {
        process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments *arguments) {
            m_startup.StartupInfo = *arguments->startupInfo;
            m_startup.StartupInfo.cb = sizeof(m_startup);
            m_startup.lpAttributeList = m_attributes;
            arguments->startupInfo = &m_startup.StartupInfo;
            arguments->flags |= EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
        });
    }
    bool activeProcesses(DWORD *count, QString *error) const
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(m_job, JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), nullptr)) {
            *error = QStringLiteral("Could not confirm Windows process-tree completion (%1).").arg(GetLastError());
            return false;
        }
        *count = accounting.ActiveProcesses;
        return true;
    }

    bool hasActiveChildren(ULONG_PTR parentPid, bool *active, QString *details, QString *error)
    {
        *active = false;
        details->clear();
        // Accounting may briefly retain the exited parent. Inspect actual job
        // PIDs instead of treating ActiveProcesses > 0 as proof of live children.
        DWORD capacity = 32;
        for (int attempt = 0; attempt < 5; ++attempt) {
            const size_t byteCount = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)
                + size_t(capacity) * sizeof(ULONG_PTR);
            std::vector<ULONG_PTR> storage((byteCount + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
            auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(storage.data());
            if (!QueryInformationJobObject(m_job, JobObjectBasicProcessIdList,
                    list, DWORD(byteCount), nullptr)) {
                const DWORD code = GetLastError();
                if (code == ERROR_MORE_DATA && capacity < 4096) {
                    capacity = std::min(DWORD(4096), std::max(capacity * 2, list->NumberOfAssignedProcesses));
                    continue;
                }
                *error = QStringLiteral("Could not inspect Windows job child PIDs (%1).").arg(code);
                return false;
            }
            for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
                const ULONG_PTR pid = list->ProcessIdList[index];
                if (pid == parentPid) continue;
                HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
                if (!child) {
                    const DWORD code = GetLastError();
                    if (code == ERROR_INVALID_PARAMETER) continue; // It already exited.
                    *error = QStringLiteral("Could not confirm a job child's lifetime (%1).").arg(code);
                    return false;
                }
                // The PID list is a snapshot. Verify the newly opened object
                // still belongs to this job before trusting a cached identity.
                BOOL member = FALSE;
                const BOOL membershipKnown = IsProcessInJob(child, m_job, &member);
                const DWORD membershipError = membershipKnown ? ERROR_SUCCESS : GetLastError();
                if (!membershipKnown || !member) {
                    const DWORD state = WaitForSingleObject(child, 0);
                    CloseHandle(child);
                    if (state == WAIT_OBJECT_0) continue;
                    *error = QStringLiteral("Could not confirm job membership for active child PID %1 (%2).")
                        .arg(qulonglong(pid)).arg(membershipError);
                    return false;
                }
                QString imagePath;
                DWORD imageError = ERROR_SUCCESS;
                const bool systemConsoleHost = m_consoleHosts->isVerifiedHost(child, &imagePath, &imageError);
                // The query can become unavailable during teardown. Only the
                // same live process object previously verified as the exact
                // system console host may reuse that identity.
                const DWORD state = WaitForSingleObject(child, 0);
                CloseHandle(child);
                if (state == WAIT_TIMEOUT && !systemConsoleHost) {
                    *active = true;
                    if (!details->isEmpty()) details->append(QLatin1Char(';'));
                    details->append(QStringLiteral("worker PID %1: %2").arg(qulonglong(pid))
                        .arg(imagePath.isEmpty() ? QStringLiteral("<unidentified>") : imagePath)
                        + QStringLiteral(" (parent PID %1, image error %2, wait state %3)")
                            .arg(qulonglong(parentPid)).arg(imageError).arg(state));
                }
                else if (state != WAIT_OBJECT_0 && state != WAIT_TIMEOUT) {
                    *error = QStringLiteral("Could not confirm whether a job child is still running.");
                    return false;
                }
            }
            return true;
        }
        *error = QStringLiteral("The local engine created too many child processes to verify safely.");
        return false;
    }

    bool terminateAndDrain(QProcess &process)
    {
        if (m_job)
            TerminateJobObject(m_job, ERROR_CANCELLED);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            if (process.state() != QProcess::NotRunning)
                process.waitForFinished(25);
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (QueryInformationJobObject(m_job, JobObjectBasicAccountingInformation,
                    &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0)
                return true;
            QThread::msleep(10);
        }
        return false;
    }
private:
    HANDLE m_job = nullptr;
    std::vector<unsigned char> m_storage;
    std::unique_ptr<detail::WindowsConsoleHostCache> m_consoleHosts;
    PPROC_THREAD_ATTRIBUTE_LIST m_attributes = nullptr;
    STARTUPINFOEXW m_startup{};
};
#endif

} // namespace
int fasterWhisperProgressPercent(const QByteArray &tail)
{
    static const QRegularExpression bar(QStringLiteral("(?:^|[\\r\\n])\\s*(\\d{1,3})(?:\\.\\d+)?%\\s*\\|"));
    auto matches = bar.globalMatch(QString::fromUtf8(tail));
    int percentage = -1;
    while (matches.hasNext()) {
        const int value = matches.next().captured(1).toInt();
        if (value >= 0 && value <= 100) percentage = value;
    }
    return percentage;
}
AiSubtitleProcessResult runAiSubtitleProcess(const AiSubtitleProcessRequest &request,
    const std::atomic_bool &cancelled, const std::function<void(const AiSubtitleTaskProgress &)> &report)
{
    AiSubtitleProcessResult result;
    QString error;
    QFile processLog(request.logPath);
    if (!processLog.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        { result.error = processLog.errorString(); return result; }
    if (!request.logPreamble.isEmpty()
        && (request.logPreamble.size() > ProcessLogLimit
            || processLog.write(request.logPreamble) != request.logPreamble.size()
            || !processLog.flush())) {
        result.error = QStringLiteral("Could not save task notes in the process log.");
        return result;
    }
    QByteArray tail;
    bool logTruncated = false;
    bool logFailed = false;
    auto drain = [&](QProcess &process) {
        // Bound both disk usage and the diagnostic tail even if an external engine is noisy.
        for (int blockIndex = 0; blockIndex < 64 && process.bytesAvailable() > 0; ++blockIndex) {
            const QByteArray block = process.read(64 * 1024);
            if (block.isEmpty())
                break;
            const qint64 remaining = std::max(qint64(0), ProcessLogLimit - processLog.size());
            if (remaining > 0) {
                const qint64 count = std::min(remaining, qint64(block.size()));
                if (processLog.write(block.constData(), count) != count)
                    logFailed = true;
            }
            if (block.size() > remaining)
                logTruncated = true;
            tail.append(block);
            if (tail.size() > TailLimit)
                tail.remove(0, tail.size() - TailLimit);
        }
    };
    if (cancelled.load())
        return result;
#ifndef Q_OS_WIN
    { result.error = QStringLiteral("This local engine requires Windows process-tree containment."); return result; }
#else
    ProcessJob job;
    if (!job.initialize(&error))
        { result.error = error; return result; }
    QProcess process;
    process.setProgram(request.program);
    process.setArguments(request.arguments);
    process.setWorkingDirectory(request.workingDirectory);
    process.setProcessEnvironment(request.environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    job.attachAtCreation(process);
    report({AiSubtitleTaskState::Running, request.runningMessage, {}});
    process.start();
    if (!process.waitForStarted(10000)) {
        job.terminateAndDrain(process);
        { result.error = QStringLiteral("Could not start the local engine with Windows process-tree containment: %1").arg(process.errorString()); return result; }
    }
    const ULONG_PTR parentPid = ULONG_PTR(process.processId());
    process.closeWriteChannel();
    QElapsedTimer clock, publishClock;
    clock.start();
    publishClock.start();
    QString executionError;
    bool forcedTermination = false;
    bool abnormalExitWithoutWorkers = false;
    QElapsedTimer identityClock;
    while (process.state() != QProcess::NotRunning) {
        if (!identityClock.isValid() || identityClock.elapsed() >= 50) {
            // Warm only verified host identities while image information is
            // available. Live ASR/ffmpeg workers are normal at this stage and
            // do not make the eventual result untrusted. A transient scan
            // error is retried; completion below still requires a strict scan.
            bool ignoredActive = false;
            QString ignoredDetails, ignoredError;
            job.hasActiveChildren(parentPid, &ignoredActive, &ignoredDetails, &ignoredError);
            identityClock.start();
        }
        process.waitForReadyRead(50);
        drain(process);
        if (publishClock.elapsed() >= 150) {
            report({AiSubtitleTaskState::Running, request.runningMessage, QString::fromUtf8(tail),
                request.parseWhisperProgress ? fasterWhisperProgressPercent(tail) : -1});
            publishClock.restart();
        }
        if (cancelled.load() || clock.elapsed() > 24LL * 60 * 60 * 1000 || logFailed) {
            forcedTermination = true;
            if (!cancelled.load())
                executionError = logFailed ? QStringLiteral("Could not save local inference diagnostics.")
                    : QStringLiteral("Local inference exceeded the 24-hour safety timeout.");
            break;
        }
    }
    result.exitCode = process.state() == QProcess::NotRunning ? process.exitCode() : -1;
    const bool parentSucceeded = process.state() == QProcess::NotRunning
        && process.exitStatus() == QProcess::NormalExit && result.exitCode == 0;
    if (!forcedTermination && !parentSucceeded) {
        bool activeWorkers = false;
        QString details, inspectionError;
        abnormalExitWithoutWorkers = job.hasActiveChildren(parentPid, &activeWorkers, &details, &inspectionError)
            && !activeWorkers;
        executionError = QStringLiteral("Local inference engine failed (exit %1). See %2.")
            .arg(result.exitCode).arg(request.logPath);
        forcedTermination = true;
    }
    if (!forcedTermination) {
        // A wrapper can exit before its worker child. Do not kill that child and then accept
        // a parseable prefix as a completed subtitle; success requires natural tree completion.
        bool unverifiedChildren = false;
        QString childDetails;
        QElapsedTimer childClock;
        childClock.start();
        for (;;) {
            DWORD active = 0;
            if (!job.activeProcesses(&active, &executionError)) {
                forcedTermination = true;
                break;
            }
            if (active == 0)
                break;
            bool activeChildren = false;
            QString observed;
            if (!job.hasActiveChildren(parentPid, &activeChildren, &observed, &executionError)) {
                forcedTermination = true;
                break;
            }
            if (activeChildren) childDetails = observed;
            unverifiedChildren = unverifiedChildren || activeChildren;
            // A child may still own the inherited output pipe after the parent
            // has exited. Continue bounded draining while waiting for the job.
            process.waitForReadyRead(25);
            drain(process);
            if (cancelled.load() || childClock.elapsed() >= 30000 || logFailed) {
                forcedTermination = true;
                if (!cancelled.load())
                    executionError = logFailed ? QStringLiteral("Could not save child-process diagnostics.")
                        : QStringLiteral("The engine parent exited, but child processes did not finish within 30 seconds. Partial output was retained.");
                break;
            }
            if (publishClock.elapsed() >= 150) {
                report({AiSubtitleTaskState::Running, QStringLiteral("Waiting for local engine child processes to finish"), QString::fromUtf8(tail)});
                publishClock.restart();
            }
            QThread::msleep(25);
        }
        if (unverifiedChildren && !forcedTermination) {
            // Job accounting confirms completion, but does not aggregate worker
            // exit codes. A parseable prefix is not proof of successful ASR.
            executionError = QStringLiteral("The engine parent exited before its worker children. Child exit codes could not be verified; no subtitle was published. Raw output and logs were retained.\n%1").arg(childDetails);
            forcedTermination = true;
        }
    }
    if (forcedTermination) {
        const bool drained = job.terminateAndDrain(process);
        abnormalExitWithoutWorkers = abnormalExitWithoutWorkers && drained;
        if (!drained)
            result.warnings.append(QStringLiteral("Windows did not confirm that the entire process job drained within five seconds."));
        // Job accounting can reach zero before QProcess has consumed its exit notification.
        if (process.state() != QProcess::NotRunning)
            process.waitForFinished(100);
        result.exitCode = process.state() == QProcess::NotRunning ? process.exitCode() : -1;
        // If only a surviving child was cancelled, the parent's earlier zero is not
        // a successful local inference exit status; no aggregate child exit code is available.
        if (cancelled.load() && result.exitCode == 0)
            result.exitCode = -1;
    }
    // QProcess can retain trailing bytes after the parent has exited.
    for (int pass = 0; pass < 8 && process.bytesAvailable() > 0; ++pass)
        drain(process);
    if (!processLog.flush())
        logFailed = true;
    if (logTruncated)
        result.warnings.append(QStringLiteral("The process log was limited to 4 MiB; the live tail contains the most recent output."));
    result.tail = tail; // Retain structured diagnostics for abnormal exits too.
    if (cancelled.load())
        return result;
    if (logFailed)
        { result.error = QStringLiteral("Could not save process diagnostics; see the task directory."); return result; }
    result.abnormalExitWithoutActiveWorkers = abnormalExitWithoutWorkers;
    if (forcedTermination || !executionError.isEmpty())
        { result.error = executionError + QLatin1Char('\n') + QString::fromUtf8(tail.right(4096)); return result; }

    result.succeeded = true;
    result.tail = tail;
    return result;
#endif
}
} // namespace asmr
