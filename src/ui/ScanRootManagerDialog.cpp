#include "ui/ScanRootManagerDialog.h"
#include "ui/DialogFrame.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QHash>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>

namespace asmr {
namespace {
QString safeTooltip(const QString &text)
{
    return QStringLiteral("<qt>%1</qt>").arg(
        text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
}
} // namespace

class ScanRootsModel final : public QAbstractTableModel
{
public:
    explicit ScanRootsModel(QObject *parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_roots.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 2;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        if (section == 0) return QStringLiteral("目录");
        if (section == 1) return QStringLiteral("状态");
        return {};
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.model() != this || index.row() < 0
            || index.row() >= m_roots.size() || index.column() < 0 || index.column() >= 2)
            return {};
        const auto &root = m_roots.at(index.row());
        const auto status = m_statuses.value(root);
        if (role == ScanRootManagerDialog::RootPathRole)
            return root;
        if (role == Qt::ToolTipRole)
            return safeTooltip(root + QLatin1Char('\n') + status.details);
        if (role == Qt::AccessibleTextRole)
            return root + QStringLiteral("，") + status.summary + QStringLiteral("，") + status.details;
        if (role == Qt::DisplayRole)
            return index.column() == 0 ? root : status.summary;
        return {};
    }
    void setRoots(const QStringList &roots)
    {
        if (roots == m_roots)
            return;
        beginResetModel();
        m_roots = roots;
        for (auto it = m_statuses.begin(); it != m_statuses.end();) {
            if (!m_roots.contains(it.key())) it = m_statuses.erase(it);
            else ++it;
        }
        endResetModel();
    }
    void setStatus(const QString &root, const QString &details, const QString &summary,
                   bool reviewRequired)
    {
        const auto row = m_roots.indexOf(root);
        const Status status{details, summary.isEmpty() ? details : summary, reviewRequired};
        if (row < 0 || m_statuses.value(root) == status)
            return;
        m_statuses.insert(root, status);
        emit dataChanged(index(static_cast<int>(row), 0), index(static_cast<int>(row), 1),
            {Qt::DisplayRole, Qt::ToolTipRole, Qt::AccessibleTextRole});
    }
    int rowForRoot(const QString &root) const
    {
        return static_cast<int>(m_roots.indexOf(root));
    }
    bool needsReview(const QString &root) const
    {
        return m_roots.contains(root) && m_statuses.value(root).reviewRequired;
    }
    QStringList rootsAwaitingReview() const
    {
        QStringList pending;
        for (const auto &root : m_roots)
            if (m_statuses.value(root).reviewRequired) pending.append(root);
        return pending;
    }
private:
    struct Status {
        QString details = QStringLiteral("尚未扫描");
        QString summary = QStringLiteral("尚未扫描");
        bool reviewRequired = false;
        bool operator==(const Status &) const = default;
    };
    QStringList m_roots;
    QHash<QString, Status> m_statuses;
};

ScanRootManagerDialog::ScanRootManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("scanRootManagerDialog"));
    setWindowTitle(QStringLiteral("扫描目录管理"));
    setWindowModality(Qt::NonModal);
    resize(880, 520);
    setMinimumSize(520, 360);

    m_startup = new QCheckBox(QStringLiteral("启动时自动扫描"), this);
    m_startup->setObjectName(QStringLiteral("scanRootsStartupCheckBox"));
    m_startup->setToolTip(QStringLiteral("首次读取作品库后在后台刷新已保存的扫描目录。"));
    m_model = new ScanRootsModel(this);
    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("scanRootsTable"));
    m_table->setAccessibleName(QStringLiteral("扫描目录及状态"));
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setWordWrap(false);
    m_table->setSortingEnabled(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setMinimumSectionSize(80);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setMinimumSize(0, 80);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);

    m_add = new QPushButton(QStringLiteral("添加目录"), this);
    m_add->setObjectName(QStringLiteral("scanRootAddButton"));
    m_add->setToolTip(QStringLiteral("选择总目录：其一级子文件夹分别作为作品，内部递归扫描音频与字幕。"));
    m_refreshAll = new QPushButton(QStringLiteral("刷新全部"), this);
    m_refreshAll->setObjectName(QStringLiteral("scanRootsRefreshAllButton"));
    auto *menu = new QMenu(this);
    m_contextMenu = menu;
    menu->setObjectName(QStringLiteral("scanRootsContextMenu"));
    menu->setToolTipsVisible(true);
    m_refresh = menu->addAction(QStringLiteral("刷新所选"));
    m_refresh->setObjectName(QStringLiteral("scanRootRefreshAction"));
    m_remove = menu->addAction(QStringLiteral("移除扫描目录"));
    m_remove->setObjectName(QStringLiteral("scanRootRemoveAction"));
    m_remove->setToolTip(QStringLiteral("仅停止扫描，保留作品和本地文件。"));
    menu->addSeparator();
    m_reviewOverlaps = menu->addAction(QStringLiteral("查看重叠项"));
    m_reviewOverlaps->setObjectName(QStringLiteral("scanRootReviewOverlapsAction"));
    m_reviewOverlaps->setToolTip(QStringLiteral("预览所选目录的父子目录重叠项，确认后再导入。"));
    menu->addSeparator();
    m_open = menu->addAction(QStringLiteral("打开目录"));
    m_open->setObjectName(QStringLiteral("scanRootOpenAction"));
    m_reviewPending = new QPushButton(this);
    m_reviewPending->setObjectName(QStringLiteral("scanRootsReviewPendingButton"));
    m_reviewPending->setToolTip(QStringLiteral("有重叠项等待确认，查看后选择需要导入的目录。"));
    m_busyLabel = new QLabel(QStringLiteral("正在处理，请稍候…"), this);
    m_busyLabel->setObjectName(QStringLiteral("scanRootsBusyLabel"));
    m_busyLabel->setTextFormat(Qt::PlainText);
    m_busyLabel->setMinimumWidth(0);
    m_busyLabel->setWordWrap(true);

    auto *topActions = new QHBoxLayout;
    topActions->addWidget(m_add);
    topActions->addWidget(m_refreshAll);
    topActions->addStretch();
    topActions->addWidget(m_startup);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("scanRootsCloseButton"));
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto *bottomActions = new QHBoxLayout;
    bottomActions->addWidget(m_reviewPending);
    bottomActions->addWidget(m_busyLabel, 1);
    bottomActions->addStretch();
    bottomActions->addWidget(buttons);
    auto *layout = new QVBoxLayout;
    layout->addLayout(topActions);
    layout->addWidget(m_table, 1);
    layout->addLayout(bottomActions);

    m_dialogFrame = new DialogFrame(this, layout);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this] { m_contextMenu->close(); updateActions(); });
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (!m_busy) emit addRootRequested();
    });
    connect(m_remove, &QAction::triggered, this, [this] {
        const QString root = selectedRoot();
        if (!m_busy && !root.isEmpty()) emit removeRootRequested(root);
    });
    connect(m_refresh, &QAction::triggered, this, [this] {
        const QString root = selectedRoot();
        if (!m_busy && !root.isEmpty()) emit refreshRootRequested(root);
    });
    connect(m_reviewOverlaps, &QAction::triggered, this, [this] {
        const QString root = selectedRoot();
        if (!m_busy && !root.isEmpty()) emit reviewOverlapsRequested(root);
    });
    connect(m_open, &QAction::triggered, this, [this] {
        const QString root = selectedRoot();
        if (!m_busy && !root.isEmpty()) emit openRootRequested(root);
    });
    connect(m_reviewPending, &QPushButton::clicked, this, [this] {
        if (m_busy) return;
        const auto pending = m_model->rootsAwaitingReview();
        if (pending.isEmpty()) return;
        const auto root = m_model->needsReview(selectedRoot()) ? selectedRoot() : pending.first();
        m_table->selectionModel()->setCurrentIndex(m_model->index(m_model->rowForRoot(root), 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        emit reviewOverlapsRequested(root);
    });
    connect(m_refreshAll, &QPushButton::clicked, this, [this] {
        if (!m_busy && m_model->rowCount() > 0) emit refreshAllRequested();
    });
    connect(m_startup, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!m_busy) emit startupScanChanged(enabled);
    });
    updateActions();
}

QString ScanRootManagerDialog::selectedRoot() const
{
    return m_table->currentIndex().data(RootPathRole).toString();
}

bool ScanRootManagerDialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_table || watched == m_table->viewport()) && event->type() == QEvent::ContextMenu) {
        auto *context = static_cast<QContextMenuEvent *>(event);
        if (m_busy) return true;
        QPoint position;
        if (context->reason() == QContextMenuEvent::Keyboard) {
            if (selectedRoot().isEmpty()) return true;
            position = m_table->viewport()->mapToGlobal(m_table->visualRect(m_table->currentIndex()).center());
        } else {
            const auto index = m_table->indexAt(m_table->viewport()->mapFromGlobal(context->globalPos()));
            if (!index.isValid()) { m_contextMenu->close(); return true; }
            m_table->selectionModel()->setCurrentIndex(index,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            position = context->globalPos();
        }
        updateActions();
        m_contextMenu->popup(position);
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void ScanRootManagerDialog::hideEvent(QHideEvent *event)
{
    m_contextMenu->close();
    QDialog::hideEvent(event);
    emit hidden();
}

bool ScanRootManagerDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}

void ScanRootManagerDialog::setConfiguration(const QStringList &roots, bool startupEnabled)
{
    m_contextMenu->close();
    const QString previousRoot = selectedRoot();
    const QSignalBlocker selectionBlocker(m_table->selectionModel());
    const QSignalBlocker startupBlocker(m_startup);
    m_model->setRoots(roots);
    m_startup->setChecked(startupEnabled);
    const int row = m_model->rowForRoot(previousRoot);
    if (row >= 0)
        m_table->selectionModel()->setCurrentIndex(m_model->index(row, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    else
        m_table->selectionModel()->clear();
    updateActions();
}

void ScanRootManagerDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (busy) m_contextMenu->close();
    updateActions();
}

void ScanRootManagerDialog::setRootStatus(const QString &root, const QString &text)
{
    setRootStatus(root, text, text);
}

void ScanRootManagerDialog::setRootStatus(const QString &root, const QString &details,
                                           const QString &summary, bool reviewRequired)
{
    m_model->setStatus(root, details, summary, reviewRequired);
    updateActions();
}

void ScanRootManagerDialog::updateActions()
{
    const bool selected = !selectedRoot().isEmpty();
    m_add->setEnabled(!m_busy);
    m_remove->setEnabled(!m_busy && selected);
    m_refresh->setEnabled(!m_busy && selected);
    m_reviewOverlaps->setEnabled(!m_busy && selected);
    m_open->setEnabled(!m_busy && selected);
    m_refreshAll->setEnabled(!m_busy && m_model->rowCount() > 0);
    m_startup->setEnabled(!m_busy);
    m_busyLabel->setVisible(m_busy);
    const auto pending = m_model->rootsAwaitingReview();
    m_reviewPending->setVisible(!pending.isEmpty());
    m_reviewPending->setEnabled(!m_busy && !pending.isEmpty());
    m_reviewPending->setText(QStringLiteral("查看待确认项（%1）").arg(pending.size()));
}

} // namespace asmr
