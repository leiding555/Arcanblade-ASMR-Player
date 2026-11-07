#include "ui/WorkReferenceDialog.h"
#include "library/WorkReferenceRepository.h"

#include <QAbstractItemView>
#include <QAction>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonArray>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPromise>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
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
constexpr qsizetype PreviewPageSize = 16 * 1024;

QString stateText(const WorkReferenceRecord &record)
{
    if (record.missing || record.state == QStringLiteral("missing")) return QStringLiteral("缺失");
    if (record.state == QStringLiteral("ready")) return QStringLiteral("可用");
    if (record.state == QStringLiteral("no-text")) return QStringLiteral("无文字");
    return QStringLiteral("提取失败");
}

QString sourceText(const QString &source)
{
    if (source == QStringLiteral("generated-asr")) return QStringLiteral("本软件 ASR");
    if (source == QStringLiteral("external-subtitle")) return QStringLiteral("外部字幕 · 来源未知");
    return QStringLiteral("文件资料");
}

class PreviewBuilder {
public:
    explicit PreviewBuilder(const std::atomic_bool &cancelled) : m_cancelled(cancelled) {}
    void line(const QString &text, const QString &prefix = {})
    {
        qsizetype offset = 0;
        do {
            if (m_cancelled.load()) return;
            if (m_current.size() + prefix.size() + 2 > PreviewPageSize) flush();
            const qsizetype available = PreviewPageSize - m_current.size() - prefix.size() - 1;
            qsizetype count = qMin(available, text.size() - offset);
            if (count > 0 && offset + count < text.size()
                && text.at(offset + count - 1).isHighSurrogate()) --count;
            if (count == 0 && offset < text.size()) { flush(); continue; }
            m_current += prefix + text.mid(offset, count) + QLatin1Char('\n');
            offset += count;
        } while (offset < text.size());
    }
    void text(const QString &text, bool numberLines, const QString &location = {})
    {
        qsizetype start = 0;
        int number = 1;
        for (;;) {
            if (m_cancelled.load()) return;
            const auto end = text.indexOf(QLatin1Char('\n'), start);
            line(end < 0 ? text.mid(start) : text.mid(start, end - start),
                location + (numberLines ? QString::number(number++) + QStringLiteral("  ") : QString()));
            if (end < 0) break;
            start = end + 1;
        }
    }
    QStringList finish() { if (!m_current.isEmpty()) flush(); return std::move(m_pages); }
private:
    void flush() { if (!m_current.isEmpty()) m_pages.append(std::exchange(m_current, {})); }
    const std::atomic_bool &m_cancelled;
    QString m_current;
    QStringList m_pages;
};
}

WorkReferenceDialog::WorkReferenceDialog(QString databasePath, LibraryWork work, QWidget *parent)
    : QDialog(parent), m_databasePath(std::move(databasePath)), m_work(std::move(work))
{
    setObjectName(QStringLiteral("workReferenceDialog"));
    setWindowTitle(QStringLiteral("作品参考资料"));
    resize(900, 680);
    setMinimumSize(620, 440);
    m_workLabel = new QLabel(this);
    m_workLabel->setTextFormat(Qt::PlainText);
    m_workLabel->setWordWrap(true);
    m_workLabel->setText(m_work.displayName);
    m_feedbackLabel = new QLabel(this);
    m_feedbackLabel->setObjectName(QStringLiteral("workReferenceFeedback"));
    m_feedbackLabel->setTextFormat(Qt::PlainText);
    m_feedbackLabel->setWordWrap(true);
    m_table = new QTableWidget(0, 4, this);
    m_table->setObjectName(QStringLiteral("workReferenceTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("相对路径"),
        QStringLiteral("格式 / 来源"), QStringLiteral("编码")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);
    m_contextMenu = new QMenu(m_table);
    m_contextMenu->setObjectName(QStringLiteral("workReferenceContextMenu"));
    m_openDirectoryAction = m_contextMenu->addAction(QStringLiteral("打开所在目录"));
    m_openDirectoryAction->setObjectName(QStringLiteral("workReferenceOpenDirectoryAction"));
    connect(m_openDirectoryAction, &QAction::triggered, this, &WorkReferenceDialog::openContextDirectory);
    connect(m_contextMenu, &QMenu::aboutToHide, this, [this] {
        const auto revision = m_menuRevision;
        // QMenu hides before dispatching the selected action.
        QTimer::singleShot(0, this, [this, revision] {
            if (revision == m_menuRevision) invalidateContextMenu();
        });
    });
    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("workReferencePreview"));
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(m_table); splitter->addWidget(m_preview);
    splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 2);
    m_update = new QPushButton(QStringLiteral("更新资料"), this);
    m_update->setObjectName(QStringLiteral("workReferenceUpdateButton"));
    m_update->setToolTip(QStringLiteral("更新作品目录，并复用未变化资料的缓存。"));
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName(QStringLiteral("workReferenceCloseButton"));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_update);
    buttons->addStretch(); buttons->addWidget(close);
    m_previous = new QPushButton(QStringLiteral("上一页"), this);
    m_previous->setObjectName(QStringLiteral("workReferencePreviousButton"));
    m_next = new QPushButton(QStringLiteral("下一页"), this);
    m_next->setObjectName(QStringLiteral("workReferenceNextButton"));
    m_pageLabel = new QLabel(this);
    m_pageLabel->setObjectName(QStringLiteral("workReferencePageLabel"));
    auto *pages = new QHBoxLayout;
    pages->addStretch(); pages->addWidget(m_previous); pages->addWidget(m_pageLabel); pages->addWidget(m_next);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_workLabel); layout->addWidget(m_feedbackLabel);
    layout->addWidget(splitter, 1); layout->addLayout(pages); layout->addLayout(buttons);
    for (auto *button : {m_update, close, m_previous, m_next})
        button->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_update, &QPushButton::clicked, this, [this] { emit updateRequested(false); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &WorkReferenceDialog::selectRecord);
    connect(m_previous, &QPushButton::clicked, this, [this] { if (m_page > 0) { --m_page; showPage(); } });
    connect(m_next, &QPushButton::clicked, this, [this] { if (m_page + 1 < m_pages.size()) { ++m_page; showPage(); } });
    updateControls();
    QTimer::singleShot(0, this, &WorkReferenceDialog::refresh);
}

WorkReferenceDialog::~WorkReferenceDialog()
{
    if (m_cancelled) m_cancelled->store(true);
    if (m_active) m_lifetime.waitForFinished();
}

void WorkReferenceDialog::setWork(const LibraryWork &work)
{
    if (m_shuttingDown) return;
    m_work = work; m_invalid = false;
    m_updateFeedback.clear();
    m_workLabel->setText(work.displayName);
    refresh();
}

void WorkReferenceDialog::refresh()
{
    if (m_shuttingDown || m_invalid) return;
    invalidateContextMenu();
    m_loading = true;
    m_pages.clear(); m_page = 0; showPage();
    setFeedback(QStringLiteral("正在读取参考资料清单…"));
    request({++m_generation, JobKind::Load, m_work, {}});
}

void WorkReferenceDialog::setBusy(bool busy)
{
    if (busy && !m_busy) ++m_openSession;
    m_busy = busy;
    if (busy) invalidateContextMenu();
    updateControls();
}
void WorkReferenceDialog::setFeedback(const QString &message)
{
    m_feedbackText = message;
    const QString displayed = m_updateFeedback.isEmpty() ? message : message + QLatin1Char('\n') + m_updateFeedback;
    m_feedbackLabel->setText(displayed);
    m_feedbackLabel->setToolTip(displayed);
}

void WorkReferenceDialog::setUpdateFeedback(const QString &message)
{
    m_updateFeedback = message;
    setFeedback(m_feedbackText);
}

void WorkReferenceDialog::invalidate()
{
    invalidateContextMenu();
    m_invalid = true; ++m_generation; m_pending.reset();
    if (m_cancelled) m_cancelled->store(true);
    m_loading = false; m_snapshot = {}; m_pages.clear(); m_page = 0; m_table->setRowCount(0);
    showPage();
    setFeedback(QStringLiteral("作品已删除或身份已变化，请重新打开参考资料。"));
    updateControls();
}

void WorkReferenceDialog::request(Job job)
{
    if (m_shuttingDown || m_invalid) return;
    if (m_active) {
        m_cancelled->store(true);
        m_pending = std::move(job);
    } else start(std::move(job));
    updateControls();
}

void WorkReferenceDialog::start(Job job)
{
    m_active = true;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    auto promise = std::make_shared<QPromise<JobResult>>();
    auto *watcher = new QFutureWatcher<JobResult>(this);
    connect(watcher, &QFutureWatcher<JobResult>::finished, this, [this, watcher, job] {
        auto result = watcher->result();
        watcher->deleteLater(); m_active = false; m_cancelled.reset();
        if (!m_shuttingDown && !m_invalid && job.generation == m_generation
            && job.work.id == m_work.id && job.work.rootPath == m_work.rootPath
            && job.work.importedAtMs == m_work.importedAtMs) {
            if (job.kind == JobKind::Load) {
                m_loading = false;
                applySnapshot(std::move(result.snapshot));
            } else if (job.kind == JobKind::Preview) {
                m_pages = std::move(result.pages); m_page = 0; showPage();
            } else if (job.openSession != m_openSession || !isVisible()) {
                // Opening a directory is a UI action; hidden/reopened windows
                // cannot accept an earlier session's completed request.
            } else if (!result.directory.isEmpty()) {
                if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.directory)))
                    setFeedback(QStringLiteral("无法打开资料所在目录。"));
            } else setFeedback(result.error);
        }
        if (!m_shuttingDown && m_pending && !m_active) {
            auto pending = std::move(*m_pending); m_pending.reset(); start(std::move(pending));
        }
        updateControls();
        finishShutdownIfReady();
    });
    m_lifetime = promise->future(); watcher->setFuture(m_lifetime);
    const auto cancelled = m_cancelled;
    const auto hook = m_loadTestHook;
    const auto directoryHook = m_directoryTestHook;
    QThreadPool::globalInstance()->start([promise, cancelled, hook, directoryHook, job = std::move(job), database = m_databasePath] {
        promise->start();
        JobResult result;
        try {
            if (!cancelled->load()) {
                if (job.kind == JobKind::Load) {
                    if (hook) result.snapshot = (*hook)(job.work);
                    else result.snapshot = WorkReferenceRepository(database).load(job.work);
                } else if (job.kind == JobKind::Preview) result.pages = renderPreview(job.record, *cancelled);
                else {
                    result.directory = directoryHook ? (*directoryHook)(job.work, job.record)
                                                     : checkedDirectory(job.work, job.record);
                    if (result.directory.isEmpty()) result.error = QStringLiteral("资料目录不存在、不在作品内或包含链接，未打开。" );
                }
            }
        } catch (const std::exception &) {
            result.snapshot.error = QStringLiteral("参考资料读取失败，请重试。"); result.error = result.snapshot.error;
        } catch (...) {
            result.snapshot.error = QStringLiteral("参考资料读取失败，请重试。"); result.error = result.snapshot.error;
        }
        promise->addResult(std::move(result)); promise->finish();
    });
}

void WorkReferenceDialog::applySnapshot(WorkReferenceSnapshot snapshot)
{
    invalidateContextMenu();
    m_snapshot = std::move(snapshot);
    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(0);
    if (!m_snapshot.success) { setFeedback(m_snapshot.error); return; }
    setFeedback(!m_snapshot.scanned ? QStringLiteral("尚未扫描参考资料，点击“更新资料”开始。")
        : m_snapshot.records.isEmpty() ? QStringLiteral("扫描完成，没有参考资料。")
        : QStringLiteral("共 %1 份参考资料 · 最近同步 %2").arg(m_snapshot.records.size()).arg(m_snapshot.synchronizedAt));
    m_table->setRowCount(static_cast<int>(m_snapshot.records.size()));
    for (int row = 0; row < m_snapshot.records.size(); ++row) {
        const auto &record = m_snapshot.records.at(row);
        const QString encoding = record.encodingInferred ? QStringLiteral("推定 ") + record.encoding : record.encoding;
        const QStringList values{QFileInfo(record.relativePath).fileName(), record.relativePath,
            record.format.toUpper() + QStringLiteral(" / ") + sourceText(record.source), encoding};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setToolTip(record.relativePath + QLatin1Char('\n') + record.audioRelativePath
                + QLatin1Char('\n') + record.error + QLatin1Char('\n') + record.warning);
            m_table->setItem(row, column, item);
        }
    }
    if (!m_snapshot.records.isEmpty()) {
        m_table->selectRow(0);
        QTimer::singleShot(0, this, &WorkReferenceDialog::selectRecord);
    }
}

void WorkReferenceDialog::selectRecord()
{
    invalidateContextMenu();
    if (m_shuttingDown || m_invalid || m_loading) return;
    const int row = m_table->currentRow();
    m_pages.clear(); m_page = 0; showPage();
    if (row < 0 || row >= m_snapshot.records.size()) { updateControls(); return; }
    const auto &record = m_snapshot.records.at(row);
    QStringList details;
    if (!record.error.isEmpty()) details.append(record.error);
    if (!record.warning.isEmpty()) details.append(record.warning);
    if (record.encodingInferred) details.append(QStringLiteral("编码为自动推定，解码成功不保证原始编码判断正确。"));
    if (!details.isEmpty()) setFeedback(details.join(QLatin1Char('\n')));
    request({++m_generation, JobKind::Preview, m_work, record});
}

QStringList WorkReferenceDialog::renderPreview(const WorkReferenceRecord &record, const std::atomic_bool &cancelled)
{
    PreviewBuilder builder(cancelled);
    if (record.state != QStringLiteral("ready") || record.missing) {
        builder.line(record.error.isEmpty() ? stateText(record) : record.error);
    } else if (record.content.contains(QStringLiteral("pages"))) {
        for (const auto value : record.content.value(QStringLiteral("pages")).toArray()) {
            const auto page = value.toObject();
            builder.line(QStringLiteral("[PDF 第 %1 页]").arg(page.value(QStringLiteral("page")).toInt()));
            builder.text(page.value(QStringLiteral("text")).toString(), true,
                QStringLiteral("P%1:").arg(page.value(QStringLiteral("page")).toInt()));
            if (cancelled.load()) break;
        }
    } else if (record.content.contains(QStringLiteral("cues"))) {
        for (const auto value : record.content.value(QStringLiteral("cues")).toArray()) {
            const auto cue = value.toObject();
            const auto location = QStringLiteral("#%1  [%2 – %3 ms]  ").arg(cue.value(QStringLiteral("id")).toInt())
                .arg(cue.value(QStringLiteral("start_ms")).toInteger()).arg(cue.value(QStringLiteral("end_ms")).toInteger());
            builder.text(cue.value(QStringLiteral("text")).toString(), false, location);
            if (cancelled.load()) break;
        }
    } else builder.text(record.content.value(QStringLiteral("text")).toString(), true);
    return cancelled.load() ? QStringList{} : builder.finish();
}

QString WorkReferenceDialog::checkedDirectory(const LibraryWork &work, const WorkReferenceRecord &record)
{
    const QString relative = QDir::fromNativeSeparators(record.relativePath);
    if (relative.isEmpty() || relative.contains(QChar::Null) || QDir::isAbsolutePath(relative)) return {};
    const auto parts = relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const auto &part : parts)
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.contains(QLatin1Char(':'))) return {};
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(work.rootPath));
    if (!QDir::isAbsolutePath(root)) return {};
    const QString directory = QFileInfo(QDir(root).filePath(relative)).absolutePath();
    QString current = directory;
    for (;;) {
        const QFileInfo info(current);
        if (!info.isDir() || info.isSymLink() || info.isJunction()) return {};
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
#endif
        const QString parent = info.absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return directory;
}

void WorkReferenceDialog::showPage()
{
    m_preview->setPlainText(m_pages.isEmpty() ? QString() : m_pages.at(m_page));
    m_pageLabel->setText(m_pages.isEmpty() ? QStringLiteral("0 / 0") : QStringLiteral("%1 / %2").arg(m_page + 1).arg(m_pages.size()));
    updateControls();
}

void WorkReferenceDialog::updateControls()
{
    const bool enabled = !m_shuttingDown && !m_invalid;
    m_table->setEnabled(enabled && !m_loading);
    m_update->setEnabled(enabled && !m_busy && !m_active);
    m_openDirectoryAction->setEnabled(enabled && !m_busy && !m_active && !m_loading && m_menuTarget.has_value());
    m_previous->setEnabled(enabled && !m_active && m_page > 0 && !m_pages.isEmpty());
    m_next->setEnabled(enabled && !m_active && m_page + 1 < m_pages.size());
}

void WorkReferenceDialog::shutdown()
{
    if (!m_shuttingDown) {
        invalidateContextMenu();
        m_shuttingDown = true; ++m_generation; m_pending.reset();
        if (m_cancelled) m_cancelled->store(true);
        updateControls();
    }
    finishShutdownIfReady();
}

bool WorkReferenceDialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_table || watched == m_table->viewport()) && event->type() == QEvent::ContextMenu) {
        invalidateContextMenu();
        if (m_shuttingDown || m_invalid || m_busy || m_loading || !isVisible()) return true;
        const auto *context = static_cast<QContextMenuEvent *>(event);
        const bool keyboard = context->reason() == QContextMenuEvent::Keyboard;
        const auto index = keyboard ? m_table->currentIndex()
            : m_table->indexAt(m_table->viewport()->mapFromGlobal(context->globalPos()));
        if (!index.isValid() || index.row() >= m_snapshot.records.size()) return true;
        m_table->setCurrentCell(index.row(), 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        ++m_menuRevision;
        m_menuTarget = Job{m_generation, JobKind::OpenDirectory, m_work, m_snapshot.records.at(index.row()), m_openSession};
        updateControls();
        m_contextMenu->popup(keyboard
            ? m_table->viewport()->mapToGlobal(m_table->visualRect(index).center()) : context->globalPos());
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void WorkReferenceDialog::hideEvent(QHideEvent *event)
{
    invalidateContextMenu();
    ++m_openSession;
    QDialog::hideEvent(event);
}

void WorkReferenceDialog::invalidateContextMenu()
{
    ++m_menuRevision;
    m_menuTarget.reset();
    if (m_openDirectoryAction) m_openDirectoryAction->setEnabled(false);
    if (m_contextMenu) m_contextMenu->close();
}

void WorkReferenceDialog::openContextDirectory()
{
    if (!m_menuTarget || m_shuttingDown || m_invalid || m_busy || m_active || m_loading || !isVisible()) return;
    auto job = *m_menuTarget;
    const int row = m_table->currentRow();
    if (job.generation != m_generation || job.openSession != m_openSession || row < 0
        || row >= m_snapshot.records.size() || job.work.id != m_work.id
        || job.work.rootPath != m_work.rootPath || job.work.importedAtMs != m_work.importedAtMs
        || job.record.id != m_snapshot.records.at(row).id
        || job.record.relativePath != m_snapshot.records.at(row).relativePath) return;
    invalidateContextMenu();
    job.generation = ++m_generation;
    request(std::move(job));
}

void WorkReferenceDialog::done(int result)
{
    m_requestedResult = result;
    hide(); shutdown();
}

void WorkReferenceDialog::finishShutdownIfReady()
{
    if (!m_shuttingDown || !isIdle() || m_shutdownNotified) return;
    m_shutdownNotified = true;
    QPointer<WorkReferenceDialog> guard(this);
    emit shutdownFinished();
    if (guard && m_requestedResult) QDialog::done(*m_requestedResult);
}
} // namespace asmr
