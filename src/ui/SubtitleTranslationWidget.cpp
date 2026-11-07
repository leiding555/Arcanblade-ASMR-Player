#include "ui/SubtitleTranslationWidget.h"
#include "ai/SubtitleTranslationService.h"
#include "ai/SubtitleWorkContext.h"
#include "library/SubtitleTranslationCleanup.h"
#include "ui/DialogFrame.h"
#include "ui/InputControlPolicy.h"
#include <QAction>
#include <QDesktopServices>
#include <QCheckBox>
#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace asmr {
namespace {
QLabel *label(const QString &name, QWidget *parent)
{
    auto *value = new QLabel(parent);
    value->setObjectName(name); value->setTextFormat(Qt::PlainText); value->setWordWrap(true);
    value->setMinimumWidth(0); value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return value;
}
QString normalizedPath(const QString &source)
{
    const auto path = QDir::fromNativeSeparators(source);
    return path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path) ? QString() : QDir::cleanPath(path);
}
bool equalPath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}
bool validOutputIdentity(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"));
    return value.isEmpty() || pattern.match(value).hasMatch();
}
QString contextDescription(SubtitleWorkContextKind kind)
{
    return kind == SubtitleWorkContextKind::Asr ? QStringLiteral("ASR 字幕上下文") : QStringLiteral("参考文件上下文");
}
QString outputKey(const QString &sourcePath, const QString &audioPath)
{
    const QFileInfo source(audioPath.isEmpty() ? sourcePath : audioPath);
    QString stem = source.completeBaseName();
    if (audioPath.isEmpty() && stem.endsWith(QStringLiteral(".ja"), Qt::CaseInsensitive)) stem.chop(3);
    QString key = source.absolutePath() + QLatin1Char('/') + stem;
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}
class TextDialog final : public QDialog {
public:
    TextDialog(const QString &title, QLayout *content, QWidget *parent) : QDialog(parent)
    {
        setWindowTitle(title); setAttribute(Qt::WA_DeleteOnClose); setWindowModality(Qt::WindowModal);
        resize(740, 560); frame = new DialogFrame(this, content);
    }
protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        return frame && frame->handleNativeEvent(type, message, result)
            ? true : QDialog::nativeEvent(type, message, result);
    }
private:
    DialogFrame *frame = nullptr;
};
} // namespace

SubtitleTranslationWidget::SubtitleTranslationWidget(QSettings *settings, QWidget *parent,
    SubtitleTranslationService *service, const QString &databasePath)
    : QWidget(parent), m_settings(settings), m_service(service),
      m_databasePath(aiTaskDatabasePath(settings, databasePath))
{
    setObjectName(QStringLiteral("subtitleTranslationWidget")); InputControlPolicy::install();
    if (!m_service) m_service = new SubtitleTranslationService(this);
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0); body->setSpacing(8);
    m_serviceLabel = label(QStringLiteral("subtitleTranslationService"), this); body->addWidget(m_serviceLabel);
    m_configure = new QPushButton(QStringLiteral("配置翻译服务…"), this);
    m_configure->setObjectName(QStringLiteral("subtitleTranslationConfigure")); body->addWidget(m_configure, 0, Qt::AlignLeft);
    auto *options = new QHBoxLayout;
    m_thinking = new QCheckBox(QStringLiteral("启用思考模式"), this);
    m_thinking->setObjectName(QStringLiteral("subtitleTranslationThinking"));
    options->addWidget(m_thinking);
    m_useWorkContext = new QCheckBox(QStringLiteral("使用作品上下文"), this);
    m_useWorkContext->setObjectName(QStringLiteral("subtitleTranslationUseWorkContext"));
    m_useWorkContext->setChecked(m_settings && m_settings->value(QStringLiteral("ai/subtitleTranslation/useWorkContext"), false).toBool());
    m_useWorkContext->setToolTip(QStringLiteral("对本批全部字幕启用：优先使用作品参考资料，无可用正文时使用作品内 ASR；先校正日语再翻译中文。仅支持已入库 ASR 和 DeepSeek。"));
    options->addWidget(m_useWorkContext); options->addStretch(); body->addLayout(options);
    connect(m_thinking, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_settings || !m_thinking->isEnabled() || isBusy() || m_shuttingDown) { updateControls(); return; }
        auto settings = currentSettings();
        if (settings.provider != TranslationApiProvider::DeepSeek) { updateControls(); return; }
        settings.thinkingEnabled = checked;
        QString error;
        if (!writeTranslationApiSettings(*m_settings, settings, &error))
            setStatus(QStringLiteral("思考模式设置未能保存，请重试。"));
        updateControls();
    });
    connect(m_useWorkContext, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_useWorkContext->isEnabled() || isBusy() || m_shuttingDown) { updateControls(); return; }
        if (m_settings) m_settings->setValue(QStringLiteral("ai/subtitleTranslation/useWorkContext"), checked);
        updateControls();
    });
    m_table = new QTableWidget(0, 2, this); m_table->setObjectName(QStringLiteral("subtitleTranslationTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("字幕文件"), QStringLiteral("状态")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->setShowGrid(false);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setMinimumHeight(140); m_table->setContextMenuPolicy(Qt::CustomContextMenu); body->addWidget(m_table, 1);
    m_status = label(QStringLiteral("subtitleTranslationStatus"), this);
    setStatus({}); body->addWidget(m_status);
    m_historyStatus = label(QStringLiteral("subtitleTranslationHistoryStatus"), this);
    m_historyStatus->setText(QStringLiteral("正在读取任务记录…")); body->addWidget(m_historyStatus);
    m_progress = new QProgressBar(this); m_progress->setObjectName(QStringLiteral("subtitleTranslationProgress"));
    m_progress->setTextVisible(false); m_progress->hide(); body->addWidget(m_progress);
    m_summary = label(QStringLiteral("subtitleTranslationSummary"), this); body->addWidget(m_summary);
    m_add = new QPushButton(QStringLiteral("添加字幕…"), this); m_add->setObjectName(QStringLiteral("subtitleTranslationAdd"));
    m_clear = new QPushButton(QStringLiteral("清空列表"), this); m_clear->setObjectName(QStringLiteral("subtitleTranslationClear"));
    m_clear->setToolTip(QStringLiteral("只移除当前列表，不删除原字幕、翻译结果或断点。"));
    m_openFolder = new QPushButton(QStringLiteral("打开结果文件夹"), this);
    m_openFolder->setObjectName(QStringLiteral("subtitleTranslationOpenFolder"));
    auto *actions = new QHBoxLayout;
    m_retry = new QPushButton(QStringLiteral("重试未完成项"), this); m_retry->setObjectName(QStringLiteral("subtitleTranslationRetry"));
    m_cancel = new QPushButton(QStringLiteral("取消批次"), this); m_cancel->setObjectName(QStringLiteral("subtitleTranslationCancel"));
    m_start = new QPushButton(QStringLiteral("开始翻译"), this); m_start->setObjectName(QStringLiteral("subtitleTranslationStart"));
    m_start->setProperty("primary", true);
    actions->addWidget(m_add); actions->addWidget(m_clear); actions->addWidget(m_retry); actions->addStretch();
    actions->addWidget(m_openFolder);
    actions->addWidget(m_cancel); actions->addWidget(m_start); body->addLayout(actions);
    m_removeAction = new QAction(QStringLiteral("从列表移除"), this);
    m_folderAction = new QAction(QStringLiteral("打开字幕所在文件夹"), this);
    m_errorAction = new QAction(QStringLiteral("查看完整错误…"), this);
    connect(m_configure, &QPushButton::clicked, this, &SubtitleTranslationWidget::settingsRequested);
    connect(m_add, &QPushButton::clicked, this, &SubtitleTranslationWidget::browseSubtitles);
    connect(m_clear, &QPushButton::clicked, this, &SubtitleTranslationWidget::clearList);
    connect(m_start, &QPushButton::clicked, this, [this] { startTranslation(false); });
    connect(m_retry, &QPushButton::clicked, this, [this] { startTranslation(true); });
    connect(m_cancel, &QPushButton::clicked, this, &SubtitleTranslationWidget::cancelTranslation);
    connect(m_openFolder, &QPushButton::clicked, this, &SubtitleTranslationWidget::openResultFolder);
    connect(m_folderAction, &QAction::triggered, this, &SubtitleTranslationWidget::openResultFolder);
    connect(m_errorAction, &QAction::triggered, this, &SubtitleTranslationWidget::showError);
    connect(m_removeAction, &QAction::triggered, this, [this] {
        const int row = selectedRow();
        if (isBusy() || row < 0 || m_shuttingDown || m_historyLoading || !m_historyWritable || m_startPending >= 0) return;
        m_items.removeAt(row); m_table->removeRow(row); scheduleHistorySave(); refreshWorkContextAvailability(); updateControls();
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        updateControls();
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto *item = m_table->itemAt(point);
        if (!item || isBusy() || m_shuttingDown) return;
        m_table->selectRow(item->row()); updateControls();
        auto *menu = new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(m_removeAction); menu->addAction(m_folderAction); menu->addAction(m_errorAction);
        menu->popup(m_table->viewport()->mapToGlobal(point));
    });
    connect(m_service, &SubtitleTranslationService::busyChanged, this, [this](bool) {
        updateControls();
        QPointer<SubtitleTranslationWidget> guard(this); emit busyChanged(isBusy());
        if (guard) finishShutdownIfReady();
    });
    connect(m_service, &SubtitleTranslationService::prepared, this, [this](const SubtitleTranslationPlan &plan) {
        if (!m_preparing || m_shuttingDown || m_activeRow < 0 || m_activeRow >= m_items.size()) return;
        persistPreparedTranslation(plan);
    });
    connect(m_service, &SubtitleTranslationService::progress, this, [this](const SubtitleTranslationProgress &value) {
        if (m_activeRow < 0 || m_activeRow >= m_items.size()) return;
        m_items[m_activeRow].status = value.state == AiSubtitleTaskState::Running
            ? (value.phase == SubtitleTranslationPhase::Correction ? QStringLiteral("校正日语") : QStringLiteral("翻译中"))
            : value.state == AiSubtitleTaskState::Validating ? QStringLiteral("校验与保存") : QStringLiteral("准备中");
        updateRow(m_activeRow);
        QString text = QStringLiteral("第 %1/%2 项 · %3").arg(m_batchStarted).arg(m_batchTotal).arg(value.message);
        if (value.totalEntries > 0) text += QStringLiteral(" · 已完成 %1/%2 条").arg(value.completedEntries).arg(value.totalEntries);
        if (value.batchCount > 0 && m_service
            && m_service->preparedPlan().mode != SubtitleTranslationMode::ToolConversation)
            text += QStringLiteral(" · 第 %1/%2 批").arg(value.batchIndex).arg(value.batchCount);
        if (value.requestCount > 0) text += QStringLiteral(" · 本次请求 %1 次").arg(value.requestCount);
        if (m_items[m_activeRow].useWorkContext) text += QStringLiteral(" · ") + contextDescription(m_items[m_activeRow].contextKind);
        setStatus(text);
        if (value.totalEntries > 0) {
            m_progress->setRange(0, 100);
            m_progress->setValue(static_cast<int>(100 * value.completedEntries / value.totalEntries));
        } else m_progress->setRange(0, 0);
    });
    connect(m_service, &SubtitleTranslationService::finished, this, [this](const SubtitleTranslationResult &value) {
        const int row = m_activeRow; m_activeRow = -1; m_preparing = false;
        if (row < 0 || row >= m_items.size()) { updateControls(); finishShutdownIfReady(); return; }
        auto &item = m_items[row];
        const bool success = value.task.state == AiSubtitleTaskState::Succeeded;
        const bool cancelled = value.task.state == AiSubtitleTaskState::Cancelled;
        item.succeeded = success; item.retryable = !success;
        item.resultPath = success ? value.task.srtPath : QString();
        item.correctedJapanesePath = success ? value.task.correctedJapanesePath : QString();
        if (!value.contextGroup.isEmpty()) item.contextGroup = value.contextGroup;
        if (!value.correctionFingerprint.isEmpty()) item.correctionFingerprint = value.correctionFingerprint;
        if (!value.translationFingerprint.isEmpty()) item.translationFingerprint = value.translationFingerprint;
        item.taskDirectory = value.task.taskDirectory; item.error = value.task.error; item.warnings = value.task.warnings;
        item.state = success ? QStringLiteral("succeeded") : cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
        item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        finishAttempt(value); scheduleHistorySave();
        item.status = success ? QStringLiteral("已完成") : cancelled ? QStringLiteral("已取消") : QStringLiteral("失败");
        updateRow(row);
        QString status = success ? QStringLiteral("翻译完成，结果已单独保存。")
            : cancelled ? QStringLiteral("已取消。已校验完成的译文保留，可重试未完成部分。") : value.task.error;
        if (value.requestCount > 0) status += QStringLiteral(" 本次请求 %1 次。").arg(value.requestCount);
        const auto tokenCount = [](qint64 count) {
            return count >= 0 ? QString::number(count) : QStringLiteral("未知");
        };
        status += QStringLiteral(" 本次累计输入 %1 / 输出 %2 tokens。")
            .arg(tokenCount(value.requestCount == 0 ? 0 : value.promptTokens),
                tokenCount(value.requestCount == 0 ? 0 : value.completionTokens));
        if (value.requestCount == 0) {
            status += QStringLiteral(" 上下文峰值占用：未使用。");
        } else if (value.peakContextTokens >= 0 && value.contextTokenLimit > 0) {
            status += QStringLiteral(" 上下文峰值占用 %1 / %2 tokens（%3%）。")
                .arg(value.peakContextTokens).arg(value.contextTokenLimit)
                .arg(double(value.peakContextTokens) / double(value.contextTokenLimit) * 100.0, 0, 'f', 1);
        } else {
            status += QStringLiteral(" 上下文峰值占用未知。");
        }
        if (value.requestCount > 0 && !value.usageComplete) status += QStringLiteral(" 用量统计不完整。");
        if (item.useWorkContext) status += QStringLiteral(" 使用") + contextDescription(item.contextKind) + QStringLiteral("。");
        if (!item.warnings.isEmpty()) status += QStringLiteral(" 有 %1 条警告，详见列表状态提示。").arg(item.warnings.size());
        if (m_batchTotal > 1) {
            status.replace(QStringLiteral("本次累计"), QStringLiteral("本项累计"));
            status.prepend(QStringLiteral("第 %1/%2 项 · ").arg(m_batchStarted).arg(m_batchTotal));
        }
        setStatus(status);
        if (success) m_batchResults.append(value.task);
        if (cancelled || value.stopQueue) m_cancelRequested = true;
        m_advancePending = m_batchRunning && !m_cancelRequested && !m_shuttingDown && m_batchCursor < m_batchItems.size();
        updateControls();
        QPointer<SubtitleTranslationWidget> guard(this);
        if (success) emit completed(value.task);
        if (!guard) return;
        if (!m_advancePending) settleBatch();
        else advanceBatchWhenReady();
    });
    connect(m_service, &QObject::destroyed, this, [this] {
        m_service = nullptr; m_startPending = -1; m_preparing = false;
        setStatus(QStringLiteral("字幕翻译服务已关闭。"));
        m_activeRow = -1; m_cancelRequested = true; settleBatch();
    });
    refreshSettings(); restoreHistory();
}
void SubtitleTranslationWidget::restoreHistory()
{
    runHistoryOperation(true);
}
AiTaskSnapshot SubtitleTranslationWidget::historySnapshot() const
{
    AiTaskSnapshot snapshot;
    for (const auto &attempt : m_history.attempts)
        if (std::any_of(m_items.cbegin(), m_items.cend(), [&attempt](const auto &item) { return item.id == attempt.taskId; }))
            snapshot.attempts.append(attempt);
    for (int row = 0; row < m_items.size(); ++row) {
        const auto &item = m_items.at(row);
        AiTaskRecord task;
        task.id = item.id; task.mode = QStringLiteral("cloud-translation");
        task.queueOrder = row; task.state = item.state;
        task.createdAt = item.createdAt; task.updatedAt = item.updatedAt;
        task.data = QJsonObject{{QStringLiteral("version"), 1},
            {QStringLiteral("sourcePath"), item.sourcePath}, {QStringLiteral("audioPath"), item.audioPath},
            {QStringLiteral("libraryRoot"), item.libraryRoot}, {QStringLiteral("libraryRelativePath"), item.libraryRelativePath},
            {QStringLiteral("requestTaskId"), item.translationTaskId}, {QStringLiteral("resultMissing"), item.resultMissing},
            {QStringLiteral("outputBatchId"), item.outputBatchId}, {QStringLiteral("outputFileSuffix"), item.outputFileSuffix},
            {QStringLiteral("resultPath"), item.resultPath}, {QStringLiteral("taskDirectory"), item.taskDirectory},
            {QStringLiteral("correctedJapanesePath"), item.correctedJapanesePath},
            {QStringLiteral("useWorkContext"), item.useWorkContext},
            {QStringLiteral("workId"), item.workId}, {QStringLiteral("audioFileId"), item.audioFileId},
            {QStringLiteral("subtitleFileId"), item.subtitleFileId}, {QStringLiteral("referenceDigest"), item.referenceDigest},
            {QStringLiteral("contextGroup"), item.contextGroup},
            {QStringLiteral("correctionFingerprint"), item.correctionFingerprint},
            {QStringLiteral("translationFingerprint"), item.translationFingerprint},
            {QStringLiteral("error"), item.error}, {QStringLiteral("warnings"), QJsonArray::fromStringList(item.warnings)}};
        if (item.useWorkContext && item.contextKind == SubtitleWorkContextKind::Asr)
            task.data.insert(QStringLiteral("contextKind"), QStringLiteral("asr"));
        snapshot.tasks.append(task);
    }
    return snapshot;
}
void SubtitleTranslationWidget::scheduleHistorySave()
{
    if (!m_historyWritable) return;
    m_historyDirty = true;
    if (m_historyLoading || m_historyWorker || m_checkingResults) return;
    QTimer::singleShot(0, this, [this] {
        if (m_historyDirty && !m_historyLoading && !m_historyWorker && !m_checkingResults) persistHistory();
    });
}
void SubtitleTranslationWidget::persistHistory()
{
    if (!m_historyDirty || !m_historyWritable || !m_historyLease || m_historyLoading || m_historyWorker
        || (m_checkingResults && !m_resultCheckWaiting)) return;
    m_historyDirty = false;
    runHistoryOperation(false, historySnapshot());
}
void SubtitleTranslationWidget::runHistoryOperation(bool loading, const AiTaskSnapshot &snapshot)
{
    m_historyWorker = true;
    const QString path = m_databasePath;
    const auto retainedLease = m_historyLease;
    auto promise = std::make_shared<QPromise<HistoryResult>>();
    auto *watcher = new QFutureWatcher<HistoryResult>(this);
    connect(watcher, &QFutureWatcher<HistoryResult>::finished, this, [this, watcher, loading] {
        auto result = watcher->future().takeResult();
        watcher->deleteLater(); m_historyWorker = false;
        historyOperationFinished(loading, std::move(result));
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([path, loading, snapshot, promise, lease = retainedLease]() mutable {
        promise->start();
        HistoryResult result;
        if (loading) result.lease = acquireAiTaskQueueLease(path, QStringLiteral("cloud"), &result.error);
        if ((loading && result.lease) || (!loading && lease)) {
            AiTaskRepository repository(path);
            if (loading) {
                // The queue lease proves this window owns recovery. Mark old
                // running rows interrupted before deciding whether a missing
                // published result is safe to discard; active tasks are kept.
                if (repository.load({QStringLiteral("cloud-translation")}, &result.snapshot, &result.error)) {
                    result.error = cleanupMissingSubtitleTranslationResults(path).error;
                    if (result.error.isEmpty())
                        repository.load({QStringLiteral("cloud-translation")}, &result.snapshot, &result.error);
                }
            }
            else repository.save({QStringLiteral("cloud-translation")}, snapshot, &result.error);
        } else if (result.error.isEmpty()) result.error = QStringLiteral("当前窗口没有任务列表的写入权限。");
        // No worker or future keeps a completed save's lease after its callback.
        lease.reset();
        promise->addResult(std::move(result)); promise->finish();
    });
}
void SubtitleTranslationWidget::historyOperationFinished(bool loading, HistoryResult result)
{
    if (loading) m_historyLoading = false;
    if (!result.error.isEmpty()) {
        const bool wasWaiting = m_startPending >= 0 || (m_resultCheckWaiting && m_resultCheckExclusive);
        if (m_resultCheckWaiting) {
            m_resultCheckWaiting = false; m_checkingResults = false;
            m_resultCheckExclusive = false; m_startAfterResultCheck = false;
            if (m_activeRow >= 0 && m_activeRow < m_items.size()) {
                auto &item = m_items[m_activeRow]; item.state = QStringLiteral("failed");
                item.status = QStringLiteral("记录保存失败"); item.retryable = true; item.error = result.error;
                updateRow(m_activeRow);
            }
            m_activeRow = -1;
        }
        m_historyWritable = false;
        m_historyStatus->setText(loading && !result.lease ? result.error
            : QStringLiteral("任务记录无法读取或保存，本次不能启动翻译。原字幕、译文和断点保持。"));
        m_historyStatus->setToolTip(result.error); m_historyStatus->show();
        result.lease.reset();
        if (m_startPending >= 0) {
            auto &item = m_items[m_startPending]; item.state = QStringLiteral("failed");
            item.status = QStringLiteral("记录保存失败"); item.retryable = true; item.error = result.error;
            updateRow(m_startPending); m_startPending = -1; m_activeRow = -1; m_preparing = false; m_currentAttempt.clear();
        }
        updateControls();
        QPointer<SubtitleTranslationWidget> guard(this);
        m_cancelRequested = true;
        if (m_batchRunning) settleBatch();
        else if (wasWaiting) emit busyChanged(false);
        if (guard) finishShutdownIfReady(); return;
    }
    if (loading) {
        m_historyLease = std::move(result.lease);
        QVector<Item> restored;
        bool valid = true;
        for (const auto &record : result.snapshot.tasks) {
            const auto &savedData = record.data;
            for (const auto &key : {"sourcePath", "audioPath", "libraryRoot", "libraryRelativePath",
                     "resultPath", "taskDirectory", "error"})
                if (!savedData.value(QLatin1String(key)).isString()) valid = false;
            if (!savedData.value(QStringLiteral("warnings")).isArray()) valid = false;
            Item item;
            item.id = record.id; item.state = record.state; item.createdAt = record.createdAt; item.updatedAt = record.updatedAt;
            item.sourcePath = normalizedPath(savedData.value(QStringLiteral("sourcePath")).toString());
            item.audioPath = savedData.value(QStringLiteral("audioPath")).toString();
            item.libraryRoot = savedData.value(QStringLiteral("libraryRoot")).toString();
            item.libraryRelativePath = savedData.value(QStringLiteral("libraryRelativePath")).toString();
            item.translationTaskId = savedData.value(QStringLiteral("requestTaskId")).toString();
            item.outputBatchId = savedData.value(QStringLiteral("outputBatchId")).toString();
            item.outputFileSuffix = savedData.value(QStringLiteral("outputFileSuffix")).toString();
            for (const auto &key : {QStringLiteral("outputBatchId"), QStringLiteral("outputFileSuffix")})
                if (savedData.contains(key) && !savedData.value(key).isString()) valid = false;
            if (!validOutputIdentity(item.outputBatchId) || !validOutputIdentity(item.outputFileSuffix)) valid = false;
            if (savedData.contains(QStringLiteral("requestTaskId"))
                && !savedData.value(QStringLiteral("requestTaskId")).isString()) valid = false;
            if (!item.translationTaskId.isEmpty() && QUuid(item.translationTaskId).isNull()) valid = false;
            item.resultMissing = savedData.value(QStringLiteral("resultMissing")).toBool();
            if (savedData.contains(QStringLiteral("resultMissing"))
                && !savedData.value(QStringLiteral("resultMissing")).isBool()) valid = false;
            if (item.resultMissing && item.translationTaskId.isEmpty()) {
                item.translationTaskId = item.id;
                m_historyDirty = true;
            }
            if (item.resultMissing && (!item.outputBatchId.isEmpty() || !item.outputFileSuffix.isEmpty())) {
                item.outputBatchId.clear(); item.outputFileSuffix.clear(); m_historyDirty = true;
            }
            item.resultPath = savedData.value(QStringLiteral("resultPath")).toString();
            item.correctedJapanesePath = savedData.value(QStringLiteral("correctedJapanesePath")).toString();
            item.useWorkContext = savedData.value(QStringLiteral("useWorkContext")).toBool();
            if (savedData.contains(QStringLiteral("useWorkContext"))
                && !savedData.value(QStringLiteral("useWorkContext")).isBool()) valid = false;
            if (savedData.contains(QStringLiteral("contextKind"))) {
                if (savedData.value(QStringLiteral("contextKind")) != QJsonValue(QStringLiteral("asr")) || !item.useWorkContext) valid = false;
                else item.contextKind = item.availableContextKind = SubtitleWorkContextKind::Asr;
            }
            item.workId = savedData.value(QStringLiteral("workId")).toInteger();
            item.audioFileId = savedData.value(QStringLiteral("audioFileId")).toInteger();
            item.subtitleFileId = savedData.value(QStringLiteral("subtitleFileId")).toInteger();
            for (const auto *key : {"workId", "audioFileId", "subtitleFileId"}) {
                const auto value = savedData.value(QLatin1String(key));
                if (!value.isUndefined() && (!value.isDouble() || value.toInteger(-1) < 0
                    || value.toDouble() != double(value.toInteger(-1)))) valid = false;
            }
            item.referenceDigest = savedData.value(QStringLiteral("referenceDigest")).toString();
            item.contextGroup = savedData.value(QStringLiteral("contextGroup")).toString();
            item.correctionFingerprint = savedData.value(QStringLiteral("correctionFingerprint")).toString();
            item.translationFingerprint = savedData.value(QStringLiteral("translationFingerprint")).toString();
            for (const auto *key : {"referenceDigest", "contextGroup", "correctionFingerprint", "translationFingerprint", "correctedJapanesePath"}) {
                const auto value = savedData.value(QLatin1String(key));
                if (!value.isUndefined() && !value.isString()) valid = false;
            }
            item.taskDirectory = savedData.value(QStringLiteral("taskDirectory")).toString();
            item.error = savedData.value(QStringLiteral("error")).toString();
            for (const auto &warning : savedData.value(QStringLiteral("warnings")).toArray()) {
                if (!warning.isString()) valid = false;
                else item.warnings.append(warning.toString());
            }
            if (item.sourcePath.isEmpty()) valid = false;
            for (const auto &path : {item.audioPath, item.resultPath, item.correctedJapanesePath, item.taskDirectory})
                if (!path.isEmpty() && normalizedPath(path).isEmpty()) valid = false;
            if ((!item.libraryRoot.isEmpty() || !item.libraryRelativePath.isEmpty())
                && (normalizedPath(item.libraryRoot).isEmpty() || item.audioPath.isEmpty()
                    || QDir::isAbsolutePath(item.libraryRelativePath)
                    || item.libraryRelativePath == QStringLiteral("..")
                    || item.libraryRelativePath.startsWith(QStringLiteral("../"))
                    || !equalPath(normalizedPath(QDir(item.libraryRoot).filePath(item.libraryRelativePath)), item.audioPath)))
                valid = false;
            item.succeeded = item.state == QStringLiteral("succeeded");
            item.retryable = item.state == QStringLiteral("failed") || item.state == QStringLiteral("cancelled")
                || item.state == QStringLiteral("interrupted");
            if (item.succeeded && item.resultPath.isEmpty()) valid = false;
            if (item.state == QStringLiteral("pending")) item.status = QStringLiteral("待翻译");
            else if (item.succeeded) item.status = QStringLiteral("已完成");
            else if (item.state == QStringLiteral("failed")) item.status = QStringLiteral("失败");
            else if (item.state == QStringLiteral("cancelled")) item.status = QStringLiteral("已取消");
            else if (item.state == QStringLiteral("interrupted")) {
                item.status = QStringLiteral("已中断");
                item.error = QStringLiteral("上次运行已中断，请手动重试。已校验的数据库断点保留。");
            } else valid = false;
            restored.append(item);
        }
        if (!valid) {
            m_historyWritable = false;
            m_historyStatus->setText(QStringLiteral("任务记录格式无效，原记录已保留；本次不能启动翻译。"));
            m_historyStatus->show(); updateControls(); finishShutdownIfReady(); return;
        }
        m_history = result.snapshot;
        // Entry points may append a subtitle while the asynchronous load runs.
        // Merge those additions after saved rows without replacing completed work.
        for (const auto &item : m_items) {
            const bool duplicate = std::any_of(restored.cbegin(), restored.cend(), [&item](const auto &other) {
                const bool differentWorks = item.workId > 0 && other.workId > 0 && item.workId != other.workId;
                const bool sameIdentity = item.workId > 0 && item.subtitleFileId > 0
                    && item.workId == other.workId && item.subtitleFileId == other.subtitleFileId;
                return !differentWorks && (sameIdentity || equalPath(item.sourcePath, other.sourcePath));
            });
            if (!duplicate) restored.append(item);
        }
        m_items = std::move(restored); m_table->setRowCount(static_cast<int>(m_items.size()));
        for (int row = 0; row < m_items.size(); ++row) updateRow(row);
        if (!m_items.isEmpty()) m_table->selectRow(0);
    }
    m_historyStatus->clear(); m_historyStatus->hide();
    if (m_resultCheckWaiting) {
        if (!m_shuttingDown) {
            // An earlier save may have captured the running row while the UI
            // already holds its terminal state. Flush every newer snapshot
            // before cleanup is allowed to decide whether that task is active.
            if (m_historyDirty) { persistHistory(); updateControls(); return; }
            m_resultCheckWaiting = false;
            launchPublishedResultCheck(); updateControls(); return;
        }
        m_resultCheckWaiting = false;
        m_checkingResults = false; m_resultCheckExclusive = false; m_startAfterResultCheck = false;
    }
    if (m_historyDirty) persistHistory();
    else if (m_startPending >= 0 && !m_shuttingDown) {
        QPointer<SubtitleTranslationWidget> guard(this); beginPersistedTranslation();
        if (!guard) return;
    }
    else if (m_advancePending) {
        QPointer<SubtitleTranslationWidget> guard(this); advanceBatchWhenReady();
        if (!guard) return;
    }
    updateControls();
    if (loading && !m_shuttingDown) refreshWorkContextAvailability();
    finishShutdownIfReady();
}
void SubtitleTranslationWidget::beginPersistedTranslation()
{
    const int row = m_startPending; m_startPending = -1;
    if (row < 0 || row >= m_items.size() || !m_service || m_shuttingDown) return;
    m_activeRow = row; m_items[row].status = QStringLiteral("准备翻译"); updateRow(row);
    QPointer<SubtitleTranslationWidget> guard(this);
    const bool accepted = m_service->startPrepared();
    if (!guard) return;
    if (!accepted) {
        failUnstartedTranslation(QStringLiteral("翻译服务未接受任务，请重试。"));
    }
}
void SubtitleTranslationWidget::failUnstartedTranslation(const QString &error)
{
    const int row = m_activeRow;
    m_activeRow = -1; m_startPending = -1; m_preparing = false;
    if (row >= 0 && row < m_items.size()) {
        auto &item = m_items[row]; item.state = QStringLiteral("failed"); item.status = QStringLiteral("失败");
        item.retryable = true; item.error = error;
        item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        SubtitleTranslationResult result; result.task.state = AiSubtitleTaskState::Failed; result.task.error = error;
        finishAttempt(result); updateRow(row); scheduleHistorySave();
    }
    setStatus(error); updateControls();
    m_advancePending = m_batchRunning && !m_cancelRequested && !m_shuttingDown && m_batchCursor < m_batchItems.size();
    QPointer<SubtitleTranslationWidget> guard(this);
    if (m_advancePending) advanceBatchWhenReady();
    else settleBatch();
    if (guard) finishShutdownIfReady();
}
void SubtitleTranslationWidget::finishAttempt(const SubtitleTranslationResult &value)
{
    if (m_currentAttempt.isEmpty()) return;
    for (auto &attempt : m_history.attempts) {
        if (attempt.id != m_currentAttempt) continue;
        const auto &task = value.task;
        attempt.state = task.state == AiSubtitleTaskState::Succeeded ? QStringLiteral("succeeded")
            : task.state == AiSubtitleTaskState::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
        attempt.finishedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        attempt.result = QJsonObject{{QStringLiteral("audioPath"), task.audioPath},
            {QStringLiteral("srtPath"), task.srtPath}, {QStringLiteral("taskDirectory"), task.taskDirectory},
            {QStringLiteral("correctedJapanesePath"), task.correctedJapanesePath},
            {QStringLiteral("contextGroup"), value.contextGroup},
            {QStringLiteral("correctionFingerprint"), value.correctionFingerprint},
            {QStringLiteral("translationFingerprint"), value.translationFingerprint},
            {QStringLiteral("logPath"), task.logPath}, {QStringLiteral("error"), task.error},
            {QStringLiteral("warnings"), QJsonArray::fromStringList(task.warnings)},
            {QStringLiteral("exitCode"), task.exitCode}, {QStringLiteral("subtitleCount"), qint64(task.subtitleCount)},
            {QStringLiteral("requestCount"), value.requestCount}, {QStringLiteral("promptTokens"), value.promptTokens},
            {QStringLiteral("completionTokens"), value.completionTokens},
            {QStringLiteral("peakContextTokens"), value.peakContextTokens},
            {QStringLiteral("contextTokenLimit"), value.contextTokenLimit},
            {QStringLiteral("usageComplete"), value.usageComplete}};
        if (attempt.parameters.value(QStringLiteral("contextKind")) == QJsonValue(QStringLiteral("asr")))
            attempt.result.insert(QStringLiteral("contextKind"), QStringLiteral("asr"));
        break;
    }
    m_currentAttempt.clear();
}

SubtitleTranslationWidget::~SubtitleTranslationWidget()
{
    if (m_contextCancelled) m_contextCancelled->store(true);
    if (m_service) { disconnect(m_service, nullptr, this, nullptr); m_service->cancel(); }
}
bool SubtitleTranslationWidget::isBusy() const { return m_batchRunning || (m_contextChecking && m_contextStartAfterCheck) || (m_checkingResults && m_resultCheckExclusive) || m_preparing || m_startPending >= 0 || (m_service && m_service->isBusy()); }
TranslationApiSettings SubtitleTranslationWidget::currentSettings() const
{
    const auto provider = m_settings ? readSelectedTranslationApiProvider(*m_settings) : TranslationApiProvider::DeepSeek;
    return m_settings ? readTranslationApiSettings(*m_settings, provider) : defaultTranslationApiSettings(provider);
}
void SubtitleTranslationWidget::refreshSettings()
{
    updateControls();
    refreshWorkContextAvailability();
}
void SubtitleTranslationWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event); refreshSettings();
    if (!m_historyLoading && !isBusy() && !m_shuttingDown && m_historyWritable)
        checkPublishedResults(false);
}
void SubtitleTranslationWidget::refreshWorkContextAvailability()
{
    if (m_shuttingDown) return;
    if (isBusy() || m_historyLoading) { m_contextRefreshPending = true; return; }
    for (auto &item : m_items) if (!item.succeeded) item.contextChecked = false;
    ++m_contextGeneration;
    if (m_contextChecking) {
        m_contextRefreshPending = true;
        if (m_contextCancelled) m_contextCancelled->store(true);
        return;
    }
    checkWorkContexts();
}

void SubtitleTranslationWidget::checkWorkContexts(bool startAfterCheck)
{
    if (m_shuttingDown) return;
    if (m_contextChecking) {
        ++m_contextGeneration; m_contextRefreshPending = true;
        m_contextStartAfterCheck = startAfterCheck || m_contextStartAfterCheck;
        if (m_contextCancelled) m_contextCancelled->store(true);
        updateControls(); return;
    }
    QVector<Item> candidates;
    for (const auto &item : std::as_const(m_items)) {
        if (item.succeeded) continue;
        if (!startAfterCheck || m_batchItems.contains(item.id)) candidates.append(item);
    }
    if (candidates.isEmpty()) {
        m_contextRefreshPending = false; m_contextStartAfterCheck = false;
        updateControls(); return;
    }
    struct Checked { QString id, sourcePath; SubtitleWorkContext context; };
    using Checks = QVector<Checked>;
    m_contextChecking = true; m_contextStartAfterCheck = startAfterCheck; m_contextRefreshPending = false;
    m_contextCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_contextCancelled;
    const auto generation = ++m_contextGeneration;
    const auto database = m_databasePath;
    const auto settings = startAfterCheck ? m_startSettings : currentSettings();
    if (startAfterCheck) setStatus(QStringLiteral("正在核验本批作品上下文，尚未发送请求…"));
    auto promise = std::make_shared<QPromise<Checks>>();
    auto *watcher = new QFutureWatcher<Checks>(this);
    connect(watcher, &QFutureWatcher<Checks>::finished, this, [this, watcher, generation, startAfterCheck, cancelled] {
        auto checks = watcher->future().takeResult(); watcher->deleteLater();
        m_contextChecking = false;
        const bool accepted = !m_shuttingDown && !cancelled->load() && generation == m_contextGeneration;
        QStringList errors;
        if (accepted) {
            for (const auto &checked : checks) {
                const auto found = std::find_if(m_items.begin(), m_items.end(), [&checked](const auto &item) {
                    return item.id == checked.id && equalPath(item.sourcePath, checked.sourcePath);
                });
                if (found == m_items.end()) { errors.append(QStringLiteral("翻译列表已变化，请重试。")); continue; }
                auto &item = *found;
                item.contextChecked = true;
                item.contextEligible = checked.context.succeeded(); item.contextError = checked.context.error;
                item.availableContextKind = checked.context.contextKind;
                item.contextWarnings = checked.context.warnings;
                updateRow(int(std::distance(m_items.begin(), found)));
                if (!item.contextEligible) {
                    errors.append(QFileInfo(item.sourcePath).fileName() + QStringLiteral("：") + item.contextError);
                    continue;
                }
            }
        }
        if (startAfterCheck && accepted && !m_cancelRequested) {
            m_contextStartAfterCheck = false;
            if (errors.isEmpty()) {
                for (const auto &checked : checks) {
                    auto found = std::find_if(m_items.begin(), m_items.end(), [&checked](const auto &item) { return item.id == checked.id; });
                    auto &item = *found; const auto &context = checked.context;
                    const bool changed = !item.useWorkContext || item.contextKind != context.contextKind
                        || (!item.referenceDigest.isEmpty() && item.referenceDigest != context.referenceDigest);
                    if (changed && (!item.contextGroup.isEmpty() || !item.referenceDigest.isEmpty()
                        || std::any_of(m_history.attempts.cbegin(), m_history.attempts.cend(), [&item](const auto &attempt) { return attempt.taskId == item.id; }))) {
                        item.translationTaskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        item.outputBatchId.clear(); item.outputFileSuffix.clear();
                        item.contextGroup.clear(); item.correctionFingerprint.clear(); item.translationFingerprint.clear();
                        item.correctedJapanesePath.clear(); item.resultPath.clear(); item.taskDirectory.clear();
                    }
                    item.useWorkContext = true; item.referenceDigest = context.referenceDigest;
                    item.contextKind = context.contextKind; item.warnings = context.warnings;
                    item.workId = context.work.id; item.audioFileId = context.audio.id; item.subtitleFileId = context.subtitle.id;
                    item.libraryRoot = context.work.rootPath; item.libraryRelativePath = context.audio.relativePath;
                    item.audioPath = QDir(context.work.rootPath).filePath(context.audio.relativePath);
                }
                beginPreparedBatch();
            }
            else {
                m_batchItems.clear();
                setStatus(QStringLiteral("本批不能使用作品上下文，未发送请求。请移除不合格项后重新勾选：\n") + errors.join(QLatin1Char('\n')));
            }
        } else if (startAfterCheck && !m_contextRefreshPending) {
            m_contextStartAfterCheck = false;
            if (!m_shuttingDown) setStatus(QStringLiteral("上下文核验已取消，尚未发送请求。"));
        }
        if (!accepted && !m_shuttingDown && std::any_of(m_items.cbegin(), m_items.cend(), [](const auto &item) {
            return !item.succeeded && !item.contextChecked;
        })) m_contextRefreshPending = true;
        updateControls();
        QPointer<SubtitleTranslationWidget> guard(this);
        if (m_contextRefreshPending && !m_shuttingDown) {
            // A stopped batch must not suppress later passive eligibility
            // refreshes after files/settings change. Only a new run is cancelled.
            const bool start = m_contextStartAfterCheck && !m_cancelRequested;
            if (!start) m_contextStartAfterCheck = false;
            if (start || !isBusy()) {
                m_contextRefreshPending = false;
                checkWorkContexts(start);
            }
        }
        if (!guard) return;
        if (startAfterCheck && !isBusy()) emit busyChanged(false);
        if (guard) finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, candidates, database, settings, cancelled] {
        promise->start(); Checks checks;
        for (const auto &item : candidates) {
            if (cancelled->load()) break;
            SubtitleTranslationRequest request{item.sourcePath, item.audioPath, item.libraryRoot, item.libraryRelativePath, settings, {}, database};
            request.useWorkContext = true; request.workId = item.workId;
            request.audioFileId = item.audioFileId; request.subtitleFileId = item.subtitleFileId;
            SubtitleWorkContext context;
            try { context = resolveSubtitleWorkContextSelection(request, *cancelled, false); }
            catch (...) { context.error = QStringLiteral("作品上下文查询失败，请重试。" ); }
            checks.append({item.id, item.sourcePath, std::move(context)});
        }
        promise->addResult(std::move(checks)); promise->finish();
    });
    updateControls();
}
bool SubtitleTranslationWidget::addSubtitleFile(const QString &sourcePath, const QString &audioPath,
    const QString &libraryRoot, const QString &libraryRelativePath, qint64 workId, qint64 audioFileId, qint64 subtitleFileId)
{
    if (m_shuttingDown || isBusy() || !m_historyWritable) return false;
    const auto path = normalizedPath(sourcePath);
    if (path.isEmpty()) { setStatus(QStringLiteral("请选择有效的本地字幕完整路径。")); return false; }
    for (int row = 0; row < m_items.size(); ++row) {
        const bool differentWorks = workId > 0 && m_items[row].workId > 0 && workId != m_items[row].workId;
        const bool sameIdentity = workId > 0 && subtitleFileId > 0 && m_items[row].workId == workId
            && m_items[row].subtitleFileId == subtitleFileId;
        if (!differentWorks && (sameIdentity || equalPath(m_items[row].sourcePath, path))) {
            const auto &existing = m_items[row];
            const bool attempted = existing.succeeded || !existing.contextGroup.isEmpty()
                || std::any_of(m_history.attempts.cbegin(), m_history.attempts.cend(), [&existing](const auto &attempt) {
                    return attempt.taskId == existing.id;
                });
            const bool sameAudio = (existing.audioFileId == 0 || existing.audioFileId == audioFileId)
                && (existing.audioPath.isEmpty() || equalPath(existing.audioPath, audioPath));
            // Enrich an unstarted path-only row, but never retarget an existing
            // execution/checkpoint simply because its shared subtitle was added
            // from the other playback format.
            if (!attempted && sameAudio && workId > 0 && audioFileId > 0 && subtitleFileId > 0) {
                m_items[row].workId = workId; m_items[row].audioFileId = audioFileId;
                m_items[row].subtitleFileId = subtitleFileId;
                m_items[row].audioPath = audioPath; m_items[row].libraryRoot = libraryRoot;
                m_items[row].libraryRelativePath = libraryRelativePath;
                scheduleHistorySave(); refreshWorkContextAvailability();
            }
            m_table->selectRow(row); updateControls(); return true;
        }
    }
    Item item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.translationTaskId = item.id;
    item.createdAt = item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    item.sourcePath = path; item.audioPath = audioPath; item.libraryRoot = libraryRoot;
    item.libraryRelativePath = libraryRelativePath; item.status = QStringLiteral("待翻译");
    item.workId = workId; item.audioFileId = audioFileId; item.subtitleFileId = subtitleFileId;
    m_items.append(item);
    const int row = static_cast<int>(m_items.size()) - 1;
    m_table->insertRow(row); updateRow(row); m_table->selectRow(row);
    scheduleHistorySave();
    refreshWorkContextAvailability();
    setStatus({}); updateControls();
    return true;
}
int SubtitleTranslationWidget::selectedRow() const
{
    const int row = m_table->currentRow();
    return row >= 0 && row < m_items.size() ? row : -1;
}
void SubtitleTranslationWidget::browseSubtitles()
{
    if (isBusy() || m_shuttingDown) return;
    if (m_picker) { m_picker->raise(); m_picker->activateWindow(); return; }
    auto *picker = new QFileDialog(this, QStringLiteral("选择日语字幕"));
    picker->setAttribute(Qt::WA_DeleteOnClose); picker->setFileMode(QFileDialog::ExistingFiles);
    picker->setNameFilter(QStringLiteral("字幕文件 (*.srt *.vtt *.lrc)")); m_picker = picker;
    connect(picker, &QFileDialog::filesSelected, this, [this](const QStringList &paths) {
        for (const auto &path : paths) addSubtitleFile(path);
    });
    picker->open();
}
void SubtitleTranslationWidget::startTranslation(bool retry)
{
    if (m_items.isEmpty() || isBusy() || m_shuttingDown || m_historyLoading || !m_historyWritable || !m_service) return;
    const bool requestedWorkContext = m_useWorkContext->isChecked();
    refreshSettings(); const auto settings = currentSettings(); const auto problem = validateTranslationApiSettings(settings);
    if (!problem.isEmpty()) { setStatus(problem); updateControls(); return; }
    if (requestedWorkContext && settings.provider != TranslationApiProvider::DeepSeek) {
        setStatus(QStringLiteral("作品上下文仅支持 DeepSeek，本次未发送请求。")); updateControls(); return;
    }
    m_batchUseWorkContext = requestedWorkContext;
    m_activeRow = -1; m_startSettings = settings; m_retryBatch = retry; m_cancelRequested = false;
    m_batchItems.clear(); m_batchResults.clear(); m_batchCursor = 0; m_batchStarted = 0; m_batchTotal = 0;
    for (const auto &item : m_items) m_batchItems.append(item.id);
    checkPublishedResults(true);
}
void SubtitleTranslationWidget::checkPublishedResults(bool startAfterCheck)
{
    if (isBusy() || m_shuttingDown || m_historyLoading || !m_historyWritable || !m_historyLease) return;
    if (m_checkingResults) {
        if (!startAfterCheck) return;
        // An explicit start joins the automatic check already in flight. This
        // keeps all SQL recovery ahead of preparation without dropping clicks.
        m_resultCheckExclusive = true; m_startAfterResultCheck = true;
        setStatus(QStringLiteral("正在核对已发布字幕…")); updateControls();
        emit busyChanged(true);
        return;
    }
    m_checkingResults = true; m_resultCheckExclusive = startAfterCheck; m_startAfterResultCheck = startAfterCheck;
    m_resultCheckWaiting = m_historyWorker || m_historyDirty;
    if (startAfterCheck) setStatus(QStringLiteral("正在核对已发布字幕…"));
    updateControls();
    QPointer<SubtitleTranslationWidget> guard(this);
    // Owners can synchronously close or destroy this widget from busyChanged.
    // Never enqueue SQL work until that notification has safely returned.
    if (startAfterCheck) emit busyChanged(true);
    if (!guard) return;
    if (m_shuttingDown && !m_resultCheckWaiting) {
        m_checkingResults = false; m_resultCheckExclusive = false; m_startAfterResultCheck = false;
        if (m_historyDirty) persistHistory();
        updateControls(); finishShutdownIfReady(); return;
    }
    if (!m_resultCheckWaiting) launchPublishedResultCheck();
    else if (!m_historyWorker && m_historyDirty) persistHistory();
}
void SubtitleTranslationWidget::launchPublishedResultCheck()
{
    const auto path = m_databasePath;
    const auto lease = m_historyLease;
    auto promise = std::make_shared<QPromise<HistoryResult>>(); promise->start();
    auto *watcher = new QFutureWatcher<HistoryResult>(this);
    connect(watcher, &QFutureWatcher<HistoryResult>::finished, this, [this, watcher] {
        QPointer<SubtitleTranslationWidget> guard(this);
        auto result = watcher->future().takeResult(); watcher->deleteLater();
        const bool start = m_startAfterResultCheck && !m_shuttingDown;
        const bool exclusive = m_resultCheckExclusive;
        m_checkingResults = false; m_resultCheckExclusive = false; m_startAfterResultCheck = false;
        if (!result.error.isEmpty()) {
            if (start) failUnstartedTranslation(QStringLiteral("无法核对已发布字幕，未发送翻译请求。\n") + result.error);
            else { setStatus(QStringLiteral("已发布字幕核对失败，任务记录保留。")); updateControls(); }
        } else {
            bool restoredPending = false;
            for (const auto &record : result.snapshot.tasks) {
                if (!record.data.value(QStringLiteral("resultMissing")).toBool()) continue;
                for (int row = 0; row < m_items.size(); ++row) {
                    auto &item = m_items[row]; if (item.id != record.id) continue;
                    item.state = QStringLiteral("pending"); item.status = QStringLiteral("待翻译");
                    item.succeeded = false; item.retryable = false; item.resultMissing = true;
                    item.contextChecked = false; item.contextEligible = false; item.contextError.clear();
                    restoredPending = true;
                    item.outputBatchId.clear(); item.outputFileSuffix.clear();
                    item.resultPath.clear(); item.taskDirectory.clear(); item.error.clear(); item.warnings.clear();
                    item.correctedJapanesePath.clear(); item.contextGroup.clear(); item.correctionFingerprint.clear();
                    item.translationFingerprint.clear(); item.referenceDigest.clear();
                    item.updatedAt = record.updatedAt;
                    if (item.translationTaskId.isEmpty()) item.translationTaskId = item.id;
                    updateRow(row); scheduleHistorySave();
                }
            }
            if (start) beginCheckedTranslation();
            else {
                if (restoredPending) refreshWorkContextAvailability();
                updateControls();
            }
        }
        if (!guard) return;
        if (m_historyDirty && !m_historyWorker) persistHistory();
        if (exclusive && !isBusy()) emit busyChanged(false);
        if (!guard) return;
        finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([path, lease, promise] {
        HistoryResult result;
        result.error = cleanupMissingSubtitleTranslationResults(path).error;
        if (result.error.isEmpty()) {
            AiTaskRepository repository(path);
            repository.load({QStringLiteral("cloud-translation")}, &result.snapshot, &result.error);
        }
        promise->addResult(std::move(result)); promise->finish();
    });
}
void SubtitleTranslationWidget::beginCheckedTranslation()
{
    if (m_cancelRequested || m_shuttingDown || !m_service) return;
    QStringList queued;
    QVector<const Item *> sources;
    const auto sameSource = [](const Item &left, const Item &right) {
        if (left.workId > 0 && right.workId > 0 && left.workId != right.workId) return false;
        return (left.workId > 0 && left.subtitleFileId > 0 && left.workId == right.workId
                && left.subtitleFileId == right.subtitleFileId)
            || equalPath(left.sourcePath, right.sourcePath);
    };
    bool duplicateSource = false;
    for (const auto &id : std::as_const(m_batchItems)) {
        const auto item = std::find_if(m_items.cbegin(), m_items.cend(), [&id](const auto &value) { return value.id == id; });
        if (item == m_items.cend() || item->succeeded
            || (item->state != QStringLiteral("pending") && !(m_retryBatch && item->retryable))) continue;
        // Older persisted queues can already contain one row per audio format.
        // Keep their history, but execute each source only once and do not redo
        // a duplicate whose other row still has a validated published result.
        if (std::any_of(sources.cbegin(), sources.cend(), [&](const auto *source) { return sameSource(*source, *item); })
            || std::any_of(m_items.cbegin(), m_items.cend(), [&](const auto &other) {
                return other.succeeded && sameSource(other, *item);
            })) {
            duplicateSource = true;
            continue;
        }
        sources.append(&*item);
        queued.append(id);
    }
    m_batchItems = std::move(queued); m_batchTotal = static_cast<int>(m_batchItems.size());
    if (m_batchItems.isEmpty()) {
        setStatus(duplicateSource ? QStringLiteral("相同源字幕已有完成项，本次未重复翻译。")
            : QStringLiteral("没有待翻译项；已完成任务保留，失败或取消项可点击重试。"));
        updateControls(); return;
    }
    if (m_batchUseWorkContext) { checkWorkContexts(true); return; }
    beginPreparedBatch();
}
void SubtitleTranslationWidget::beginPreparedBatch()
{
    if (m_cancelRequested || m_shuttingDown || !m_service) return;
    if (!m_batchUseWorkContext) {
        for (auto &item : m_items) {
            if (!m_batchItems.contains(item.id) || !item.useWorkContext) continue;
            item.useWorkContext = false;
            item.contextKind = SubtitleWorkContextKind::ReferenceFiles;
            item.translationTaskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.outputBatchId.clear(); item.outputFileSuffix.clear(); item.referenceDigest.clear();
            item.contextGroup.clear(); item.correctionFingerprint.clear(); item.translationFingerprint.clear();
            item.correctedJapanesePath.clear(); item.resultPath.clear(); item.taskDirectory.clear();
        }
    }
    const QString batchId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz_"))
        + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    QSet<QString> occupied;
    QHash<QString, int> unassignedNames;
    for (const auto &item : std::as_const(m_items)) {
        if (!item.outputBatchId.isEmpty())
            occupied.insert(item.outputBatchId + QLatin1Char('/') + outputKey(item.sourcePath, item.audioPath)
                + QLatin1Char('/') + item.outputFileSuffix);
        else if (m_batchItems.contains(item.id)) ++unassignedNames[outputKey(item.sourcePath, item.audioPath)];
    }
    for (auto &item : m_items) {
        if (!m_batchItems.contains(item.id) || !item.outputBatchId.isEmpty()) continue;
        item.outputBatchId = batchId;
        const auto name = outputKey(item.sourcePath, item.audioPath);
        const bool legacyAttempt = !item.resultMissing && std::any_of(m_history.attempts.cbegin(), m_history.attempts.cend(),
            [&item](const auto &attempt) { return attempt.taskId == item.id; });
        if (legacyAttempt && unassignedNames.value(name) > 1) {
            // Old checkpoints include the original filename in their manifest.
            // Isolate only colliding legacy tasks instead of discarding their
            // verified translations by assigning a new filename suffix.
            QString identity = item.id; identity.remove(QLatin1Char('-'));
            item.outputBatchId += QLatin1Char('_') + identity;
        }
        const auto key = item.outputBatchId + QLatin1Char('/') + name + QLatin1Char('/');
        if (occupied.contains(key)) {
            item.outputFileSuffix = item.id;
            item.outputFileSuffix.remove(QLatin1Char('-'));
        }
        occupied.insert(key + item.outputFileSuffix);
    }
    m_batchRunning = true; m_advancePending = true;
    scheduleHistorySave(); advanceBatchWhenReady(); updateControls();
}
void SubtitleTranslationWidget::advanceBatchWhenReady()
{
    if (!m_advancePending || m_historyWorker || m_checkingResults || m_preparing || m_startPending >= 0
        || (m_service && m_service->isBusy())) return;
    if (m_historyDirty) { persistHistory(); return; }
    if (!m_historyWritable || m_cancelRequested || m_shuttingDown || !m_service) { settleBatch(); return; }
    startNextTranslation();
}
void SubtitleTranslationWidget::startNextTranslation()
{
    if (!m_batchRunning || m_activeRow >= 0 || !m_service || m_service->isBusy()) return;
    m_advancePending = false;
    int row = -1;
    while (m_batchCursor < m_batchItems.size() && row < 0) {
        const auto id = m_batchItems.at(m_batchCursor++);
        for (int index = 0; index < m_items.size(); ++index) {
            const auto &item = m_items.at(index);
            if (item.id == id && !item.succeeded
                && (item.state == QStringLiteral("pending") || (m_retryBatch && item.retryable))) { row = index; break; }
        }
    }
    if (row < 0) { settleBatch(); return; }
    ++m_batchStarted;
    const auto settings = m_startSettings;
    const auto &item = m_items.at(row);
    SubtitleTranslationRequest request{item.sourcePath, item.audioPath, item.libraryRoot, item.libraryRelativePath, settings, {}, m_databasePath};
    request.taskId = item.translationTaskId;
    request.outputBatchId = item.outputBatchId; request.outputFileSuffix = item.outputFileSuffix;
    request.useWorkContext = m_batchUseWorkContext;
    request.workId = item.workId; request.audioFileId = item.audioFileId; request.subtitleFileId = item.subtitleFileId;
    if (request.useWorkContext) request.expectedReferenceDigest = item.referenceDigest;
    m_startSettings = settings; m_preparing = true; m_activeRow = row;
    m_table->selectRow(row);
    m_items[row].status = QStringLiteral("准备翻译"); m_items[row].error.clear(); updateRow(row);
    if (m_picker) m_picker->close();
    QPointer<SubtitleTranslationWidget> guard(this);
    const bool accepted = m_service->prepare(request);
    if (!guard) return;
    if (!accepted) { failUnstartedTranslation(QStringLiteral("翻译服务正忙，请稍后重试。")); return; }
    updateControls();
}
void SubtitleTranslationWidget::settleBatch()
{
    const bool notify = m_batchRunning;
    m_batchRunning = false; m_advancePending = false; m_batchItems.clear(); m_batchCursor = 0;
    const auto results = std::exchange(m_batchResults, {});
    if (notify && m_batchTotal > 1)
        setStatus(m_status->text() + QStringLiteral(" 本批成功 %1/%2 项。").arg(results.size()).arg(m_batchTotal));
    updateControls();
    QPointer<SubtitleTranslationWidget> guard(this);
    if (notify) emit batchCompleted(results);
    if (!guard) return;
    emit busyChanged(isBusy());
    if (guard && m_contextRefreshPending && !m_shuttingDown) refreshWorkContextAvailability();
    if (guard) finishShutdownIfReady();
}
void SubtitleTranslationWidget::persistPreparedTranslation(const SubtitleTranslationPlan &plan)
{
    const int row = m_activeRow;
    auto &item = m_items[row];
    item.status = QStringLiteral("正在保存执行记录"); item.error.clear(); item.state = QStringLiteral("running");
    item.resultMissing = false;
    item.useWorkContext = plan.request.useWorkContext;
    item.contextKind = plan.contextKind; item.warnings = plan.warnings;
    item.workId = plan.request.workId; item.audioFileId = plan.request.audioFileId; item.subtitleFileId = plan.request.subtitleFileId;
    item.contextGroup = plan.contextGroup; item.referenceDigest = plan.referenceDigest;
    if (plan.request.useWorkContext) item.correctionFingerprint = plan.fingerprint;
    item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto &settings = plan.request.settings;
    AiTaskAttempt attempt;
    attempt.id = QUuid::createUuid().toString(QUuid::WithoutBraces); attempt.taskId = item.id;
    attempt.state = QStringLiteral("running"); attempt.startedAt = item.updatedAt;
    attempt.parameters = QJsonObject{{QStringLiteral("schema"), 1},
        {QStringLiteral("provider"), translationApiProviderKey(settings.provider)},
        {QStringLiteral("endpoint"), translationChatCompletionsUrl(settings).toString()},
        {QStringLiteral("model"), settings.model}, {QStringLiteral("timeoutSeconds"), settings.timeoutSeconds},
        {QStringLiteral("thinkingEnabled"), settings.thinkingEnabled},
        {QStringLiteral("translationMode"), plan.mode == SubtitleTranslationMode::ToolConversation
            ? QStringLiteral("tool-conversation") : QStringLiteral("independent-batches")},
        {QStringLiteral("fingerprint"), plan.fingerprint},
        {QStringLiteral("useWorkContext"), plan.request.useWorkContext},
        {QStringLiteral("workId"), item.workId}, {QStringLiteral("audioFileId"), item.audioFileId},
        {QStringLiteral("subtitleFileId"), item.subtitleFileId}, {QStringLiteral("referenceDigest"), item.referenceDigest},
        {QStringLiteral("contextGroup"), item.contextGroup}, {QStringLiteral("correctionFingerprint"), item.correctionFingerprint},
        {QStringLiteral("sourceSha256"), QString::fromLatin1(plan.sourceSha256.toHex())},
        {QStringLiteral("checkpointDirectory"), plan.checkpointDirectory},
        {QStringLiteral("outputBatchId"), item.outputBatchId}, {QStringLiteral("outputFileSuffix"), item.outputFileSuffix},
        {QStringLiteral("batchCount"), plan.batches.size()}};
    if (item.useWorkContext && item.contextKind == SubtitleWorkContextKind::Asr)
        attempt.parameters.insert(QStringLiteral("contextKind"), QStringLiteral("asr"));
    m_history.attempts.append(attempt); m_currentAttempt = attempt.id; m_startPending = row; m_preparing = false;
    updateRow(row); scheduleHistorySave(); updateControls();
}
void SubtitleTranslationWidget::cancelTranslation()
{
    if (!isBusy() || !m_service) return;
    m_cancelRequested = true; m_advancePending = false;
    if (m_contextChecking && m_contextStartAfterCheck && m_contextCancelled) m_contextCancelled->store(true);
    // The cursor has already passed every started item, including failures.
    // Before the result check finishes, only the explicit action's eligible
    // rows belong to this run; Start does not include older failed tasks.
    const int firstUnstarted = m_batchRunning ? m_batchCursor : 0;
    for (int index = firstUnstarted; index < m_batchItems.size(); ++index) {
        for (int row = 0; row < m_items.size(); ++row) {
            auto &item = m_items[row];
            if (item.id != m_batchItems.at(index) || row == m_activeRow || item.succeeded) continue;
            if (item.state == QStringLiteral("pending") || (m_retryBatch && item.retryable)) {
                item.state = QStringLiteral("cancelled"); item.status = QStringLiteral("已取消"); item.retryable = true;
                item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                updateRow(row); scheduleHistorySave();
            }
            break;
        }
    }
    if (m_checkingResults) {
        m_startAfterResultCheck = false; m_activeRow = -1;
        setStatus(QStringLiteral("已取消，等待本地核对结束；尚未发送请求。")); return;
    }
    if (m_contextChecking && m_contextStartAfterCheck) {
        setStatus(QStringLiteral("已取消，等待作品上下文核验结束；尚未发送请求。")); return;
    }
    if (m_startPending >= 0) {
        const int row = m_startPending; m_startPending = -1; m_activeRow = -1; m_preparing = false;
        auto &item = m_items[row]; item.state = QStringLiteral("cancelled"); item.status = QStringLiteral("已取消");
        item.retryable = true; item.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        SubtitleTranslationResult result; result.task.state = AiSubtitleTaskState::Cancelled;
        finishAttempt(result); updateRow(row); scheduleHistorySave(); updateControls();
        setStatus(QStringLiteral("已取消，尚未发送请求。")); settleBatch(); return;
    }
    if (!m_preparing && !m_service->isBusy()) { settleBatch(); return; }
    setStatus(QStringLiteral("正在取消，等待当前本地操作结束…"));
    QPointer<SubtitleTranslationWidget> guard(this); m_service->cancel(); if (guard) updateControls();
}
void SubtitleTranslationWidget::setStatus(const QString &message)
{
    m_status->setText(message);
    m_status->setVisible(!message.isEmpty());
}
void SubtitleTranslationWidget::updateControls()
{
    const bool busy = isBusy(); const int row = selectedRow(); const bool selected = row >= 0;
    const bool ready = !m_shuttingDown && !busy && !m_historyLoading && m_historyWritable && m_service;
    const auto settings = busy ? m_startSettings : currentSettings();
    const bool deepSeek = settings.provider == TranslationApiProvider::DeepSeek;
    m_serviceLabel->setText(deepSeek ? translationApiModelDisplayName(settings.model)
        : QStringLiteral("自定义兼容服务 · %1").arg(settings.model));
    m_configure->setVisible(!validateTranslationApiSettings(settings).isEmpty());
    {
        const QSignalBlocker blocker(m_thinking);
        m_thinking->setChecked(translationApiThinkingEnabled(settings));
    }
    m_thinking->setVisible(deepSeek);
    m_thinking->setEnabled(ready && deepSeek && m_settings);
    m_thinking->setToolTip(QStringLiteral("自动保存，下次启动翻译时应用；本批次开始后固定。思考模式请求超时至少 180 秒。"));
    const bool startable = ready && !m_items.isEmpty()
        && validateTranslationApiSettings(settings).isEmpty();
    bool pendingContextValid = true, retryContextValid = true;
    bool pendingContextKnown = true, retryContextKnown = true;
    int pendingCount = 0, retryCount = 0, succeeded = 0, failed = 0, cancelled = 0;
    QStringList contextErrors;
    for (const auto &item : std::as_const(m_items)) {
        succeeded += item.succeeded;
        failed += item.state == QStringLiteral("failed");
        cancelled += item.state == QStringLiteral("cancelled");
        if (item.succeeded) continue;
        if (item.state == QStringLiteral("pending")) {
            ++pendingCount;
            pendingContextKnown = pendingContextKnown && item.contextChecked;
        }
        if (item.state == QStringLiteral("pending") || item.retryable) {
            ++retryCount;
            retryContextKnown = retryContextKnown && item.contextChecked;
        }
        if (!item.contextEligible) {
            if (item.state == QStringLiteral("pending")) pendingContextValid = false;
            if (item.state == QStringLiteral("pending") || item.retryable) retryContextValid = false;
            if (!item.contextError.isEmpty()) contextErrors.append(QFileInfo(item.sourcePath).fileName() + QStringLiteral("：") + item.contextError);
        }
    }
    // Start runs pending rows; Retry also includes failed/cancelled rows.
    // A cursor selection never changes this batch-wide option's scope.
    const bool hasContextCandidates = pendingCount > 0 || retryCount > 0;
    const bool contextKnown = pendingCount > 0 ? pendingContextKnown : retryContextKnown;
    const bool contextValid = pendingCount > 0 ? pendingContextValid : retryContextValid;
    const bool checkingContext = m_contextChecking || m_contextRefreshPending || !contextKnown;
    const bool contextAllowed = deepSeek && hasContextCandidates && contextValid && !checkingContext;
    if (busy) {
        const QSignalBlocker blocker(m_useWorkContext);
        m_useWorkContext->setChecked(m_batchUseWorkContext);
    } else if (!m_historyLoading && (!deepSeek || !hasContextCandidates
        || (!checkingContext && !contextValid)) && m_useWorkContext->isChecked()) {
        const QSignalBlocker blocker(m_useWorkContext);
        m_useWorkContext->setChecked(false);
        if (m_settings) m_settings->setValue(QStringLiteral("ai/subtitleTranslation/useWorkContext"), false);
    }
    m_useWorkContext->setEnabled(ready && contextAllowed);
    m_useWorkContext->setToolTip(!deepSeek ? QStringLiteral("作品上下文仅支持 DeepSeek。")
        : !hasContextCandidates ? QStringLiteral("添加已入库的日语 ASR 字幕后可使用作品上下文。")
        : checkingContext ? QStringLiteral("正在核验在库 ASR 和作品上下文。")
        : !contextErrors.isEmpty() ? contextErrors.join(QLatin1Char('\n'))
        : QStringLiteral("对整批字幕先校正日语再翻译中文，分别生成独立文件。优先使用已缓存参考文件，无可用正文时使用作品内 ASR。"));
    const bool startContextReady = !m_useWorkContext->isChecked()
        || (deepSeek && pendingContextValid && pendingContextKnown && !checkingContext);
    const bool retryContextReady = !m_useWorkContext->isChecked()
        || (deepSeek && retryContextValid && retryContextKnown && !checkingContext);
    m_table->setEnabled(!busy && !m_shuttingDown); m_add->setEnabled(ready);
    m_clear->setEnabled(ready && !m_items.isEmpty());
    m_start->setEnabled(startable && startContextReady);
    const bool retryable = std::any_of(m_items.cbegin(), m_items.cend(), [](const auto &item) { return item.retryable; });
    m_retry->setVisible(retryable);
    m_retry->setEnabled(startable && retryable && retryContextReady);
    m_cancel->setVisible(busy); m_cancel->setEnabled(busy && !m_shuttingDown); m_configure->setEnabled(ready);
    const bool result = selected && !m_items.at(row).resultPath.isEmpty();
    const bool directory = selected && (result || !m_items.at(row).taskDirectory.isEmpty());
    m_openFolder->setText(result ? QStringLiteral("打开结果文件夹") : QStringLiteral("打开任务目录"));
    m_folderAction->setText(result ? QStringLiteral("打开字幕所在文件夹") : QStringLiteral("打开任务目录"));
    m_openFolder->setVisible(directory); m_openFolder->setEnabled(ready && directory); m_folderAction->setEnabled(ready && directory);
    m_removeAction->setEnabled(ready && selected);
    m_errorAction->setEnabled(ready && selected && !m_items.at(row).error.isEmpty());
    m_progress->setVisible(busy); updateGeometry();
    m_summary->setText(QStringLiteral("共 %1 项 · 完成 %2 · 失败 %3 · 已取消 %4")
        .arg(m_items.size()).arg(succeeded).arg(failed).arg(cancelled));
}
void SubtitleTranslationWidget::updateRow(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    const auto &item = m_items.at(row);
    auto *name = m_table->item(row, 0); auto *state = m_table->item(row, 1);
    if (!name) { name = new QTableWidgetItem; m_table->setItem(row, 0, name); }
    if (!state) { state = new QTableWidgetItem; m_table->setItem(row, 1, state); }
    name->setText(QFileInfo(item.sourcePath).fileName());
    name->setToolTip(item.sourcePath + (item.audioPath.isEmpty() ? QString() : QStringLiteral("\n关联音轨：") + item.audioPath));
    state->setText(item.status);
    QString details = item.error.isEmpty() ? item.resultPath
        + (item.correctedJapanesePath.isEmpty() ? QString() : QStringLiteral("\n日语校正：") + item.correctedJapanesePath) : item.error;
    QStringList warnings = item.warnings;
    if (item.useWorkContext) details += QStringLiteral("\n本次执行：") + contextDescription(item.contextKind);
    if (item.contextEligible && !item.succeeded) {
        details += QStringLiteral("\n可用上下文：") + contextDescription(item.availableContextKind);
        warnings.append(item.contextWarnings);
    }
    warnings.removeDuplicates();
    if (!warnings.isEmpty()) details += QLatin1Char('\n') + warnings.join(QLatin1Char('\n'));
    state->setToolTip(details.trimmed());
}
void SubtitleTranslationWidget::showError()
{
    const int row = selectedRow(); if (row < 0 || m_items.at(row).error.isEmpty()) return;
    if (m_errorDialog) m_errorDialog->close();
    auto *body = new QVBoxLayout; body->setContentsMargins(16, 12, 16, 12);
    auto *text = new QPlainTextEdit(this); text->setObjectName(QStringLiteral("subtitleTranslationErrorText")); text->setReadOnly(true);
    const auto &item = m_items.at(row);
    QString details = item.error + QStringLiteral("\n\n源字幕：") + item.sourcePath
        + (item.taskDirectory.isEmpty() ? QString() : QStringLiteral("\n任务目录：") + item.taskDirectory);
    if (item.useWorkContext) details += QStringLiteral("\n使用上下文：") + contextDescription(item.contextKind);
    if (!item.warnings.isEmpty()) details += QStringLiteral("\n\n警告：\n") + item.warnings.join(QLatin1Char('\n'));
    if (item.error.contains(QStringLiteral("检查点")) || item.error.contains(QStringLiteral("断点")))
        details += QStringLiteral("\n数据库：") + m_databasePath
            + QStringLiteral("\n\n断点恢复：先关闭软件并备份数据库，保留此错误信息供检查。修复后手动重试；重新翻译可能再次计费。程序不会自动删除或覆盖损坏的断点记录。");
    text->setPlainText(details);
    body->addWidget(text, 1);
    auto *close = new QPushButton(QStringLiteral("关闭"), this); body->addWidget(close, 0, Qt::AlignRight);
    auto *dialog = new TextDialog(QStringLiteral("字幕翻译失败"), body, this);
    dialog->setObjectName(QStringLiteral("subtitleTranslationErrorDialog")); m_errorDialog = dialog;
    connect(close, &QPushButton::clicked, dialog, &QDialog::close); dialog->open();
}
void SubtitleTranslationWidget::openResultFolder()
{
    const int row = selectedRow();
    if (row < 0) return;
    const auto &item = m_items.at(row);
    const QString directory = !item.resultPath.isEmpty() ? QFileInfo(item.resultPath).absolutePath() : item.taskDirectory;
    if (!directory.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}
void SubtitleTranslationWidget::clearList()
{
    if (isBusy() || m_shuttingDown || m_historyLoading || !m_historyWritable) return;
    m_items.clear(); m_table->setRowCount(0); scheduleHistorySave();
    refreshWorkContextAvailability();
    setStatus({}); updateControls();
}
bool SubtitleTranslationWidget::hasOpenDialog() const
{
    return (m_picker && m_picker->isVisible()) || (m_errorDialog && m_errorDialog->isVisible());
}
void SubtitleTranslationWidget::closeTransientDialogs()
{
    QPointer<SubtitleTranslationWidget> guard(this);
    if (m_picker) m_picker->close();
    if (!guard) return;
    if (m_errorDialog) m_errorDialog->close();
    if (!guard) return;
    // The user already started the operation; hiding its owner does not cancel
    // local preparation, a pending execution record or the network request.
}
void SubtitleTranslationWidget::shutdown()
{
    if (m_shuttingDown) { finishShutdownIfReady(); return; }
    m_shuttingDown = true;
    ++m_contextGeneration;
    if (m_contextCancelled) m_contextCancelled->store(true);
    QPointer<SubtitleTranslationWidget> guard(this);
    closeTransientDialogs();
    if (!guard) return;
    if (isBusy()) cancelTranslation();
    else if (m_service) m_service->cancel();
    if (!guard) return;
    if (m_historyDirty && !m_historyLoading && !m_historyWorker) persistHistory();
    updateControls(); finishShutdownIfReady();
}
void SubtitleTranslationWidget::finishShutdownIfReady()
{
    if (m_shuttingDown && !isBusy() && !m_contextChecking && !m_checkingResults && !m_historyLoading && !m_historyWorker
        && (!m_historyDirty || !m_historyWritable) && !m_shutdownEmitted) {
        m_historyLease.reset(); m_shutdownEmitted = true; emit shutdownFinished();
    }
}
} // namespace asmr
