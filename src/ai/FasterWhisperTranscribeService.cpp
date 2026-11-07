#include "ai/FasterWhisperTranscribeService.h"
#include "ai/FasterWhisperArchive.h"
#include "ai/FasterWhisperPackage.h"
#include "ai/AiSubtitleProcess.h"
#include "ai/AiSubtitleOutput.h"
#include "playback/AudioFileValidation.h"
#include "subtitle/SubtitleFileLoader.h"
#include <QCryptographicHash>
#include <QStringDecoder>
#include <cmath>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcessEnvironment>
#include <QPromise>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <exception>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace asmr {
namespace {
using Request = FasterWhisperTranscribeRequest;
using Result = FasterWhisperTranscribeResult;
using Progress = FasterWhisperTranscribeProgress;
using State = FasterWhisperTranscribeState;
using Report = std::function<void(const Progress &)>;
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
    result.error = cancelled.load() ? QStringLiteral("Recognition cancelled. Task logs were retained.") : error;
    return result;
}

// This recovery is restricted to the verified official r245.4 executable.
// It never changes the generic process result or accepts a parseable prefix.
bool completedCrashOutput(const QString &program, bool testProcess, const QString &task,
    const QString &audio, const QByteArray &tail, const std::atomic_bool &cancelled, QString *error)
{
    const auto reject = [error](const QString &message) { *error = message; return false; };
    if (cancelled.load()) return false;
    if (!testProcess) {
        QFile executable(program);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!safeAncestors(program) || !executable.open(QIODevice::ReadOnly) || !hash.addData(&executable)
            || hash.result().toHex() != QByteArray("cce4c27d0d7039c93b06c6f962374ea3e8b605dbb9bf564ad23db4c46f1a15c1"))
            return reject(QStringLiteral("退出异常恢复仅支持已核验的官方 r245.4 识别程序。"));
    }
    const QString log = QString::fromUtf8(tail);
    const QString written = QStringLiteral("Subtitles are written to '%1' directory.")
        .arg(QDir::toNativeSeparators(task));
    const qsizetype writtenAt = log.lastIndexOf(written);
    static const QRegularExpression finished(QStringLiteral(
        "(?:^|[\\r\\n])Operation finished in:\\s+[0-9]+:[0-9]{2}:[0-9]{2}(?:\\.[0-9]+)?[ \\t]*(?:[\\r\\n]|$)"));
    if (writtenAt < 0 || !finished.match(log, writtenAt + written.size()).hasMatch())
        return reject(QStringLiteral("缺少本次任务的字幕写出及操作完成记录，未恢复异常退出结果。"));

    const QString stem = QFileInfo(audio).completeBaseName();
    const QString srtPath = QDir(task).filePath(stem + QStringLiteral(".srt"));
    const QString jsonPath = QDir(task).filePath(stem + QStringLiteral(".json"));
    constexpr qint64 JsonLimit = 32 * 1024 * 1024;
    const QFileInfo jsonInfo(jsonPath);
    if (!safeAncestors(srtPath) || !safeAncestors(jsonPath) || !jsonInfo.isFile()
        || jsonInfo.size() <= 0 || jsonInfo.size() > JsonLimit)
        return reject(QStringLiteral("本次 JSON 结果缺失、过大或路径无效。"));
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
        return reject(QStringLiteral("无法读取本次 JSON 结果。"));
    const QByteArray bytes = file.read(JsonLimit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > JsonLimit || !file.atEnd())
        return reject(QStringLiteral("本次 JSON 结果读取不完整或超过大小上限。"));
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString jsonText = decoder(bytes);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (decoder.hasError() || jsonText.isEmpty() || parseError.error != QJsonParseError::NoError || !document.isObject())
        return reject(QStringLiteral("本次 JSON 结果不是完整有效的 UTF-8 JSON。"));
    const auto object = document.object();
    if (object.value(QStringLiteral("language")).toString() != QStringLiteral("ja")
        || !object.value(QStringLiteral("segments")).isArray())
        return reject(QStringLiteral("本次 JSON 结果缺少日语分段信息。"));
    const auto loaded = SubtitleFileLoader::load(srtPath, SubtitleFormat::Srt);
    if (cancelled.load()) return false;
    if (!loaded.succeeded())
        return reject(QStringLiteral("异常退出后的 SRT 未通过现有字幕解析校验。"));
    for (const auto &diagnostic : loaded.diagnostics)
        if (diagnostic.severity == SubtitleLoadDiagnosticSeverity::Error)
            return reject(QStringLiteral("异常退出后的 SRT 包含解析错误。"));
    const auto segments = object.value(QStringLiteral("segments")).toArray();
    if (segments.size() != loaded.entries.size())
        return reject(QStringLiteral("异常退出后的 SRT 与 JSON 分段数量不一致。"));
    qint64 previousStart = -1;
    for (qsizetype index = 0; index < segments.size(); ++index) {
        if (cancelled.load()) return false;
        const auto segment = segments.at(index).toObject();
        const auto &entry = loaded.entries.at(index);
        const auto startValue = segment.value(QStringLiteral("start"));
        const auto endValue = segment.value(QStringLiteral("end"));
        const double start = startValue.toDouble(-1);
        const double end = endValue.toDouble(-1);
        if (!startValue.isDouble() || !endValue.isDouble() || !std::isfinite(start) || !std::isfinite(end)
            || start < 0 || end <= start || entry.startMs < previousStart || entry.endMs <= entry.startMs
            || std::abs(start * 1000 - double(entry.startMs)) > 1.1
            || std::abs(end * 1000 - double(entry.endMs)) > 1.1
            || entry.text.trimmed().isEmpty() || !segment.value(QStringLiteral("text")).isString()
            || segment.value(QStringLiteral("text")).toString().simplified() != entry.text.simplified())
            return reject(QStringLiteral("异常退出后的 SRT 与 JSON 时间轴或字幕内容不一致。"));
        previousStart = entry.startMs;
    }
    return true;
}

Result transcribe(const Request &request, const std::atomic_bool &cancelled, const Report &report)
{
    Result result;
    result.audioPath = request.audioPath;
    result.mode = QStringLiteral("offline-asr-ja");
    result.language = QStringLiteral("ja");
    report({State::Preparing, QStringLiteral("Preparing task and checking audio paths"), {}});
    if (cancelled.load()) return fail(result, {}, cancelled);
    if ((!request.outputBatchId.isEmpty() && !validAiSubtitleOutputToken(request.outputBatchId))
        || (!request.outputFileSuffix.isEmpty() && !validAiSubtitleOutputToken(request.outputFileSuffix)))
        return fail(result, QStringLiteral("Invalid subtitle output batch or filename suffix."), cancelled);
    const bool libraryTrack = !request.libraryRoot.isEmpty() || !request.libraryRelativePath.isEmpty();
    if (libraryTrack && (request.libraryRoot.isEmpty() || request.libraryRelativePath.isEmpty()))
        return fail(result, QStringLiteral("A library track requires both its work root and relative path."), cancelled);
    const auto audio = libraryTrack ? validateLibraryAudioFile(request.libraryRoot, request.libraryRelativePath) : validateAudioFile(request.audioPath);
    if (!audio.success) return fail(result, audio.errorMessage, cancelled);
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
    QString error;
    const bool testProcess = !request.executableForTests.isEmpty();
    if (!testProcess && !request.modelFilesForTests.isEmpty())
        return fail(result, QStringLiteral("Test model files require an explicit test executable."), cancelled);
    const QString engine = QDir::cleanPath(QFileInfo(request.engineDirectory).absoluteFilePath());
    const QString model = QDir::cleanPath(QFileInfo(request.modelDirectory).absoluteFilePath());
    if (request.engineDirectory.isEmpty() || !QFileInfo(request.engineDirectory).isAbsolute()
        || !safeAncestors(engine) || !QFileInfo(engine).isDir())
        return fail(result, QStringLiteral("Engine directory must be an existing absolute directory without links."), cancelled);
    if (request.modelDirectory.isEmpty() || !QFileInfo(request.modelDirectory).isAbsolute()
        || !safeAncestors(model) || !QFileInfo(model).isDir())
        return fail(result, QStringLiteral("The local large-v2 model directory is missing or unsafe. Prepare components first."), cancelled);
    // r245.4 constructs <model_dir>/faster-whisper-<model>, even for unknown model IDs.
    // Pass a verified local leaf via that mapping; never pass an absolute path to --model.
    const QString modelLeaf = QFileInfo(model).fileName();
    const QString modelPrefix = QStringLiteral("faster-whisper-");
    if (!modelLeaf.startsWith(modelPrefix) || !QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]+$")).match(modelLeaf).hasMatch())
        return fail(result, QStringLiteral("XXL requires a model leaf named faster-whisper-<name>. Select the existing large-v2 installation; files will not be moved."), cancelled);
    const QString modelName = modelLeaf.mid(modelPrefix.size());
    if (modelName.isEmpty()) return fail(result, QStringLiteral("The local model directory name is invalid."), cancelled);
    if (!testProcess && !validateFasterWhisperEngine(engine, &error)) return fail(result, error, cancelled);
    if (!testProcess || !request.modelFilesForTests.isEmpty()) {
        // Every track retains the full model SHA-256 check. This percentage is
        // bytes hashed within this phase, never overall inference progress.
        report({State::CheckingModel, QStringLiteral("Verifying local model files (SHA-256)"), {}});
        const auto files = testProcess ? request.modelFilesForTests : fasterWhisperPackage().modelFiles;
        if (!validateFasterWhisperModel(model, files, cancelled, &error, [&](qint64 completed, qint64 total) {
                const int percent = total > 0 ? qBound(0, int(double(completed) / double(total) * 100), 100) : -1;
                report({State::CheckingModel, QStringLiteral("Verifying local model files (SHA-256)"), {}, percent});
            }))
            return fail(result, error, cancelled);
    }
    if (cancelled.load()) return fail(result, {}, cancelled);
    const QString program = testProcess ? request.executableForTests : QDir(engine).filePath(QStringLiteral("faster-whisper-xxl.exe"));
    if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isFile() || !safeAncestors(program))
        return fail(result, QStringLiteral("Recognition executable is missing or unsafe."), cancelled);
    if (!testProcess && !request.argumentsForTests.isEmpty())
        return fail(result, QStringLiteral("Test arguments require an explicit test executable."), cancelled);
    const QString root = QDir::cleanPath(QFileInfo(request.outputRoot).absoluteFilePath());
    if (request.outputRoot.isEmpty() || !QFileInfo(request.outputRoot).isAbsolute()
        || QDir(root).isRoot() || !safeAncestors(root)
        || (QFileInfo::exists(root) && !QFileInfo(root).isDir()))
        return fail(result, QStringLiteral("Output root must be an absolute, non-root directory without links."), cancelled);
    if (cancelled.load()) return fail(result, {}, cancelled);
    if (!QDir().mkpath(root)) return fail(result, QStringLiteral("Could not create recognition task storage."), cancelled);
    QString task;
    for (int attempt = 0; attempt < 3 && task.isEmpty(); ++attempt) {
        const QString name = QStringLiteral("asr-ja-") + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-"))
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (QDir(root).mkdir(name)) task = QDir(root).filePath(name);
    }
    if (task.isEmpty()) return fail(result, QStringLiteral("Could not create an exclusive recognition task directory."), cancelled);
    result.taskDirectory = task;
    result.logPath = QDir(task).filePath(QStringLiteral("process.log"));
    const QString temporaryDirectory = QDir(task).filePath(QStringLiteral("temp"));
    if (!QDir().mkdir(temporaryDirectory)) return fail(result, QStringLiteral("Could not create private engine temporary storage."), cancelled);
    QStringList arguments{audio.path,
        QStringLiteral("--model"), modelName,
        QStringLiteral("--model_dir"), QFileInfo(model).absolutePath(),
        QStringLiteral("--language"), QStringLiteral("ja"),
        QStringLiteral("--task"), QStringLiteral("transcribe"),
        QStringLiteral("--device"), request.computeMode == AiSubtitleComputeMode::Cpu ? QStringLiteral("cpu") : QStringLiteral("cuda"),
        QStringLiteral("--output_dir"), task, QStringLiteral("--print_progress")};
    arguments.append(fasterWhisperGenerationArguments(request.generationOptions, request.computeMode));
    if (testProcess) arguments.append(request.argumentsForTests);
    // This is a multi-value option: input and all other options must precede it.
    arguments.append({QStringLiteral("--output_format"), QStringLiteral("srt"), QStringLiteral("json")});
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : {QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("TRANSFORMERS_OFFLINE"),
             QStringLiteral("HF_HUB_DISABLE_TELEMETRY"), QStringLiteral("PYTHONUTF8"), QStringLiteral("PYTHONUNBUFFERED")})
        environment.insert(key, QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("TEMP"), QDir::toNativeSeparators(temporaryDirectory));
    environment.insert(QStringLiteral("TMP"), QDir::toNativeSeparators(temporaryDirectory));
    QJsonArray argumentJson;
    for (const auto &argument : arguments) argumentJson.append(argument);
    const auto package = fasterWhisperPackage();
    const QJsonObject manifest{{QStringLiteral("program"), program}, {QStringLiteral("arguments"), argumentJson},
        {QStringLiteral("audioPath"), request.audioPath}, {QStringLiteral("workingDirectory"), engine},
        {QStringLiteral("mode"), result.mode}, {QStringLiteral("language"), result.language}, {QStringLiteral("offline"), true},
        {QStringLiteral("computeMode"), aiSubtitleComputeModeKey(request.computeMode)},
        {QStringLiteral("expectedPackageVersion"), package.version}, {QStringLiteral("modelDirectory"), model},
        {QStringLiteral("modelRevision"), package.modelRevision}, {QStringLiteral("generation"), fasterWhisperGenerationRecord(request.generationOptions, request.computeMode)},
        {QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if (!writeNewFile(QDir(task).filePath(QStringLiteral("request.json")), QJsonDocument(manifest).toJson(), &error))
        return fail(result, error, cancelled);
    const auto execution = runAiSubtitleProcess({program, arguments, engine, environment, result.logPath,
        QStringLiteral("Loading local model / recognising Japanese speech"), true}, cancelled, report);
    result.exitCode = execution.exitCode;
    result.warnings.append(execution.warnings);
    bool recoveredCrash = false;
    QString processError = execution.error;
    if (!execution.succeeded && request.computeMode == AiSubtitleComputeMode::GpuLowMemory
        && result.exitCode == -1073740791 && execution.abnormalExitWithoutActiveWorkers && !cancelled.load()) {
        QString recoveryError;
        recoveredCrash = completedCrashOutput(program, testProcess, task, audio.path, execution.tail, cancelled, &recoveryError);
        if (!recoveredCrash && !recoveryError.isEmpty())
            processError += QLatin1Char('\n') + recoveryError;
    }
    if (!execution.succeeded && !recoveredCrash) {
        const bool effectiveVad = request.generationOptions.preset != FasterWhisperGenerationPreset::Custom
            || request.generationOptions.vadFilter;
        if (result.exitCode == -1073740791 && request.computeMode != AiSubtitleComputeMode::Cpu && effectiveVad)
            return fail(result, QStringLiteral("引擎异常退出（0xC0000409），可在生成参数→自定义中尝试关闭语音检测（VAD）后重试；原始结果与日志保留。")
                + QLatin1Char('\n') + processError, cancelled);
        return fail(result, processError, cancelled);
    }
    const QString expectedSrt = QDir(task).filePath(QFileInfo(audio.path).completeBaseName() + QStringLiteral(".srt"));
    auto published = validateAndPublishAiSubtitle({request.audioPath, request.libraryRoot, request.libraryRelativePath,
        expectedSrt, result.mode, result.language, QStringLiteral("offline_asr_"), QString::fromUtf8(execution.tail), initialAudioSize, initialAudioModified,
        request.outputBatchId, request.outputFileSuffix}, result, cancelled, report);
    if (recoveredCrash && published.state == State::Succeeded)
        published.warnings.append(QStringLiteral("字幕已生成并保存；识别引擎退出时发生异常（0xC0000409）。原始日志已保留。"));
    // Keep the common strict validator authoritative. This advice applies only
    // to its interval/text rejection, never to cancellation or publish failures.
    if (request.generationOptions.preset == FasterWhisperGenerationPreset::Custom
        && request.generationOptions.subtitleFormatting && published.state == State::Failed
        && published.error == QStringLiteral("Generated SRT contains an invalid subtitle interval or empty text."))
        published.error.prepend(QStringLiteral("生成字幕的时间轴或内容无效，可在生成参数→自定义中关闭“字幕断句与排版”后重试；原始结果与日志保留。\n"));
    return published;
}
} // namespace

struct FasterWhisperTranscribeService::WorkerState {
    std::atomic_bool cancelled{false};
    QMutex mutex;
    Progress latest;
    bool changed = false;
};
FasterWhisperTranscribeService::FasterWhisperTranscribeService(QObject *parent, Executor executor)
    : QObject(parent), m_executor(executor ? std::move(executor) : Executor(transcribe)),
      m_progressTimer(new QTimer(this))
{
    qRegisterMetaType<Progress>();
    qRegisterMetaType<Result>();
    m_progressTimer->setInterval(75);
    connect(m_progressTimer, &QTimer::timeout, this, &FasterWhisperTranscribeService::flushProgress);
}
FasterWhisperTranscribeService::~FasterWhisperTranscribeService() { cancel(); }
bool FasterWhisperTranscribeService::start(const Request &request)
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
        const QPointer<FasterWhisperTranscribeService> guard(this);
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
                ? validateFasterWhisperGenerationOptions(request.generationOptions) : QStringLiteral("Unknown inference execution mode.");
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
            result.error = QStringLiteral("Unexpected local recognition error.");
        }
        if (result.audioPath.isEmpty()) result.audioPath = request.audioPath;
        promise->addResult(std::move(result));
        promise->finish();
    });
    emit progressChanged(m_progress);
    return true;
}
void FasterWhisperTranscribeService::cancel()
{
    if (m_worker) m_worker->cancelled.store(true);
}
void FasterWhisperTranscribeService::shutdown()
{
    m_shuttingDown = true;
    cancel();
    if (!m_busy && !m_shutdownEmitted) {
        m_shutdownEmitted = true;
        emit shutdownFinished();
    }
}
bool FasterWhisperTranscribeService::isBusy() const { return m_busy; }
FasterWhisperTranscribeService::Progress FasterWhisperTranscribeService::progress() const { return m_progress; }
FasterWhisperTranscribeService::Result FasterWhisperTranscribeService::result() const { return m_result; }
void FasterWhisperTranscribeService::flushProgress()
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
