#include "ai/ChickenRiceTranslateService.h"
#include "ai/ChickenRiceArchive.h"
#include "ai/AiSubtitleProcess.h"
#include "ai/AiSubtitleOutput.h"
#include "playback/AudioFileValidation.h"
#include "subtitle/SubtitleFileLoader.h"
#include "subtitle/GeneratedSubtitleInfo.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPromise>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <exception>
#include <utility>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
using Request = ChickenRiceTranslateRequest;
using Result = ChickenRiceTranslateResult;
using Progress = ChickenRiceTranslateProgress;
using State = ChickenRiceTranslateState;
using Report = std::function<void(const Progress &)>;
constexpr qint64 ConfigLimit = 256 * 1024;
constexpr qint64 PreviousLogLimit = 1024 * 1024;

bool reparsePoint(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}
bool safeAncestors(QString path)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        if (reparsePoint(path))
            return false;
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path)
            return true;
        path = parent;
    }
}
bool writeNewFile(const QString &path, const QByteArray &bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || file.write(bytes) != bytes.size() || !file.flush()) {
        *error = QStringLiteral("Could not write task artifact %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}
Result fail(Result result, const QString &error, const std::atomic_bool &cancelled)
{
    result.state = cancelled.load() ? State::Cancelled : State::Failed;
    result.srtPath.clear(); // A failed/cancelled partial file is never offered for loading.
    result.error = cancelled.load() ? QStringLiteral("Translation cancelled. Task logs were retained.") : error;
    return result;
}

Result translate(const Request &request, const std::atomic_bool &cancelled, const Report &report)
{
    Result result;
    result.audioPath = request.audioPath;
    report({State::Preparing, QStringLiteral("Preparing task and checking audio paths"), {}});
    if (cancelled.load())
        return fail(result, {}, cancelled);
    if ((!request.outputBatchId.isEmpty() && !validAiSubtitleOutputToken(request.outputBatchId))
        || (!request.outputFileSuffix.isEmpty() && !validAiSubtitleOutputToken(request.outputFileSuffix)))
        return fail(result, QStringLiteral("Invalid subtitle output batch or filename suffix."), cancelled);
    const bool libraryTrack = !request.libraryRoot.isEmpty() || !request.libraryRelativePath.isEmpty();
    if (libraryTrack && (request.libraryRoot.isEmpty() || request.libraryRelativePath.isEmpty()))
        return fail(result, QStringLiteral("A library track requires both its work root and relative path."), cancelled);
    const auto audio = libraryTrack
        ? validateLibraryAudioFile(request.libraryRoot, request.libraryRelativePath)
        : validateAudioFile(request.audioPath);
    if (!audio.success)
        return fail(result, audio.errorMessage, cancelled);
    const QFileInfo initialAudio(audio.path);
    const qint64 initialAudioSize = initialAudio.size();
    const QDateTime initialAudioModified = initialAudio.lastModified();
    const QString requestedPath = QDir::cleanPath(QDir::fromNativeSeparators(request.audioPath));
#ifdef Q_OS_WIN
    const bool sameRequestedPath = requestedPath.compare(audio.path, Qt::CaseInsensitive) == 0;
#else
    const bool sameRequestedPath = requestedPath == audio.path;
#endif
    if (!sameRequestedPath)
        return fail(result, QStringLiteral("The requested audio path does not match the validated library track."), cancelled);
    const QString engine = QDir::cleanPath(QFileInfo(request.engineDirectory).absoluteFilePath());
    const bool testProcess = !request.executableForTests.isEmpty();
    QString error;
    if (request.engineDirectory.isEmpty() || !QFileInfo(request.engineDirectory).isAbsolute()
        || !safeAncestors(engine) || !QFileInfo(engine).isDir())
        return fail(result, QStringLiteral("Engine directory must be an existing absolute directory without links."), cancelled);
    if (!testProcess) {
        // Structural/JSON checks have no reliable byte percentage.
        report({State::CheckingModel, QStringLiteral("Checking local model files and component configuration"), {}});
        if (!validateChickenRiceEngine(engine, &error))
            return fail(result, error, cancelled);
    }
    if (cancelled.load()) return fail(result, {}, cancelled);
    QString program = testProcess ? request.executableForTests : QDir(engine).filePath(QStringLiteral("infer.exe"));
    if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isFile() || !safeAncestors(program))
        return fail(result, QStringLiteral("Translation executable is missing or unsafe."), cancelled);
    if (!testProcess && !request.argumentsForTests.isEmpty())
        return fail(result, QStringLiteral("Test arguments require an explicit test executable."), cancelled);
    const QString root = QDir::cleanPath(QFileInfo(request.outputRoot).absoluteFilePath());
    if (request.outputRoot.isEmpty() || !QFileInfo(request.outputRoot).isAbsolute()
        || QDir(root).isRoot() || !safeAncestors(root))
        return fail(result, QStringLiteral("Output root must be absolute, non-root and without links."), cancelled);
    if (QFileInfo::exists(root) && !QFileInfo(root).isDir())
        return fail(result, QStringLiteral("Output root is not a directory."), cancelled);
    const QString sourceConfig = QDir(engine).filePath(QStringLiteral("generation_config.json5"));
    QFile config(sourceConfig);
    if (reparsePoint(sourceConfig) || !config.open(QIODevice::ReadOnly) || config.size() <= 0 || config.size() > ConfigLimit)
        return fail(result, QStringLiteral("The local generation configuration is missing, linked or too large."), cancelled);
    const QByteArray configBytes = config.readAll();
    if (config.error() != QFileDevice::NoError)
        return fail(result, config.errorString(), cancelled);
    config.close();
    const QString latestLog = QDir(engine).filePath(QStringLiteral("latest.log"));
    if (reparsePoint(latestLog) || (QFileInfo::exists(latestLog) && !QFileInfo(latestLog).isFile()))
        return fail(result, QStringLiteral("The engine latest.log is linked or is not an ordinary file; it will not be overwritten."), cancelled);
    const QString lockPath = QDir(engine).filePath(QStringLiteral(".arcanblade-translate.lock"));
    if (reparsePoint(lockPath))
        return fail(result, QStringLiteral("Engine translation lock is linked."), cancelled);
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (QFileInfo::exists(lockPath)) {
        qint64 pid = 0;
        QString host, app;
        if (!lock.getLockInfo(&pid, &host, &app) || app != QCoreApplication::applicationName())
            return fail(result, QStringLiteral("The engine has an unknown or another application's lock."), cancelled);
    }
    if (!lock.tryLock(0))
        return fail(result, QStringLiteral("Another translation is using this engine. Run tracks sequentially."), cancelled);
    if (!QDir().mkpath(root))
        return fail(result, QStringLiteral("Could not create the translation output root."), cancelled);
    QString task;
    for (int attempt = 0; attempt < 3 && task.isEmpty(); ++attempt) {
        const QString name = QStringLiteral("mode-a-")
            + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-"))
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (QDir(root).mkdir(name))
            task = QDir(root).filePath(name);
    }
    if (task.isEmpty())
        return fail(result, QStringLiteral("Could not create an exclusive translation task directory."), cancelled);
    result.taskDirectory = task;
    result.logPath = QDir(task).filePath(QStringLiteral("process.log"));
    const QString snapshot = QDir(task).filePath(QStringLiteral("generation_config.snapshot.json5"));
    if (!writeNewFile(snapshot, configBytes, &error))
        return fail(result, error, cancelled);
    if (QFileInfo::exists(latestLog)) {
        QFile previous(latestLog);
        if (!previous.open(QIODevice::ReadOnly))
            return fail(result, QStringLiteral("Could not back up the engine's previous latest.log: %1").arg(previous.errorString()), cancelled);
        const bool truncated = previous.size() > PreviousLogLimit;
        if (truncated && !previous.seek(previous.size() - PreviousLogLimit))
            return fail(result, previous.errorString(), cancelled);
        const QByteArray bytes = previous.read(PreviousLogLimit);
        if (previous.error() != QFileDevice::NoError
            || !writeNewFile(QDir(task).filePath(QStringLiteral("engine-latest.before.log")), bytes, &error))
            return fail(result, error.isEmpty() ? previous.errorString() : error, cancelled);
        if (truncated)
            result.warnings.append(QStringLiteral("The previous engine log backup contains its last 1 MiB."));
    }
    const QByteArray logPreamble = QByteArrayLiteral("[INFO] ChickenRice refreshes latest.log in its own engine directory. Its previous log was backed up when present; model and generation configuration are unchanged.\n");
    QStringList arguments{
        QStringLiteral("--model_name_or_path"), QDir(engine).filePath(QStringLiteral("models")),
        QStringLiteral("--device"), request.computeMode == ChickenRiceComputeMode::Cpu ? QStringLiteral("cpu") : QStringLiteral("cuda"),
        QStringLiteral("--task"), QStringLiteral("translate"),
        QStringLiteral("--audio_suffixes"), QFileInfo(audio.path).suffix().toLower(),
        QStringLiteral("--sub_formats"), QStringLiteral("srt"),
        QStringLiteral("--output_dir"), task,
        QStringLiteral("--generation_config"), snapshot,
        QStringLiteral("--log_level"), QStringLiteral("INFO")};
    if (request.computeMode == ChickenRiceComputeMode::GpuHighMemory)
        arguments.append({QStringLiteral("--enable_batching"), QStringLiteral("--max_batch_size"), QStringLiteral("8")});
    else
        arguments.append({QStringLiteral("--compute_type"), request.computeMode == ChickenRiceComputeMode::Cpu
            ? QStringLiteral("int8") : QStringLiteral("int8_float16")});
    arguments.append(chickenRiceGenerationArguments(request.generationOptions));
    if (testProcess)
        arguments.append(request.argumentsForTests);
    arguments.append(audio.path); // Exactly one absolute file; never pass a directory for recursive scanning.
    const QString expectedSrt = QDir(task).filePath(QFileInfo(audio.path).completeBaseName() + QStringLiteral(".srt"));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TRANSFORMERS_OFFLINE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("HF_HUB_DISABLE_TELEMETRY"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    QJsonArray argumentJson;
    for (const auto &argument : arguments)
        argumentJson.append(argument);
    const QJsonObject manifest{{QStringLiteral("program"), program}, {QStringLiteral("arguments"), argumentJson},
        {QStringLiteral("audioPath"), request.audioPath}, {QStringLiteral("workingDirectory"), engine},
        {QStringLiteral("offline"), true},
        {QStringLiteral("generation"), chickenRiceGenerationRecord(request.generationOptions)},
        {QStringLiteral("computeMode"), aiSubtitleComputeModeKey(request.computeMode)},
        {QStringLiteral("computeType"), request.computeMode == ChickenRiceComputeMode::GpuHighMemory ? QStringLiteral("component-default")
            : request.computeMode == ChickenRiceComputeMode::Cpu ? QStringLiteral("int8") : QStringLiteral("int8_float16")},
        {QStringLiteral("internalBatching"), request.computeMode == ChickenRiceComputeMode::GpuHighMemory},
        {QStringLiteral("maxBatchSize"), request.computeMode == ChickenRiceComputeMode::GpuHighMemory ? 8 : 0},
        {QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if (!writeNewFile(QDir(task).filePath(QStringLiteral("request.json")), QJsonDocument(manifest).toJson(), &error))
        return fail(result, error, cancelled);
    const auto execution = runAiSubtitleProcess({program, arguments, engine, environment, result.logPath,
        QStringLiteral("Running offline Japanese-to-Chinese translation"), false, logPreamble}, cancelled, report);
    result.exitCode = execution.exitCode;
    result.warnings.append(execution.warnings);
    if (!execution.succeeded)
        return fail(result, execution.error, cancelled);
    return validateAndPublishAiSubtitle({request.audioPath, request.libraryRoot, request.libraryRelativePath,
        expectedSrt, QStringLiteral("offline-direct"), QStringLiteral("zh"), QStringLiteral("offline_translate_"), QString::fromUtf8(execution.tail), initialAudioSize, initialAudioModified,
        request.outputBatchId, request.outputFileSuffix},
        result, cancelled, report);
}
} // namespace

struct ChickenRiceTranslateService::WorkerState {
    std::atomic_bool cancelled{false};
    QMutex mutex;
    Progress latest;
    bool changed = false;
};
ChickenRiceTranslateService::ChickenRiceTranslateService(QObject *parent, Executor executor)
    : QObject(parent), m_executor(executor ? std::move(executor) : Executor(translate)),
      m_progressTimer(new QTimer(this))
{
    qRegisterMetaType<Progress>();
    qRegisterMetaType<Result>();
    m_progressTimer->setInterval(75);
    connect(m_progressTimer, &QTimer::timeout, this, &ChickenRiceTranslateService::flushProgress);
}
ChickenRiceTranslateService::~ChickenRiceTranslateService() { cancel(); }
bool ChickenRiceTranslateService::start(const Request &request)
{
    if (m_busy || m_shuttingDown)
        return false;
    m_busy = true;
    m_result = {};
    m_progress = {State::Preparing, QStringLiteral("Preparing task and checking audio paths"), {}};
    m_worker = std::make_shared<WorkerState>();
    const auto worker = m_worker;
    const auto executor = m_executor;
    auto promise = std::make_shared<QPromise<Result>>();
    auto *watcher = new QFutureWatcher<Result>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher] {
        const QPointer<ChickenRiceTranslateService> guard(this);
        flushProgress();
        if (!guard) return;
        const Result completedResult = watcher->result();
        m_result = completedResult;
        m_progress.state = completedResult.state;
        m_progress.message = completedResult.error;
        m_progress.percent = completedResult.state == State::Succeeded ? 100 : -1;
        const Progress completedProgress = m_progress;
        m_busy = false;
        m_progressTimer->stop();
        m_worker.reset();
        m_watcher = nullptr;
        watcher->deleteLater();
        emit progressChanged(completedProgress);
        if (!guard) return;
        emit finished(completedResult);
        if (!guard) return;
        if (m_shuttingDown && !m_busy && !m_shutdownEmitted) {
            m_shutdownEmitted = true;
            emit shutdownFinished();
        }
    });
    watcher->setFuture(promise->future());
    m_progressTimer->start();
    QThreadPool::globalInstance()->start([promise, executor, worker, request] {
        promise->start();
        Result result;
        try {
            const QString generationError = isValidAiSubtitleComputeMode(request.computeMode)
                ? validateChickenRiceGenerationOptions(request.generationOptions) : QStringLiteral("Unknown inference execution mode.");
            if (worker->cancelled.load() || !generationError.isEmpty()) {
                result.state = worker->cancelled.load() ? State::Cancelled : State::Failed;
                result.error = generationError;
            } else {
                result = executor(request, worker->cancelled, [worker](const Progress &progress) {
                    QMutexLocker lock(&worker->mutex);
                    worker->latest = progress;
                    worker->changed = true;
                });
            }
        } catch (const std::exception &exception) {
            result.state = worker->cancelled.load() ? State::Cancelled : State::Failed;
            result.error = QString::fromUtf8(exception.what());
        } catch (...) {
            result.state = worker->cancelled.load() ? State::Cancelled : State::Failed;
            result.error = QStringLiteral("Unexpected local translation error.");
        }
        if (result.audioPath.isEmpty()) result.audioPath = request.audioPath;
        promise->addResult(std::move(result));
        promise->finish();
    });
    emit progressChanged(m_progress);
    return true;
}
void ChickenRiceTranslateService::cancel()
{
    if (m_worker) m_worker->cancelled.store(true);
}
void ChickenRiceTranslateService::shutdown()
{
    m_shuttingDown = true;
    cancel();
    if (!m_busy && !m_shutdownEmitted) {
        m_shutdownEmitted = true;
        emit shutdownFinished();
    }
}
bool ChickenRiceTranslateService::isBusy() const { return m_busy; }
ChickenRiceTranslateService::Progress ChickenRiceTranslateService::progress() const { return m_progress; }
ChickenRiceTranslateService::Result ChickenRiceTranslateService::result() const { return m_result; }
void ChickenRiceTranslateService::flushProgress()
{
    if (!m_worker) return;
    bool changed = false;
    {
        QMutexLocker lock(&m_worker->mutex);
        changed = m_worker->changed;
        if (changed) { m_progress = m_worker->latest; m_worker->changed = false; }
    }
    if (changed) emit progressChanged(m_progress);
}
} // namespace asmr
