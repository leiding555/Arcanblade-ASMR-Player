#include "ui/BatchImportPreviewDialog.h"
#include "ui/DialogFrame.h"

#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QTableView>
#include <QVBoxLayout>

namespace asmr {
namespace {
QString safeTooltip(const QString &text)
{
    return QStringLiteral("<qt>%1</qt>").arg(
        text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
}

QLabel *plainLabel(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

class RootDirectoryLabel final : public QLabel
{
public:
    explicit RootDirectoryLabel(QWidget *parent) : QLabel(parent)
    {
        setObjectName(QStringLiteral("batchImportRootLabel"));
        setTextFormat(Qt::PlainText);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setDirectory(const QString &directory)
    {
        m_fullText = QStringLiteral("总目录：%1").arg(directory);
        setAccessibleName(m_fullText);
        setToolTip(safeTooltip(directory));
        updateText();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateText();
    }
    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            updateText();
    }
private:
    void updateText()
    {
        setText(fontMetrics().elidedText(m_fullText, Qt::ElideMiddle, contentsRect().width()));
    }
    QString m_fullText;
};

QString overlapDescription(const WorkDirectoryCandidate &candidate)
{
    if (candidate.overlappingWorks.isEmpty())
        return QStringLiteral("无父子目录重叠");
    QStringList entries;
    for (const auto &work : candidate.overlappingWorks)
        entries.push_back(QStringLiteral("%1（%2）").arg(work.displayName, work.rootPath));
    return QStringLiteral("与已收录作品存在父子目录重叠：%1；需显式勾选")
        .arg(entries.join(QStringLiteral("；")));
}

QString candidateStatus(const WorkDirectoryCandidate &candidate)
{
    QString status = candidate.existingWorkId > 0 ? QStringLiteral("更新作品") : QStringLiteral("新作品");
    if (!candidate.overlappingWorks.isEmpty())
        status += QStringLiteral(" · 目录重叠");
    return status;
}
} // namespace

// Values and check states only. A large preview never creates widgets per row.
class BatchImportCandidatesModel final : public QAbstractTableModel
{
public:
    explicit BatchImportCandidatesModel(QObject *parent) : QAbstractTableModel(parent) {}

    void setCandidates(const QVector<WorkDirectoryCandidate> &candidates, const QString &root, bool enabled)
    {
        beginResetModel();
        m_candidates = candidates;
        m_enabled = enabled;
        m_root = root;
        m_nameCounts.clear();
        m_checked.clear();
        m_overlapCount = 0;
        for (int row = 0; row < m_candidates.size(); ++row) {
            const auto &candidate = m_candidates.at(row);
            ++m_nameCounts[candidate.name.toCaseFolded()];
            if (!candidate.overlappingWorks.isEmpty())
                ++m_overlapCount;
            if (enabled && candidate.defaultSelected())
                m_checked.insert(row);
        }
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_candidates.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 2;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        const QStringList headers{QStringLiteral("作品目录"), QStringLiteral("状态")};
        return section >= 0 && section < headers.size() ? headers.at(section) : QVariant();
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!valid(index))
            return {};
        const auto &candidate = m_candidates.at(index.row());
        if (role == BatchImportPreviewDialog::DirectoryRole)
            return candidate.directory;
        if (role == BatchImportPreviewDialog::ExistingWorkIdRole)
            return candidate.existingWorkId;
        if (role == BatchImportPreviewDialog::HasOverlapRole)
            return !candidate.overlappingWorks.isEmpty();
        if (role == Qt::CheckStateRole && index.column() == 0)
            return m_checked.contains(index.row()) ? Qt::Checked : Qt::Unchecked;
        if (role == Qt::ToolTipRole || role == Qt::AccessibleTextRole) {
            const QString details = candidate.name + QLatin1Char('\n') + candidate.directory
                + QLatin1Char('\n') + candidateStatus(candidate)
                + QLatin1Char('\n') + overlapDescription(candidate);
            return role == Qt::ToolTipRole ? safeTooltip(details) : details;
        }
        if (role != Qt::DisplayRole)
            return {};
        if (index.column() == 1)
            return candidateStatus(candidate);
        if (m_nameCounts.value(candidate.name.toCaseFolded()) <= 1)
            return candidate.name;
        const QString path = m_root.isEmpty() ? candidate.directory
            : QDir(m_root).relativeFilePath(candidate.directory);
        return QStringLiteral("%1（%2）").arg(candidate.name, path);
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!valid(index))
            return Qt::NoItemFlags;
        auto result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (m_enabled && index.column() == 0)
            result |= Qt::ItemIsUserCheckable;
        return result;
    }
    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!m_enabled || !valid(index) || index.column() != 0 || role != Qt::CheckStateRole)
            return false;
        const int checked = value.toInt();
        if (checked != Qt::Checked && checked != Qt::Unchecked)
            return false;
        if (m_checked.contains(index.row()) == (checked == Qt::Checked))
            return true;
        if (checked == Qt::Checked)
            m_checked.insert(index.row());
        else
            m_checked.remove(index.row());
        emit dataChanged(index, index, {Qt::CheckStateRole});
        return true;
    }
    QStringList selectedDirectories() const
    {
        QStringList result;
        // Follow the displayed discovery order, independent of click order.
        for (int row = 0; row < m_candidates.size(); ++row) {
            if (m_checked.contains(row))
                result.push_back(m_candidates.at(row).directory);
        }
        return result;
    }
    int overlapCount() const { return m_overlapCount; }
    int selectedOverlapCount() const
    {
        int count = 0;
        for (const int row : m_checked)
            if (!m_candidates.at(row).overlappingWorks.isEmpty()) ++count;
        return count;
    }

private:
    bool valid(const QModelIndex &index) const
    {
        return index.isValid() && index.model() == this && index.row() >= 0
            && index.row() < m_candidates.size() && index.column() >= 0 && index.column() < 2;
    }
    QVector<WorkDirectoryCandidate> m_candidates;
    QSet<int> m_checked;
    QHash<QString, int> m_nameCounts;
    QString m_root;
    int m_overlapCount = 0;
    bool m_enabled = false;
};

BatchImportPreviewDialog::BatchImportPreviewDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("batchImportPreviewDialog"));
    setWindowTitle(QStringLiteral("总目录批量导入预览"));
    setWindowModality(Qt::NonModal);
    resize(780, 500);
    setMinimumSize(480, 320);
    m_rootLabel = new RootDirectoryLabel(this);
    m_warningLabel = plainLabel(QStringLiteral("batchImportWarningLabel"), this);
    m_summaryLabel = plainLabel(QStringLiteral("batchImportSummaryLabel"), this);
    m_summaryLabel->setWordWrap(false);
    m_summaryLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_feedbackLabel = plainLabel(QStringLiteral("batchImportFeedbackLabel"), this);
    m_feedbackLabel->setProperty("errorLabel", true);
    m_model = new BatchImportCandidatesModel(this);
    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("batchImportCandidatesView"));
    m_table->setAccessibleName(QStringLiteral("待批量导入的作品目录，空格切换勾选"));
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setWordWrap(false);
    m_table->setSortingEnabled(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->setMinimumSize(0, 80);
    m_table->setToolTip(QStringLiteral("每个一级子目录作为一部作品；已收录的同一目录将更新。媒体文件保留原位置。"));
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(34);
    m_table->horizontalHeader()->setMinimumSectionSize(80);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_diagnostics = new QPlainTextEdit(this);
    m_diagnostics->setObjectName(QStringLiteral("batchImportDiagnostics"));
    m_diagnostics->setAccessibleName(QStringLiteral("目录发现详情"));
    m_diagnostics->setReadOnly(true);
    m_diagnostics->setMinimumHeight(60);
    m_diagnostics->setMaximumHeight(100);
    m_diagnostics->setVisible(false);
    m_detailsButton = new QPushButton(QStringLiteral("查看详情"), this);
    m_detailsButton->setObjectName(QStringLiteral("batchImportDetailsButton"));
    m_detailsButton->setCheckable(true);
    m_detailsButton->setToolTip(QStringLiteral("查看跳过的文件、链接和目录发现问题。"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_confirmButton = buttons->button(QDialogButtonBox::Ok);
    m_confirmButton->setObjectName(QStringLiteral("batchImportConfirmButton"));
    m_confirmButton->setText(QStringLiteral("导入所选"));
    m_confirmButton->setToolTip(QStringLiteral("仅导入勾选的作品目录，媒体文件保留原位置。"));
    m_confirmButton->setProperty("primary", true);
    m_confirmButton->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("batchImportCancelButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_summaryLabel);
    bottom->addStretch();
    bottom->addWidget(m_detailsButton);
    bottom->addWidget(buttons);
    auto *layout = new QVBoxLayout;
    layout->setSpacing(12);
    layout->addWidget(m_rootLabel);
    layout->addWidget(m_warningLabel);
    layout->addWidget(m_feedbackLabel);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_diagnostics);
    layout->addLayout(bottom);
    m_dialogFrame = new DialogFrame(this, layout);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &BatchImportPreviewDialog::updateActions);
    connect(m_detailsButton, &QPushButton::toggled, this, [this](bool expanded) {
        m_diagnostics->setVisible(expanded);
        m_detailsButton->setText(expanded ? QStringLiteral("收起详情") : QStringLiteral("查看详情"));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &BatchImportPreviewDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_detailsButton->hide();
    m_feedbackLabel->hide();
    updateActions();
}

void BatchImportPreviewDialog::setDiscovery(const WorkDirectoryDiscoveryResult &discovery)
{
    m_ready = discovery.success && !discovery.cancelled;
    static_cast<RootDirectoryLabel *>(m_rootLabel)->setDirectory(discovery.rootDirectory);
    m_model->setCandidates(discovery.candidates, discovery.rootDirectory, m_ready);
    QStringList diagnostics = discovery.diagnostics;
    if (discovery.suppressedDiagnostics > 0)
        diagnostics.push_back(QStringLiteral("另有 %1 条诊断未展开。").arg(discovery.suppressedDiagnostics));
    const bool hasDetails = !diagnostics.isEmpty() || discovery.rootFilesSkipped > 0 || discovery.linksSkipped > 0;
    if (discovery.rootFilesSkipped > 0 || discovery.linksSkipped > 0)
        diagnostics.prepend(QStringLiteral("跳过总目录直属文件 %1 个；跳过链接目录 %2 个。")
            .arg(discovery.rootFilesSkipped).arg(discovery.linksSkipped));
    m_diagnostics->setPlainText(diagnostics.join(QLatin1Char('\n')));
    m_detailsButton->setChecked(false);
    m_diagnostics->hide();
    m_detailsButton->setVisible(hasDetails);
    m_feedbackLabel->setText(discovery.cancelled ? QStringLiteral("发现已取消，未导入任何作品。")
        : !discovery.success ? discovery.errorMessage.isEmpty()
            ? QStringLiteral("目录发现失败，未导入任何作品。") : discovery.errorMessage
        : discovery.candidates.isEmpty() ? QStringLiteral("没有发现可导入的作品目录。") : QString());
    m_feedbackLabel->setVisible(!m_feedbackLabel->text().isEmpty());
    updateActions();
}

QStringList BatchImportPreviewDialog::selectedDirectories() const
{
    return m_ready ? m_model->selectedDirectories() : QStringList();
}

void BatchImportPreviewDialog::updateActions()
{
    const int selected = static_cast<int>(selectedDirectories().size());
    m_summaryLabel->setText(QStringLiteral("已选 %1 / 共 %2").arg(selected).arg(m_model->rowCount()));
    const int overlaps = m_model->overlapCount();
    const int selectedOverlaps = m_model->selectedOverlapCount();
    const QString overlapSummary = selectedOverlaps > 0
        ? QStringLiteral("已选 %1 项目录重叠。").arg(selectedOverlaps)
        : QStringLiteral("%1 项目录重叠，默认不选。").arg(overlaps);
    m_warningLabel->setText(overlaps > 0 ? overlapSummary
        + QStringLiteral("新增作品不继承原字幕关联、首选或进度；原作品不会拆分或删除。") : QString());
    m_warningLabel->setVisible(overlaps > 0);
    m_confirmButton->setEnabled(m_ready && selected > 0);
}

void BatchImportPreviewDialog::accept()
{
    if (m_ready && !selectedDirectories().isEmpty())
        QDialog::accept();
}

bool BatchImportPreviewDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}

} // namespace asmr
