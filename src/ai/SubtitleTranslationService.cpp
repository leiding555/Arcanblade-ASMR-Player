#include "ai/SubtitleTranslationService.h"
#include "ai/SubtitleTranslationTools.h"
#include "ai/SubtitleTranslationPairPublication.h"
#include "library/SubtitleTranslationCleanup.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPromise>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace asmr {
namespace {
using State = AiSubtitleTaskState;
QString httpFailure(int status)
{
    if (status >= 300 && status < 400)
        return QStringLiteral("服务返回重定向；为保护 API Key，未继续请求。请使用最终服务地址。");
    switch (status) {
    case 400: case 422: return QStringLiteral("服务不接受翻译请求，请检查模型及兼容服务是否支持 Chat Completions JSON 输出。");
    case 401: return QStringLiteral("API Key 认证失败，请在翻译设置中检查密钥。");
    case 402: return QStringLiteral("服务账户余额不足，请检查账户余额。");
    case 403: return QStringLiteral("服务拒绝访问，请检查密钥和模型权限。");
    case 404: return QStringLiteral("未找到接口或模型，请检查翻译设置。");
    case 408: return QStringLiteral("服务请求超时；已保留完成的批次，可手动重试。");
    case 429: return QStringLiteral("服务请求达到限额；已停止请求，请稍后手动重试。");
    default: return status >= 500
        ? QStringLiteral("服务暂时不可用；已保留完成的批次，可手动重试。")
        : QStringLiteral("翻译失败，服务返回 HTTP %1。").arg(status);
    }
}
qint64 usageCount(const QJsonValue &value)
{
    const auto count = value.toInteger(-1);
    return count >= 0 && count <= 100000000 ? count : -1;
}
void addUsage(qint64 &total, qint64 count)
{
    if (count >= 0) total = (total < 0 ? 0 : total) + count;
}
bool containsSecret(const QJsonValue &value, const QString &secret)
{
    if (value.isString()) return value.toString().contains(secret);
    if (value.isArray()) {
        for (const auto &item : value.toArray()) if (containsSecret(item, secret)) return true;
    } else if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key().contains(secret) || containsSecret(it.value(), secret)) return true;
    }
    return false;
}
}
struct SubtitleTranslationService::Secret {
    QString value;
    ~Secret() { value.fill(QChar::Null); value.clear(); }
};

SubtitleTranslationService::SubtitleTranslationService(QObject *parent,
    std::shared_ptr<ApiKeyStore> keyStore, QNetworkAccessManager *manager)
    : QObject(parent), m_manager(manager ? manager : new QNetworkAccessManager),
      m_timeout(new QTimer(this)), m_keyStore(std::move(keyStore))
{
    Q_ASSERT(!m_manager->parent() && m_manager->thread() == thread());
    m_manager->setParent(this);
    qRegisterMetaType<SubtitleTranslationPlan>();
    qRegisterMetaType<SubtitleTranslationProgress>();
    qRegisterMetaType<SubtitleTranslationResult>();
    if (!m_keyStore) m_keyStore = std::make_shared<WindowsApiKeyStore>(
        QStringLiteral("Arcanblade/TranslationApi/") + QCoreApplication::organizationName()
            + QLatin1Char('/') + QCoreApplication::applicationName());
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (m_busy && !m_completing) {
            m_stopQueue = true;
            complete(State::Failed, QStringLiteral("翻译请求超时；已保留完成的批次，可手动重试。"));
        }
    });
}

SubtitleTranslationService::~SubtitleTranslationService()
{
    if (m_cancelled) m_cancelled->store(true);
    m_timeout->stop();
    clearReply();
    m_key.reset();
}

bool SubtitleTranslationService::prepare(const SubtitleTranslationRequest &request)
{
    if (m_busy) return false;
    ++m_generation;
    m_busy = true;
    m_prepared = false;
    m_completing = false;
    m_checkpointReady = false;
    m_stopQueue = false;
    m_checkpointLease.reset();
    m_plan = {};
    m_correctionPlan = {};
    m_plan.request = request;
    m_result = {};
    m_translations.clear();
    m_corrections.clear();
    m_contextRead = false;
    m_messages = {}; m_sourceRead = false; m_stalledTurns = 0;
    m_requestCount = 0;
    m_promptTokens = m_completionTokens = -1;
    m_peakContextTokens = -1;
    m_usageResponseCount = 0;
    m_currentBatch = -1;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    QPointer<SubtitleTranslationService> guard(this);
    emit busyChanged(true);
    if (!guard || m_completing || m_cancelled->load()) return true;
    emitProgress(State::Preparing, QStringLiteral("正在本地读取并检查字幕…"));
    if (!guard || m_completing || m_cancelled->load()) return true;
    const auto cancelled = m_cancelled;
    runWorker(WorkStage::Prepare, [request, cancelled] {
        WorkResult value;
        value.plan = prepareSubtitleTranslation(request, *cancelled);
        return value;
    });
    return true;
}

bool SubtitleTranslationService::startPrepared()
{
    if (m_busy || !m_prepared || !m_plan.succeeded()) return false;
    ++m_generation;
    m_busy = true;
    m_completing = false;
    m_checkpointReady = false;
    m_stopQueue = false;
    m_checkpointLease.reset();
    m_result = {};
    m_translations.clear();
    m_corrections.clear();
    m_contextRead = false;
    if (m_plan.request.useWorkContext) m_plan = m_correctionPlan;
    m_messages = {}; m_sourceRead = false; m_stalledTurns = 0;
    m_requestCount = 0;
    m_promptTokens = m_completionTokens = -1;
    m_peakContextTokens = -1;
    m_usageResponseCount = 0;
    m_currentBatch = -1;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    QPointer<SubtitleTranslationService> guard(this);
    emit busyChanged(true);
    if (!guard || m_completing || m_cancelled->load()) return true;
    emitProgress(State::Preparing, QStringLiteral("正在核对源字幕、恢复已完成批次并读取安全凭据…"));
    if (!guard || m_completing || m_cancelled->load()) return true;
    const auto plan = m_plan;
    const auto cancelled = m_cancelled;
    const auto store = m_keyStore;
    runWorker(WorkStage::Start, [plan, cancelled, store] {
        WorkResult value;
        value.plan = plan;
        if (!revalidateSubtitleTranslationSource(plan, *cancelled, &value.error)) {
            value.invalidatedSource = !cancelled->load();
            return value;
        }
        if (cancelled->load()) return value;
        const auto cleanup = cleanupMissingSubtitleTranslationResults(plan.request.databasePath, {}, plan.fingerprint);
        if (!cleanup.succeeded()) { value.error = cleanup.error; return value; }
        if (cancelled->load()) return value;
        value.lease = acquireSubtitleTranslationCheckpointLease(plan, &value.error);
        if (!value.lease || cancelled->load()) return value;
        if (cancelled->load()
            || !loadSubtitleTranslationCheckpoint(plan, &value.translations, &value.error))
            return value;
        // Verify durable checkpoint storage before the first potentially paid
        // request. This writes only our own empty/already verified record.
        if (!saveSubtitleTranslationCheckpoint(plan, value.translations, *cancelled, &value.error)) return value;
        value.checkpointReady = true;
        if (plan.request.useWorkContext && value.translations.size() == plan.entries.size()) {
            value.corrections = value.translations;
            const auto translated = makeSubtitleTranslationPhasePlan(plan, value.corrections, &value.error);
            if (!value.error.isEmpty() || !translated.succeeded()) return value;
            SubtitleTranslations saved;
            if (!loadSubtitleTranslationCheckpoint(translated, &saved, &value.error)
                || !saveSubtitleTranslationCheckpoint(translated, saved, *cancelled, &value.error)) return value;
            value.plan = translated;
            value.translations = std::move(saved);
        }
        if (!updateSubtitleTranslationCheckpointState(value.plan, QStringLiteral("interrupted"), -1, &value.error)
            || cancelled->load() || value.translations.size() == value.plan.entries.size()) return value;
        auto secret = std::make_shared<Secret>();
        if (!store->read(translationCredentialScope(plan.request.settings), &secret->value, &value.error)) {
            value.stopQueue = true;
            // A third-party/injected store may return text containing input.
            value.error = QStringLiteral("无法读取安全保存的 API Key，请在翻译设置中检查凭据。");
            return value;
        }
        if (cancelled->load()) return value;
        if (!validateTranslationApiKey(secret->value).isEmpty()) {
            value.stopQueue = true;
            value.error = QStringLiteral("当前服务尚未保存有效的 API Key，请先打开翻译设置。");
            return value;
        }
        value.key = std::move(secret);
        return value;
    });
    return true;
}

void SubtitleTranslationService::runWorker(WorkStage stage, std::function<WorkResult()> operation)
{
    auto promise = std::make_shared<QPromise<WorkResult>>();
    promise->start();
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    m_worker = watcher;
    const quint64 generation = m_generation;
    connect(watcher, &QFutureWatcher<WorkResult>::finished, this, [this, watcher, stage, generation] {
        WorkResult value = watcher->future().takeResult();
        if (m_worker == watcher) m_worker = nullptr;
        watcher->deleteLater();
        if (generation != m_generation || !m_busy || (m_completing && stage != WorkStage::Finish)) return;
        workerFinished(stage, std::move(value), generation);
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, operation = std::move(operation)]() mutable {
        WorkResult value;
        try { value = operation(); }
        catch (...) { value.error = QStringLiteral("字幕翻译后台操作失败；未发送额外请求，请手动重试。"); }
        promise->addResult(std::move(value));
        promise->finish();
    });
}

void SubtitleTranslationService::workerFinished(WorkStage stage, WorkResult value, quint64 generation)
{
    if (!m_busy || (m_completing && stage != WorkStage::Finish) || generation != m_generation) return;
    if (stage == WorkStage::ToolTurn) {
        recordUsage(value.promptTokens, value.completionTokens);
    }
    if (stage == WorkStage::Finish) {
        if (value.result.task.mode.isEmpty()) value.result = m_result;
        if (!value.error.isEmpty()) {
            if (value.result.task.state == State::Succeeded)
                value.result.task.warnings.append(QStringLiteral("字幕已保存，但数据库终态未写入：") + value.error);
            else {
                if (!value.result.task.error.isEmpty()) value.result.task.error += QLatin1Char('\n');
                value.result.task.error += QStringLiteral("数据库断点状态保存失败：") + value.error;
            }
        }
        notifyCompleted(std::move(value.result));
        return;
    }
    if (stage == WorkStage::Start || stage == WorkStage::AdvancePhase) {
        if (stage == WorkStage::Start) m_checkpointLease = std::move(value.lease);
        m_checkpointReady = value.checkpointReady;
        if (value.checkpointReady) {
            m_plan = value.plan;
            m_translations = value.translations;
            m_corrections = value.corrections;
        }
    }
    // The publisher's successful rename is a commit point, even if cancel
    // arrives immediately afterwards. Never hide an already complete result.
    if (stage == WorkStage::Publish && value.task.state == State::Succeeded) {
        SubtitleTranslationResult result;
        result.task = std::move(value.task);
        complete(std::move(result));
        return;
    }
    if (stage == WorkStage::Start && value.invalidatedSource)
        m_prepared = false;
    // A committed checkpoint remains complete if cancellation arrives before
    // this queued watcher callback. Reflect that saved progress accurately.
    if ((stage == WorkStage::Save && value.error.isEmpty())
        || (stage == WorkStage::ToolTurn && value.progressAvailable))
        m_translations = value.translations;
    if (m_cancelled->load()) { complete(State::Cancelled); return; }
    if (!value.error.isEmpty()) { m_stopQueue = value.stopQueue; complete(State::Failed, value.error); return; }
    if (stage == WorkStage::ToolRequest) {
        m_messages = std::move(value.messages);
        postRequest(value.requestBody);
        return;
    }
    if (stage == WorkStage::ToolTurn) {
        const bool initialized = (!m_sourceRead && value.sourceRead)
            || (!m_contextRead && value.contextRead);
        m_sourceRead = value.sourceRead;
        m_contextRead = value.contextRead;
        m_messages = std::move(value.messages);
        m_stalledTurns = value.madeProgress || initialized ? 0 : m_stalledTurns + 1;
        if (m_stalledTurns >= 4) {
            complete(State::Failed, QStringLiteral("模型连续 4 轮没有新增有效字幕，已停止；可手动继续未完成部分。"));
            return;
        }
        QPointer<SubtitleTranslationService> guard(this);
        emitProgress(State::Running, QStringLiteral("%1：已保存 %2/%3 条。")
            .arg(m_plan.phase == SubtitleTranslationPhase::Correction ? QStringLiteral("日语校正") : QStringLiteral("中文翻译"))
            .arg(m_translations.size()).arg(m_plan.entries.size()));
        if (guard && m_busy && !m_completing && !m_cancelled->load()) sendNext();
        return;
    }
    if (stage == WorkStage::Prepare) {
        m_plan = std::move(value.plan);
        if (!m_plan.succeeded()) { complete(State::Failed, m_plan.error); return; }
        if (m_plan.request.useWorkContext) m_correctionPlan = m_plan;
        m_prepared = true;
        m_busy = false;
        QPointer<SubtitleTranslationService> guard(this);
        emit prepared(m_plan);
        if (guard && !m_busy && generation == m_generation) emit busyChanged(false);
        return;
    }
    if (stage == WorkStage::Start || stage == WorkStage::Save || stage == WorkStage::AdvancePhase) {
        m_translations = std::move(value.translations);
        if (stage == WorkStage::Start) m_key = std::move(value.key);
        if (stage == WorkStage::AdvancePhase) {
            m_sourceRead = false;
            m_stalledTurns = 0;
        }
        sendNext();
        return;
    }
    if (stage == WorkStage::BeginBatch) {
        postCurrentBatch();
        return;
    }
    if (stage == WorkStage::Publish)
        complete(value.task.state == State::Cancelled ? State::Cancelled : State::Failed,
            value.task.error.isEmpty() ? QStringLiteral("中文字幕保存失败；已完成批次仍保留，可手动重试保存。") : value.task.error);
}

void SubtitleTranslationService::sendNext()
{
    if (!m_busy || m_completing || m_worker) return;
    if (m_cancelled->load()) { complete(State::Cancelled); return; }
    m_currentBatch = -1;
    if (m_plan.request.useWorkContext && m_plan.phase == SubtitleTranslationPhase::Correction
        && m_translations.size() == m_plan.entries.size()) {
        advanceContextPhase();
        return;
    }
    if (m_plan.mode == SubtitleTranslationMode::ToolConversation
        && m_translations.size() < m_plan.entries.size()) {
        buildToolRequest();
        return;
    }
    for (int index = 0; index < m_plan.batches.size(); ++index) {
        const auto batch = m_plan.batches[index];
        bool finished = true;
        for (int id = batch.firstId; id < batch.firstId + batch.count; ++id)
            if (!m_translations.contains(id)) { finished = false; break; }
        if (!finished) { m_currentBatch = index; break; }
    }
    if (m_currentBatch < 0) {
        QPointer<SubtitleTranslationService> guard(this);
        emitProgress(State::Validating, m_plan.request.useWorkContext
            ? QStringLiteral("正在校验时间轴并保存日语校正稿和中文字幕…")
            : QStringLiteral("正在校验时间轴并独立保存中文字幕…"));
        if (!guard || m_completing || m_cancelled->load()) return;
        const auto plan = m_plan;
        const auto translations = m_translations;
        const auto cancelled = m_cancelled;
        const auto lease = m_checkpointLease;
        const auto correctionPlan = m_correctionPlan;
        const auto corrections = m_corrections;
        m_key.reset();
        runWorker(WorkStage::Publish, [plan, translations, cancelled, lease, correctionPlan, corrections] {
            WorkResult value;
            value.lease = lease;
            value.task = plan.request.useWorkContext
                ? publishSubtitleTranslationPair(correctionPlan, corrections, plan, translations, *cancelled)
                : publishSubtitleTranslation(plan, translations, *cancelled);
            return value;
        });
        return;
    }
    if (!m_key) { complete(State::Failed, QStringLiteral("未能读取有效 API Key，请检查翻译设置后重试。")); return; }
    const auto plan = m_plan;
    const auto lease = m_checkpointLease;
    const int batchIndex = m_currentBatch;
    runWorker(WorkStage::BeginBatch, [plan, lease, batchIndex] {
        WorkResult value;
        value.lease = lease;
        if (!updateSubtitleTranslationCheckpointState(plan, QStringLiteral("running"), batchIndex, &value.error)
            && value.error.isEmpty()) value.error = QStringLiteral("无法保存当前批次状态，未发送翻译请求。");
        return value;
    });
}

void SubtitleTranslationService::advanceContextPhase()
{
    const auto correctionPlan = m_correctionPlan;
    const auto corrections = m_translations;
    const auto cancelled = m_cancelled;
    const auto lease = m_checkpointLease;
    runWorker(WorkStage::AdvancePhase, [correctionPlan, corrections, cancelled, lease] {
        WorkResult value;
        value.lease = lease;
        value.plan = correctionPlan;
        value.corrections = corrections;
        if (!revalidateSubtitleTranslationSource(correctionPlan, *cancelled, &value.error)) return value;
        const auto plan = makeSubtitleTranslationPhasePlan(correctionPlan, corrections, &value.error);
        if (!value.error.isEmpty() || !plan.succeeded()) return value;
        if (!loadSubtitleTranslationCheckpoint(plan, &value.translations, &value.error)
            || !saveSubtitleTranslationCheckpoint(plan, value.translations, *cancelled, &value.error)) return value;
        value.plan = plan;
        value.checkpointReady = true;
        if (!updateSubtitleTranslationCheckpointState(plan, QStringLiteral("interrupted"), -1, &value.error)) return value;
        return value;
    });
}

void SubtitleTranslationService::buildToolRequest()
{
    if (!m_key) { m_stopQueue = true; complete(State::Failed, QStringLiteral("无法读取有效 API Key，请检查翻译设置。")); return; }
    const auto plan = m_plan;
    const auto lease = m_checkpointLease;
    const auto cancelled = m_cancelled;
    const auto messages = m_messages;
    runWorker(WorkStage::ToolRequest, [plan, lease, cancelled, messages] {
        WorkResult value; value.lease = lease;
        if (cancelled->load()) return value;
        if (plan.request.useWorkContext && !revalidateSubtitleTranslationSource(plan, *cancelled, &value.error)) return value;
        if (!updateSubtitleTranslationCheckpointState(plan, QStringLiteral("running"), -1, &value.error)) return value;
        value.messages = messages.isEmpty() ? subtitleTranslationInitialMessages(plan) : messages;
        value.requestBody = subtitleTranslationToolRequest(plan, value.messages);
        if (value.requestBody.isEmpty() || value.requestBody.size() > SubtitleTranslationMaximumRequestBytes)
            value.error = QStringLiteral("完整对话请求超过 16 MiB 本地上限，已保留译文；未截断历史或发送请求。");
        return value;
    });
}

void SubtitleTranslationService::processToolResponse(const QByteArray &response)
{
    const auto plan = m_plan;
    const auto lease = m_checkpointLease;
    const auto cancelled = m_cancelled;
    const auto key = m_key;
    const auto messages = m_messages;
    const auto confirmed = m_translations;
    const bool sourceRead = m_sourceRead;
    const bool contextRead = m_contextRead;
    runWorker(WorkStage::ToolTurn, [plan, lease, cancelled, key, messages, confirmed, sourceRead, contextRead, response] {
        WorkResult value;
        value.lease = lease; value.messages = messages;
        value.translations = confirmed; value.sourceRead = sourceRead; value.progressAvailable = true;
        value.contextRead = contextRead;
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(response, &parseError);
        const auto root = document.object();
        const auto usage = root.value(QStringLiteral("usage")).toObject();
        value.promptTokens = usageCount(usage.value(QStringLiteral("prompt_tokens")));
        value.completionTokens = usageCount(usage.value(QStringLiteral("completion_tokens")));
        const auto choices = root.value(QStringLiteral("choices")).toArray();
        const auto choice = choices.size() == 1 ? choices.first().toObject() : QJsonObject{};
        const auto assistant = choice.value(QStringLiteral("message")).toObject();
        const auto finish = choice.value(QStringLiteral("finish_reason")).toString();
        const auto content = assistant.value(QStringLiteral("content"));
        const auto reasoning = assistant.value(QStringLiteral("reasoning_content"));
        const auto callValue = assistant.value(QStringLiteral("tool_calls"));
        const auto calls = callValue.toArray();
        if (parseError.error != QJsonParseError::NoError || !document.isObject() || choices.size() != 1
            || assistant.value(QStringLiteral("role")).toString() != QStringLiteral("assistant")
            || (!content.isString() && !content.isNull() && !content.isUndefined())
            || (!reasoning.isString() && !reasoning.isNull() && !reasoning.isUndefined())
            || (!callValue.isArray() && !callValue.isNull() && !callValue.isUndefined())
            || (calls.isEmpty() ? finish != QStringLiteral("stop") : finish != QStringLiteral("tool_calls"))) {
            value.error = finish == QStringLiteral("length")
                ? QStringLiteral("模型输出被截断，未执行本轮工具；已保存译文保留，可手动重试。")
                : QStringLiteral("模型未返回完整有效的工具调用协议，已停止并保留译文。");
            return value;
        }
        if (key && containsSecret(root, key->value)) {
            value.error = QStringLiteral("服务响应包含安全凭据内容，已停止保存；请检查当前服务。");
            return value;
        }
        QSet<QString> identifiers;
        for (const auto &entry : messages) {
            for (const auto &call : entry.toObject().value(QStringLiteral("tool_calls")).toArray())
                identifiers.insert(call.toObject().value(QStringLiteral("id")).toString());
        }
        // Validate the entire call envelope before any side effect. Invalid arguments
        // are recoverable tool errors; missing/mismatched call IDs are protocol failures.
        for (const auto &entry : calls) {
            const auto call = entry.toObject();
            const auto id = call.value(QStringLiteral("id"));
            const auto function = call.value(QStringLiteral("function")).toObject();
            if (!entry.isObject() || !id.isString() || id.toString().trimmed().isEmpty()
                || identifiers.contains(id.toString()) || call.value(QStringLiteral("type")).toString() != QStringLiteral("function")
                || !function.value(QStringLiteral("name")).isString()
                || !function.value(QStringLiteral("arguments")).isString()) {
                value.error = QStringLiteral("工具调用 ID、类型或函数结构无效，未执行本轮工具。");
                return value;
            }
            identifiers.insert(id.toString());
        }
        value.messages.append(assistant);
        if (calls.isEmpty()) {
            auto feedback = subtitleTranslationToolFeedback(plan, value.translations, QStringLiteral("error"),
                QStringLiteral("CALL_TOOL"), plan.request.useWorkContext && plan.contextKind == SubtitleWorkContextKind::Asr
                    ? QStringLiteral("字幕尚未完成。先调用 read_work_asr_subtitles 和 read_subtitle_translation_state 读取完整上下文，再用 write_subtitle_translations 提交当前阶段的连续结果。")
                    : QStringLiteral("字幕尚未完成。先调用 read_subtitle_translation_state 读取全文，再用 write_subtitle_translations 提交连续译文。"));
            value.messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(feedback).toJson(QJsonDocument::Compact))}});
        }
        for (const auto &entry : calls) {
            if (cancelled->load()) return value;
            const auto call = entry.toObject();
            const auto function = call.value(QStringLiteral("function")).toObject();
            const auto name = function.value(QStringLiteral("name")).toString();
            const auto arguments = function.value(QStringLiteral("arguments")).toString().toUtf8();
            QJsonObject feedback;
            if (name == QStringLiteral("read_subtitle_translation_state")
                || (plan.request.useWorkContext && name == QString::fromLatin1(subtitleTranslationContextToolName(plan)))) {
                QJsonParseError error;
                const auto args = QJsonDocument::fromJson(arguments, &error);
                if (error.error != QJsonParseError::NoError || !args.isObject() || !args.object().isEmpty())
                    feedback = subtitleTranslationToolFeedback(plan, value.translations, QStringLiteral("error"),
                        QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("读取工具不接受参数，请使用空对象 {}。"));
                else if (name == QString::fromLatin1(subtitleTranslationContextToolName(plan))) {
                    feedback = subtitleTranslationToolFeedback(plan, value.translations);
                    const auto context = plan.contextKind == SubtitleWorkContextKind::Asr
                        ? subtitleTranslationReadWorkAsrResult(plan, !value.contextRead)
                        : subtitleTranslationReadWorkReferencesResult(plan, !value.contextRead);
                    for (auto it = context.begin(); it != context.end(); ++it) feedback.insert(it.key(), it.value());
                    if (context.value(QStringLiteral("status")).toString() == QStringLiteral("ok"))
                        value.contextRead = true;
                } else {
                    feedback = subtitleTranslationReadToolResult(plan, value.translations, !value.sourceRead);
                    value.sourceRead = true;
                }
            } else if (name == QStringLiteral("write_subtitle_translations")) {
                if (plan.request.useWorkContext && !contextRead) {
                    feedback = subtitleTranslationToolFeedback(plan, value.translations, QStringLiteral("error"),
                        plan.contextKind == SubtitleWorkContextKind::Asr
                            ? QStringLiteral("ASR_READ_REQUIRED") : QStringLiteral("REFERENCES_READ_REQUIRED"),
                        plan.contextKind == SubtitleWorkContextKind::Asr
                            ? QStringLiteral("请先调用 read_work_asr_subtitles({}) 获取本次作品 ASR 上下文，并在收到工具结果后提交字幕。")
                            : QStringLiteral("请先调用 read_work_references({}) 获取本次作品参考资料，并在收到工具结果后提交字幕。"));
                    value.messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                        {QStringLiteral("tool_call_id"), call.value(QStringLiteral("id"))},
                        {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(feedback).toJson(QJsonDocument::Compact))}});
                    continue;
                }
                // Reading earlier in this same assistant response does not mean
                // the model has received that tool result yet. Only a prior turn qualifies.
                const auto write = parseSubtitleTranslationToolWrite(plan, value.translations, arguments, sourceRead);
                if (!write.succeeded())
                    feedback = subtitleTranslationToolFeedback(plan, value.translations, QStringLiteral("error"), write.code, write.error);
                else {
                    auto combined = value.translations;
                    for (auto it = write.translations.cbegin(); it != write.translations.cend(); ++it) {
                        if (key && it.value().contains(key->value)) {
                            value.error = QStringLiteral("工具译文包含安全凭据内容，已停止保存。"); return value;
                        }
                        combined.insert(it.key(), it.value());
                    }
                    if (!write.duplicate) {
                        if (plan.request.useWorkContext && !revalidateSubtitleTranslationSource(plan, *cancelled, &value.error)) return value;
                        if (!saveSubtitleTranslationCheckpoint(plan, combined, *cancelled, &value.error)) {
                            if (value.error.isEmpty()) value.error = QStringLiteral("无法保存译文断点，已停止后续请求。");
                            return value;
                        }
                        value.translations = std::move(combined);
                        value.madeProgress = true;
                    }
                    feedback = subtitleTranslationToolFeedback(plan, value.translations);
                    feedback.insert(QStringLiteral("written_count"), write.duplicate ? 0 : write.translations.size());
                }
            } else {
                feedback = subtitleTranslationToolFeedback(plan, value.translations, QStringLiteral("error"),
                    QStringLiteral("UNKNOWN_TOOL"), plan.request.useWorkContext
                        ? QStringLiteral("仅支持 %1、read_subtitle_translation_state 和 write_subtitle_translations。")
                            .arg(QString::fromLatin1(subtitleTranslationContextToolName(plan)))
                        : QStringLiteral("仅支持 read_subtitle_translation_state 和 write_subtitle_translations。"));
            }
            value.messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                {QStringLiteral("tool_call_id"), call.value(QStringLiteral("id"))},
                {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(feedback).toJson(QJsonDocument::Compact))}});
        }
        return value;
    });
}

void SubtitleTranslationService::postCurrentBatch()
{
    if (!m_busy || m_completing || m_worker) return;
    if (m_cancelled->load()) { complete(State::Cancelled); return; }
    const QByteArray body = subtitleTranslationBatchRequest(m_plan, m_currentBatch);
    if (body.isEmpty()) { complete(State::Failed, QStringLiteral("无法创建当前字幕批次请求。")); return; }
    postRequest(body);
}

void SubtitleTranslationService::postRequest(const QByteArray &body)
{
    if (!m_busy || m_completing || m_worker || m_cancelled->load()) return;
    QNetworkRequest request(translationChatCompletionsUrl(m_plan.request.settings));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_key->value.toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    m_response.clear();
    m_maximumResponseBytes = translationApiThinkingEnabled(m_plan.request.settings) ? 512 * 1024 : 128 * 1024;
    const quint64 generation = m_generation;
    auto *reply = m_manager->post(request, body);
    m_reply = reply;
    ++m_requestCount;
    reply->setReadBufferSize(m_maximumResponseBytes + 1);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, generation] {
        if (!m_busy || m_completing || m_reply != reply || generation != m_generation) return;
        bool ok = false;
        const auto size = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
        if (ok && size > m_maximumResponseBytes)
            complete(State::Failed, QStringLiteral("翻译服务响应过大，已停止接收；可手动重试当前批次。"));
    });
    connect(reply, &QIODevice::readyRead, this, [this, reply, generation] { readAvailable(reply, generation); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] { replyFinished(reply, generation); });
    m_timeout->start(translationApiRequestTimeoutSeconds(m_plan.request.settings) * 1000);
    emitProgress(State::Running, m_plan.mode == SubtitleTranslationMode::ToolConversation
        ? (m_plan.request.useWorkContext && m_plan.phase == SubtitleTranslationPhase::Correction
            ? QStringLiteral("日语校正：%1/%2 条，正在读取资料或校正字幕…").arg(m_translations.size()).arg(m_plan.entries.size())
            : (m_sourceRead ? QStringLiteral("模型正在理解上下文并翻译…") : QStringLiteral("模型正在读取完整字幕…")))
        : QStringLiteral("已完成 %1/%2 条；正在翻译第 %3/%4 批…")
            .arg(m_translations.size()).arg(m_plan.entries.size()).arg(m_currentBatch + 1).arg(m_plan.batches.size()));
}

void SubtitleTranslationService::readAvailable(QNetworkReply *reply, quint64 generation)
{
    if (!m_busy || m_completing || m_reply != reply || generation != m_generation) return;
    m_response += reply->read(m_maximumResponseBytes - m_response.size() + 1);
    if (m_response.size() > m_maximumResponseBytes)
        complete(State::Failed, QStringLiteral("翻译服务响应过大，已停止接收；可手动重试当前批次。"));
}

void SubtitleTranslationService::replyFinished(QNetworkReply *reply, quint64 generation)
{
    if (!m_busy || m_completing || m_reply != reply || generation != m_generation) return;
    QPointer<SubtitleTranslationService> guard(this);
    readAvailable(reply, generation);
    if (!guard || !m_busy || m_completing || m_reply != reply || generation != m_generation) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300) { m_stopQueue = true; complete(State::Failed, httpFailure(status)); return; }
    if (reply->error() != QNetworkReply::NoError || status < 200) {
        m_stopQueue = true;
        complete(State::Failed, QStringLiteral("无法连接翻译服务，请检查网络、代理或证书；已完成批次仍保留。"));
        return;
    }
    if (m_plan.mode == SubtitleTranslationMode::ToolConversation) {
        const auto response = m_response;
        clearReply();
        processToolResponse(response);
        return;
    }
    const auto document = QJsonDocument::fromJson(m_response);
    const auto choices = document.object().value(QStringLiteral("choices")).toArray();
    const auto first = choices.size() == 1 ? choices.first().toObject() : QJsonObject();
    const auto content = first.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    const auto reason = first.value(QStringLiteral("finish_reason"));
    const auto usage = document.object().value(QStringLiteral("usage")).toObject();
    recordUsage(usageCount(usage.value(QStringLiteral("prompt_tokens"))),
        usageCount(usage.value(QStringLiteral("completion_tokens"))));
    if (m_key && content.isString() && content.toString().contains(m_key->value)) {
        complete(State::Failed, QStringLiteral("服务响应包含安全凭据内容，已停止保存；请检查当前服务。"));
        return;
    }
    if (!document.isObject() || !content.isString() || content.toString().trimmed().isEmpty()
        || (!reason.isUndefined() && !reason.isNull()
            && (!reason.isString() || reason.toString() != QStringLiteral("stop")))) {
        complete(State::Failed, reason.toString() == QStringLiteral("length")
            ? QStringLiteral("服务响应被截断，当前批次未保存；可手动重试。")
            : reason.toString() == QStringLiteral("content_filter")
            ? QStringLiteral("服务未返回当前批次译文；请检查服务限制。")
            : QStringLiteral("服务未返回完整的 Chat Completions JSON 译文，当前批次未保存。"));
        return;
    }
    SubtitleTranslations batch;
    QString error;
    if (!parseSubtitleTranslationBatch(m_plan, m_currentBatch, content.toString().toUtf8(), &batch, &error)) {
        complete(State::Failed, error.isEmpty() ? QStringLiteral("字幕批次校验失败，当前批次未保存。") : error);
        return;
    }
    auto translations = m_translations;
    for (auto it = batch.cbegin(); it != batch.cend(); ++it) {
        // Check decoded values too: inner JSON may spell a credential with
        // Unicode escapes that were absent from its raw content string.
        if (m_key && it.value().contains(m_key->value)) {
            complete(State::Failed, QStringLiteral("服务响应包含安全凭据内容，已停止保存；请检查当前服务。"));
            return;
        }
        translations.insert(it.key(), it.value());
    }
    clearReply();
    const auto plan = m_plan;
    const auto cancelled = m_cancelled;
    const auto lease = m_checkpointLease;
    runWorker(WorkStage::Save, [plan, translations = std::move(translations), cancelled, lease] {
        WorkResult value;
        value.lease = lease;
        if (saveSubtitleTranslationCheckpoint(plan, translations, *cancelled, &value.error))
            value.translations = translations;
        else if (value.error.isEmpty())
            value.error = QStringLiteral("无法保存翻译断点，已停止后续请求。");
        return value;
    });
}

void SubtitleTranslationService::cancel()
{
    if (!m_busy || m_completing) return;
    if (m_cancelled) m_cancelled->store(true);
    clearReply();
    // Let the value-only worker finish before notifying the owner. This keeps
    // shutdown responsive without permitting an old credential read to send.
    if (!m_worker) complete(State::Cancelled);
}

void SubtitleTranslationService::clearReply()
{
    m_timeout->stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        if (!reply->isFinished()) reply->abort();
        reply->deleteLater();
    }
    m_response.fill('\0');
    m_response.clear();
}

void SubtitleTranslationService::emitProgress(State state, const QString &message)
{
    SubtitleTranslationProgress value;
    value.state = state;
    value.message = message;
    value.completedEntries = m_translations.size();
    value.totalEntries = m_plan.entries.size();
    value.phase = m_plan.phase;
    value.phaseCompletedEntries = m_translations.size();
    value.phaseTotalEntries = m_plan.entries.size();
    if (m_plan.request.useWorkContext) {
        value.completedEntries += m_corrections.size();
        value.totalEntries *= 2;
    }
    value.batchIndex = m_currentBatch < 0 ? 0 : m_currentBatch + 1;
    value.batchCount = static_cast<int>(m_plan.batches.size());
    value.requestCount = m_requestCount;
    value.promptTokens = m_promptTokens;
    value.completionTokens = m_completionTokens;
    value.peakContextTokens = m_peakContextTokens;
    value.contextTokenLimit = translationApiContextTokenLimit(m_plan.request.settings);
    value.usageComplete = m_usageResponseCount == m_requestCount;
    emit progress(value);
}

void SubtitleTranslationService::recordUsage(qint64 promptTokens, qint64 completionTokens)
{
    addUsage(m_promptTokens, promptTokens);
    addUsage(m_completionTokens, completionTokens);
    if (promptTokens >= 0 && completionTokens >= 0) {
        ++m_usageResponseCount;
        m_peakContextTokens = std::max(m_peakContextTokens, promptTokens + completionTokens);
    }
}

void SubtitleTranslationService::complete(State state, const QString &error)
{
    SubtitleTranslationResult result;
    result.task.state = state;
    result.task.audioPath = m_plan.request.audioPath;
    result.task.taskDirectory = m_plan.checkpointDirectory;
    result.task.error = state == State::Cancelled
        ? QStringLiteral("已取消字幕翻译；已完成批次保留，可手动重试。已送达服务端的请求仍可能计费。") : error;
    result.task.subtitleCount = m_plan.request.useWorkContext && m_plan.phase == SubtitleTranslationPhase::Correction
        ? 0 : m_translations.size();
    result.task.mode = QStringLiteral("cloud-translation");
    result.task.language = QStringLiteral("zh");
    result.task.warnings = m_plan.warnings;
    complete(std::move(result));
}

void SubtitleTranslationService::complete(SubtitleTranslationResult result)
{
    if (!m_busy || m_completing) return;
    m_completing = true;
    clearReply();
    m_key.reset();
    m_messages = {};
    result.requestCount = m_requestCount;
    result.promptTokens = m_requestCount == 0 ? 0 : m_promptTokens;
    result.completionTokens = m_requestCount == 0 ? 0 : m_completionTokens;
    result.peakContextTokens = m_peakContextTokens;
    result.contextTokenLimit = translationApiContextTokenLimit(m_plan.request.settings);
    // Failed, timed-out or cancelled requests may still have consumed tokens on
    // the server. Missing usage must not silently look like an exact run total.
    result.usageComplete = m_usageResponseCount == m_requestCount;
    result.stopQueue = m_stopQueue && result.task.state == State::Failed;
    if (m_plan.request.useWorkContext) {
        result.contextGroup = m_correctionPlan.contextGroup;
        result.correctionFingerprint = m_correctionPlan.fingerprint;
        if (m_plan.phase == SubtitleTranslationPhase::Translation) result.translationFingerprint = m_plan.fingerprint;
    }
    m_result = result;
    if (result.task.state != State::Succeeded && m_checkpointReady && m_checkpointLease) {
        const auto plan = m_plan;
        const auto lease = m_checkpointLease;
        const QString state = result.task.state == State::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
        const int batchIndex = m_currentBatch;
        runWorker(WorkStage::Finish, [plan, lease, state, batchIndex, result = std::move(result)]() mutable {
            WorkResult value;
            value.lease = lease;
            value.result = std::move(result);
            if (!updateSubtitleTranslationCheckpointState(plan, state, batchIndex, &value.error)
                && value.error.isEmpty()) value.error = QStringLiteral("无法保存任务终态。");
            return value;
        });
        return;
    }
    notifyCompleted(std::move(result));
}

void SubtitleTranslationService::notifyCompleted(SubtitleTranslationResult result)
{
    m_result = result;
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation, result] {
        if (!m_busy || !m_completing || generation != m_generation) return;
        m_busy = false;
        m_completing = false;
        m_checkpointReady = false;
        m_checkpointLease.reset();
        QPointer<SubtitleTranslationService> guard(this);
        emit finished(result);
        if (guard && !m_busy && generation == m_generation) emit busyChanged(false);
    });
}
} // namespace asmr
