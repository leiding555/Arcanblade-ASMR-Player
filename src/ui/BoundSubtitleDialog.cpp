#include "ui/BoundSubtitleDialog.h"
#include "ui/DialogFrame.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QHash>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPushButton>
#include <QSet>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace asmr {
namespace {
constexpr int PageSize = 200;
constexpr int FileIdRole = Qt::UserRole;
constexpr int AvailableRole = Qt::UserRole + 1;
constexpr int ChoiceKindRole = Qt::UserRole + 2;
constexpr int GenerationTextRole = Qt::UserRole + 3;
constexpr int PrimaryNameRole = Qt::UserRole + 4;

QString safeTooltip(const QString &text)
{
    return QStringLiteral("<qt>%1</qt>").arg(
        text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
}
QString fileName(const LibraryFile &file)
{
    return file.fileName.isEmpty() ? file.relativePath : file.fileName;
}
QString generationText(const BoundSubtitleChoiceItem &item)
{
    if (!item.generation || !item.generation->completedAt.isValid())
        return {};
    const QString mode = item.generation->mode == QStringLiteral("offline-direct")
        ? QStringLiteral("离线直译")
        : item.generation->mode == QStringLiteral("offline-asr-ja")
            ? QStringLiteral("日语识别")
            : item.generation->mode == QStringLiteral("cloud-correction-ja") ? QStringLiteral("日语校正")
            : item.generation->mode == QStringLiteral("cloud-translation")
                ? QStringLiteral("字幕翻译 · %1").arg(item.generation->translationService == QStringLiteral("deepseek")
                    ? QStringLiteral("DeepSeek") : QStringLiteral("自定义服务")) : QString();
    if (mode.isEmpty())
        return {};
    return mode + QStringLiteral(" · ")
        + item.generation->completedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}
QString categoryName(BoundSubtitleChoiceKind kind)
{
    switch (kind) {
    case BoundSubtitleChoiceKind::Bound: return QStringLiteral("已关联");
    case BoundSubtitleChoiceKind::Recommended: return QStringLiteral("推荐");
    case BoundSubtitleChoiceKind::Other: return QStringLiteral("其他");
    }
    return {};
}
}

// Paint each line separately so a long filename cannot elide the version line.
// Keep the style's normal selection, hover, focus and disabled rendering.
class SubtitleNameDelegate final : public QStyledItemDelegate
{
public:
    explicit SubtitleNameDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QString version = index.data(GenerationTextRole).toString();
        if (version.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const auto *widget = styled.widget;
        auto *style = widget ? widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, widget)
                                   .adjusted(2, 2, -2, -2);
        styled.text.clear();
        painter->save();
        painter->setClipRect(option.rect);
        style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, widget);
        painter->setFont(option.font);
        const QFontMetrics metrics(option.font);
        const int lineHeight = metrics.height();
        const int top = textRect.top() + std::max(0, (textRect.height() - lineHeight * 2 - 2) / 2);
        const QRect first(textRect.left(), top, textRect.width(), lineHeight);
        const QRect second(textRect.left(), top + lineHeight + 2, textRect.width(), lineHeight);
        const auto group = option.state.testFlag(QStyle::State_Enabled)
            ? QPalette::Active : QPalette::Disabled;
        const auto role = option.state.testFlag(QStyle::State_Selected)
            ? QPalette::HighlightedText : QPalette::Text;
        QColor color = styled.palette.color(group, role);
        painter->setPen(color);
        painter->drawText(first, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(index.data(PrimaryNameRole).toString(), Qt::ElideRight, first.width()));
        color.setAlphaF(color.alphaF() * 0.75);
        painter->setPen(color);
        painter->drawText(second, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(version, Qt::ElideRight, second.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (!index.data(GenerationTextRole).toString().isEmpty())
            size.setHeight(std::max(size.height(), QFontMetrics(option.font).height() * 2 + 20));
        return size;
    }
};

// Stores values per file, never a QWidget per row. Fetches another bounded page
// when requested by the view or the explicit button.
class BoundSubtitleTableModel final : public QAbstractTableModel
{
public:
    explicit BoundSubtitleTableModel(QObject *parent) : QAbstractTableModel(parent) {}

    void setItems(qint64 workId, const QVector<BoundSubtitleChoiceItem> &items)
    {
        beginResetModel();
        m_items.clear();
        m_fileNameCounts.clear();
        m_generationCounts.clear();
        auto ordered = items;
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
            if (left.kind != right.kind)
                return static_cast<int>(left.kind) < static_cast<int>(right.kind);
            return left.kind == BoundSubtitleChoiceKind::Bound && librarySubtitleNewer(left.order, right.order);
        });
        QSet<qint64> ids;
        for (const auto &item : ordered) {
            if (workId <= 0 || item.file.id <= 0 || item.file.workId != workId
                || item.file.type != LibraryFileType::Subtitle || ids.contains(item.file.id))
                continue;
            ids.insert(item.file.id);
            m_items.push_back(item);
            ++m_fileNameCounts[fileName(item.file).toCaseFolded()];
            const auto generated = generationText(item);
            if (!generated.isEmpty())
                ++m_generationCounts[(fileName(item.file) + generated).toCaseFolded()];
        }
        m_visible = std::min(PageSize, static_cast<int>(m_items.size()));
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : m_visible;
    }
    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 2;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        const QStringList headers{QStringLiteral("字幕文件"), QStringLiteral("状态")};
        return section >= 0 && section < headers.size() ? headers[section] : QVariant();
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        const auto *entry = item(index.row());
        if (!index.isValid() || !entry || index.column() < 0 || index.column() >= 2)
            return {};
        if (role == FileIdRole)
            return entry->file.id;
        if (role == AvailableRole)
            return entry->available && !entry->file.missing;
        if (role == ChoiceKindRole)
            return static_cast<int>(entry->kind);
        if (role == GenerationTextRole)
            return index.column() == 0 ? versionText(*entry) : QString();
        if (role == PrimaryNameRole)
            return primaryName(*entry);
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole
            && role != Qt::AccessibleTextRole && role != Qt::AccessibleDescriptionRole)
            return {};
        const auto &file = entry->file;
        QString status = categoryName(entry->kind);
        if (file.missing)
            status += QStringLiteral(" · 文件缺失");
        else if (!entry->available)
            status += QStringLiteral(" · 不可用");
        if (role == Qt::AccessibleTextRole)
            return index.column() == 0 ? displayName(*entry) : status;
        if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole) {
            const QString source = entry->kind != BoundSubtitleChoiceKind::Bound
                ? QStringLiteral("尚未关联")
                : entry->source == SubtitleBindingSource::Manual ? QStringLiteral("人工关联")
                                                                 : QStringLiteral("自动关联");
            const QString reason = entry->matchReason.isEmpty()
                ? entry->kind == BoundSubtitleChoiceKind::Bound ? QStringLiteral("已保存的字幕关联")
                    : QStringLiteral("无规则推荐，可人工确认")
                : entry->matchReason;
            QString details = QStringLiteral("%1\n格式：%2\n状态：%3\n来源：%4\n匹配依据：%5\n"
                "文件名中的语言标记仅供参考，未验证实际语言。")
                .arg(file.relativePath, file.extension.toUpper(), status, source, reason);
            if (!versionText(*entry).isEmpty())
                details += QStringLiteral("\n生成方式与时间：%1").arg(versionText(*entry));
            if (entry->generation && !entry->generation->translationModel.isEmpty())
                details += QStringLiteral("\n翻译模型：%1").arg(entry->generation->translationModel);
            if (!file.missing && !entry->available)
                details += QStringLiteral("\n%1").arg(entry->unavailableReason.isEmpty()
                    ? QStringLiteral("文件不可用") : entry->unavailableReason);
            return role == Qt::ToolTipRole ? safeTooltip(details) : details;
        }
        if (index.column() == 1)
            return status;
        const QString version = versionText(*entry);
        return primaryName(*entry) + (version.isEmpty() ? QString() : QLatin1Char('\n') + version);
    }
    QString versionText(const BoundSubtitleChoiceItem &entry) const
    {
        QString text = generationText(entry);
        if (!text.isEmpty()
            && m_generationCounts.value((fileName(entry.file) + text).toCaseFolded()) > 1) {
            text += QStringLiteral("（%1）").arg(directoryName(entry.file));
        }
        return text;
    }
    QString primaryName(const BoundSubtitleChoiceItem &entry) const
    {
        QString name = fileName(entry.file);
        if (generationText(entry).isEmpty() && m_fileNameCounts.value(name.toCaseFolded()) > 1)
            name += QStringLiteral("（%1）").arg(directoryName(entry.file));
        return name;
    }
    QString displayName(const BoundSubtitleChoiceItem &entry) const
    {
        const QString version = versionText(entry);
        return primaryName(entry) + (version.isEmpty() ? QString() : QStringLiteral(" · ") + version);
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        // Missing records remain selectable for unbinding and inspecting reasons.
        return index.isValid() && item(index.row())
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }
    bool canFetchMore(const QModelIndex &parent = {}) const override
    {
        return !parent.isValid() && m_visible < m_items.size();
    }
    void fetchMore(const QModelIndex &parent = {}) override
    {
        if (!canFetchMore(parent))
            return;
        const int next = std::min(m_visible + PageSize, static_cast<int>(m_items.size()));
        beginInsertRows({}, m_visible, next - 1);
        m_visible = next;
        endInsertRows();
    }
    const BoundSubtitleChoiceItem *item(int row) const
    {
        return row >= 0 && row < m_visible ? &m_items[row] : nullptr;
    }
    int totalCount() const { return static_cast<int>(m_items.size()); }
    const QVector<BoundSubtitleChoiceItem> &items() const { return m_items; }
    void reveal(int row)
    {
        while (row >= m_visible && canFetchMore())
            fetchMore();
    }

private:
    static QString directoryName(const LibraryFile &file)
    {
        const QString path = QDir::fromNativeSeparators(file.relativePath);
        return path.contains(QLatin1Char('/'))
            ? path.section(QLatin1Char('/'), 0, -2) : QStringLiteral(".");
    }
    QVector<BoundSubtitleChoiceItem> m_items;
    QHash<QString, int> m_fileNameCounts;
    QHash<QString, int> m_generationCounts;
    int m_visible = 0;
};

BoundSubtitleDialog::BoundSubtitleDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("boundSubtitleDialog"));
    setWindowTitle(QStringLiteral("字幕管理"));
    setModal(false);
    resize(760, 490);

    m_trackLabel = new QLabel(this);
    m_trackLabel->setObjectName(QStringLiteral("boundSubtitleTrackLabel"));
    m_trackLabel->setTextFormat(Qt::PlainText);
    m_trackLabel->setWordWrap(true);
    m_trackLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preferenceLabel = new QLabel(this);
    m_preferenceLabel->setObjectName(QStringLiteral("boundSubtitlePreferenceLabel"));
    m_preferenceLabel->setTextFormat(Qt::PlainText);
    m_preferenceLabel->setWordWrap(true);
    m_restoreButton = new QPushButton(QStringLiteral("恢复默认"), this);
    m_restoreButton->setObjectName(QStringLiteral("automaticBoundSubtitleButton"));
    m_restoreButton->setToolTip(QStringLiteral("清除手动默认或禁用设置，自动使用最新可用的已关联字幕；不改变关联。"));
    m_contextMenu = new QMenu(this);
    m_contextMenu->setObjectName(QStringLiteral("boundSubtitleContextMenu"));
    m_contextMenu->setToolTipsVisible(true);
    m_translateAction = m_contextMenu->addAction(QStringLiteral("翻译所选字幕…"));
    m_translateAction->setObjectName(QStringLiteral("translateBoundSubtitleButton"));
    m_translateAction->setToolTip(QStringLiteral("加入字幕翻译列表，不改变字幕关联和默认选择。"));
    m_contextMenu->addSeparator();
    m_bindAction = m_contextMenu->addAction(QStringLiteral("关联所选字幕"));
    m_bindAction->setObjectName(QStringLiteral("bindSubtitleButton"));
    m_bindAction->setToolTip(QStringLiteral("确认为人工关联，不改变默认字幕。"));
    m_unbindAction = m_contextMenu->addAction(QStringLiteral("解除所选关联"));
    m_unbindAction->setObjectName(QStringLiteral("removeSubtitleBindingButton"));
    m_unbindAction->setToolTip(QStringLiteral("立即解除关联；解除默认字幕时恢复自动选择。重新扫描可能再次自动关联。"));
    m_contextMenu->addSeparator();
    m_openFolderAction = m_contextMenu->addAction(QStringLiteral("打开所在文件夹"));
    m_openFolderAction->setObjectName(QStringLiteral("openSubtitleFolderAction"));

    m_model = new BoundSubtitleTableModel(this);
    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("boundSubtitleTable"));
    m_table->setAccessibleName(QStringLiteral("当前播放音轨的字幕关联与候选"));
    m_table->setModel(m_model);
    m_table->setItemDelegateForColumn(0, new SubtitleNameDelegate(m_table));
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    // Status values are short; reserve the remaining width for version details.
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setWordWrap(false);
    m_table->setSortingEnabled(false);
    m_table->setMinimumHeight(130);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);

    m_useButton = new QPushButton(QStringLiteral("使用所选字幕"), this);
    m_useButton->setObjectName(QStringLiteral("useBoundSubtitleButton"));
    m_useButton->setAttribute(Qt::WA_AlwaysShowToolTips);
    m_moreButton = new QPushButton(QStringLiteral("加载更多"), this);
    m_moreButton->setObjectName(QStringLiteral("moreSubtitleChoicesButton"));
    m_importButton = new QPushButton(QStringLiteral("导入字幕"), this);
    m_importButton->setObjectName(QStringLiteral("importSubtitleButton"));
    m_importButton->setToolTip(QStringLiteral("为当前音轨临时加载字幕文件；不复制文件或改变字幕绑定和首选。"));
    m_pageLabel = new QLabel(this);
    m_pageLabel->setObjectName(QStringLiteral("boundSubtitlePageLabel"));
    m_pageLabel->setTextFormat(Qt::PlainText);
    m_feedbackLabel = new QLabel(this);
    m_feedbackLabel->setObjectName(QStringLiteral("boundSubtitleFeedbackLabel"));
    m_feedbackLabel->setTextFormat(Qt::PlainText);
    m_feedbackLabel->setWordWrap(true);
    // Long filenames and paths must not become a top-level minimum width.
    // Text still wraps within the available width; full values remain in the
    // tooltips and accessibility text rather than widening the whole dialog.
    for (auto *label : {m_trackLabel, m_preferenceLabel, m_feedbackLabel}) {
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    auto *closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeButtons->setObjectName(QStringLiteral("boundSubtitleCloseButtons"));
    closeButtons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto *page = new QHBoxLayout;
    page->addWidget(m_pageLabel);
    page->addStretch();
    page->addWidget(m_moreButton);
    auto *preferenceRow = new QHBoxLayout;
    preferenceRow->addWidget(m_preferenceLabel, 1);
    preferenceRow->addWidget(m_restoreButton);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_useButton);
    actions->addWidget(m_importButton);
    actions->addStretch();
    actions->addWidget(closeButtons);
    auto *layout = new QVBoxLayout;
    layout->addWidget(m_trackLabel);
    layout->addLayout(preferenceRow);
    layout->addWidget(m_table, 1);
    layout->addLayout(page);
    layout->addWidget(m_feedbackLabel);
    layout->addLayout(actions);
    m_dialogFrame = new DialogFrame(this, layout);
    setTabOrder(m_restoreButton, m_table);
    setTabOrder(m_table, m_useButton);
    setTabOrder(m_useButton, m_importButton);
    setTabOrder(m_importButton, m_moreButton);

    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { m_contextMenu->close(); updateActions(); });
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex &) {
        requestPreference(SubtitlePreferenceState::Subtitle, selectedSubtitleId());
    });
    connect(m_useButton, &QPushButton::clicked, this, [this] {
        requestPreference(SubtitlePreferenceState::Subtitle, selectedSubtitleId());
    });
    connect(m_restoreButton, &QPushButton::clicked, this, [this] {
        requestPreference(SubtitlePreferenceState::Unset, 0);
    });
    connect(m_bindAction, &QAction::triggered, this, &BoundSubtitleDialog::requestBinding);
    connect(m_unbindAction, &QAction::triggered, this, &BoundSubtitleDialog::requestUnbind);
    connect(m_moreButton, &QPushButton::clicked, this, [this] { m_model->fetchMore(); });
    connect(m_importButton, &QPushButton::clicked, this, [this] {
        if (m_importEnabled)
            emit importRequested();
    });
    connect(m_translateAction, &QAction::triggered, this, [this] {
        const auto *entry = selectedItem();
        if (!m_querying && m_workId > 0 && m_audioFileId > 0 && entry && entry->available && !entry->file.missing)
            emit translationRequested(entry->file.id);
    });
    connect(m_openFolderAction, &QAction::triggered, this, [this] {
        const auto *entry = selectedItem();
        if (!m_querying && m_workId > 0 && m_audioFileId > 0 && entry && entry->available && !entry->file.missing)
            emit openFolderRequested(entry->file.id);
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this,
            [this] { m_table->resizeRowsToContents(); updatePageSummary(); });
    connect(closeButtons, &QDialogButtonBox::rejected, this, [this] {
        m_contextMenu->close(); hide();
    });
    setTrack(0, 0, {}, {}, {});
    setQuerying(true);
}

bool BoundSubtitleDialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_table || watched == m_table->viewport()) && event->type() == QEvent::ContextMenu) {
        auto *context = static_cast<QContextMenuEvent *>(event);
        if (m_querying || m_workId <= 0 || m_audioFileId <= 0)
            return true;
        QPoint popupPosition;
        if (context->reason() == QContextMenuEvent::Keyboard) {
            const auto selected = m_table->selectionModel()->selectedRows(0);
            if (selected.size() != 1)
                return true;
            popupPosition = m_table->viewport()->mapToGlobal(m_table->visualRect(selected.front()).center());
        } else {
            const auto position = m_table->viewport()->mapFromGlobal(context->globalPos());
            const auto index = m_table->indexAt(position);
            if (!index.isValid()) {
                m_contextMenu->close();
                return true;
            }
            m_table->setCurrentIndex(index);
            m_table->selectRow(index.row());
            popupPosition = context->globalPos();
        }
        updateActions();
        m_contextMenu->popup(popupPosition);
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void BoundSubtitleDialog::hideEvent(QHideEvent *event)
{
    m_contextMenu->close();
    QDialog::hideEvent(event);
}

bool BoundSubtitleDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}

void BoundSubtitleDialog::setTrack(qint64 workId, qint64 audioFileId, const QString &title,
                                  const LibrarySubtitlePreference &preference,
                                  const QVector<BoundSubtitleChoiceItem> &items)
{
    m_contextMenu->close();
    m_workId = workId;
    m_audioFileId = audioFileId;
    const bool validTrack = workId > 0 && audioFileId > 0
        && (preference.audioFileId == 0 || preference.audioFileId == audioFileId);
    if (!validTrack) {
        m_workId = 0;
        m_audioFileId = 0;
    }
    m_trackLabel->setText(validTrack || (workId == 0 && audioFileId == 0 && !title.isEmpty())
        ? QStringLiteral("音轨：%1").arg(title.isEmpty() ? QStringLiteral("未命名音轨") : title)
        : QStringLiteral("当前没有音轨"));
    m_trackLabel->setToolTip(safeTooltip(m_trackLabel->text()));
    m_trackLabel->setAccessibleDescription(m_trackLabel->text());
    m_model->setItems(validTrack ? workId : 0, items);
    m_table->resizeRowsToContents();
    QString preferredName;
    int preferredRow = -1;
    for (int row = 0; row < m_model->totalCount(); ++row) {
        const auto &entry = m_model->items()[row];
        if (entry.kind == BoundSubtitleChoiceKind::Bound
            && entry.file.id == preference.subtitleFileId
            && preference.state == SubtitlePreferenceState::Subtitle) {
            preferredName = m_model->displayName(entry);
            if (entry.available && !entry.file.missing)
                preferredRow = row;
        }
    }
    m_table->clearSelection();
    m_table->setCurrentIndex({});
    if (preferredRow >= 0) {
        m_model->reveal(preferredRow);
        m_table->selectRow(preferredRow);
    }
    QString preferenceSummary = QStringLiteral("默认字幕：自动选择");
    if (!validTrack)
        preferenceSummary = QStringLiteral("默认字幕：仅作品库音轨可设置");
    else if (preference.state == SubtitlePreferenceState::None)
        preferenceSummary = QStringLiteral("默认字幕：不使用字幕");
    else if (preference.state == SubtitlePreferenceState::Subtitle)
        preferenceSummary = QStringLiteral("默认字幕：%1")
            .arg(preferredName.isEmpty() ? QStringLiteral("不可用字幕") : preferredName);
    m_preferenceLabel->setText(preferenceSummary);
    m_preferenceLabel->setToolTip(safeTooltip(preferenceSummary));
    m_preferenceLabel->setAccessibleDescription(preferenceSummary);
    m_querying = false;
    setFeedback(validTrack && m_model->totalCount() == 0
        ? QStringLiteral("暂无字幕，可导入字幕文件。") : QString());
    updatePageSummary();
    updateActions();
}

void BoundSubtitleDialog::setFeedback(const QString &message, bool error)
{
    m_feedbackLabel->setText(error && !message.isEmpty()
        ? QStringLiteral("操作失败：%1").arg(message) : message);
    m_feedbackLabel->setProperty("error", error);
    m_feedbackLabel->setAccessibleDescription(m_feedbackLabel->text());
    m_feedbackLabel->setVisible(!message.isEmpty());
}

void BoundSubtitleDialog::setQuerying(bool querying)
{
    m_querying = querying;
    m_contextMenu->close();
    if (querying)
        setFeedback(QStringLiteral("正在读取字幕..."));
    updateActions();
}

void BoundSubtitleDialog::setImportEnabled(bool enabled)
{
    m_importEnabled = enabled;
    updateActions();
}

const BoundSubtitleChoiceItem *BoundSubtitleDialog::selectedItem() const
{
    const auto rows = m_table->selectionModel()->selectedRows(0);
    return rows.size() == 1 ? m_model->item(rows.front().row()) : nullptr;
}

qint64 BoundSubtitleDialog::selectedSubtitleId() const
{
    const auto *entry = selectedItem();
    return entry && entry->available && !entry->file.missing ? entry->file.id : 0;
}

void BoundSubtitleDialog::updatePageSummary()
{
    const bool paged = m_model->totalCount() > PageSize;
    m_pageLabel->setText(QStringLiteral("已加载 %1 / %2")
        .arg(m_model->rowCount()).arg(m_model->totalCount()));
    m_pageLabel->setVisible(paged);
    m_moreButton->setVisible(m_model->canFetchMore());
    m_moreButton->setEnabled(!m_querying && m_model->canFetchMore());
}

void BoundSubtitleDialog::updateActions()
{
    const bool enabled = !m_querying && m_workId > 0 && m_audioFileId > 0;
    const auto *entry = selectedItem();
    const bool bound = entry && entry->kind == BoundSubtitleChoiceKind::Bound;
    m_table->setEnabled(enabled);
    m_useButton->setEnabled(enabled && selectedSubtitleId() > 0);
    m_useButton->setToolTip(entry && !bound
        ? QStringLiteral("关联所选字幕并设为默认字幕。")
        : QStringLiteral("将所选字幕保存为默认字幕。"));
    m_useButton->setAccessibleDescription(m_useButton->toolTip());
    m_restoreButton->setEnabled(enabled);
    m_bindAction->setEnabled(enabled && entry && !entry->file.missing
        && (!bound || entry->source == SubtitleBindingSource::Auto));
    m_bindAction->setText(bound ? QStringLiteral("改为人工关联") : QStringLiteral("关联所选字幕"));
    m_unbindAction->setEnabled(enabled && bound);
    m_moreButton->setEnabled(enabled && m_model->canFetchMore());
    // Import belongs to the accepted audio session, independently of the
    // asynchronous library snapshot and its preference/binding controls.
    m_importButton->setEnabled(m_importEnabled);
    m_translateAction->setEnabled(enabled && selectedSubtitleId() > 0);
    m_openFolderAction->setEnabled(enabled && selectedSubtitleId() > 0);
}

void BoundSubtitleDialog::requestPreference(SubtitlePreferenceState state, qint64 subtitleFileId)
{
    if (m_querying || m_workId <= 0 || m_audioFileId <= 0)
        return;
    if (state == SubtitlePreferenceState::Subtitle
        && (subtitleFileId <= 0 || subtitleFileId != selectedSubtitleId()))
        return;
    // No optimistic preference change: this label changes only on committed setTrack.
    setFeedback(QStringLiteral("正在保存默认字幕..."));
    emit preferenceRequested(state, subtitleFileId);
}

void BoundSubtitleDialog::requestBinding()
{
    const auto *entry = selectedItem();
    if (m_querying || m_workId <= 0 || m_audioFileId <= 0 || !entry || entry->file.missing
        || (entry->kind == BoundSubtitleChoiceKind::Bound
            && entry->source != SubtitleBindingSource::Auto))
        return;
    const auto id = entry->file.id;
    setFeedback(QStringLiteral("正在保存人工关联..."));
    emit bindingRequested(id);
}

void BoundSubtitleDialog::requestUnbind()
{
    const auto *entry = selectedItem();
    if (m_querying || m_workId <= 0 || m_audioFileId <= 0 || !entry
        || entry->kind != BoundSubtitleChoiceKind::Bound)
        return;
    const auto id = entry->file.id;
    setFeedback(QStringLiteral("正在解除关联..."));
    emit unbindRequested(id);
}

} // namespace asmr
