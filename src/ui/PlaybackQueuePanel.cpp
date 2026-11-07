#include "ui/PlaybackQueuePanel.h"

#include "playback/PlaybackQueue.h"

#include <QColor>
#include <QAction>
#include <QContextMenuEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace asmr {
namespace {
constexpr int EntryIdRole = Qt::UserRole;
}

PlaybackQueuePanel::PlaybackQueuePanel(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("playbackQueuePanel"));
    setAccessibleName(QStringLiteral("播放队列"));
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
        "QFrame#playbackQueuePanel { background: #1F232A; border: 1px solid #45504E; border-radius: 10px; }"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    m_title = new QLabel(QStringLiteral("播放队列 · 0"), this);
    m_title->setObjectName(QStringLiteral("playbackQueueTitle"));
    m_title->setProperty("sectionTitle", true);
    header->addWidget(m_title, 1);
    m_close = new QPushButton(QStringLiteral("关闭"), this);
    m_close->setObjectName(QStringLiteral("closePlaybackQueueButton"));
    m_close->setAccessibleName(QStringLiteral("关闭播放队列"));
    m_close->setToolTip(QStringLiteral("关闭播放队列（Esc）"));
    m_clear = new QPushButton(QStringLiteral("清空"), this);
    m_clear->setObjectName(QStringLiteral("clearOtherQueueButton"));
    m_clear->setToolTip(QStringLiteral("保留当前音轨，清空其他队列项"));
    header->addWidget(m_clear);
    header->addWidget(m_close);
    layout->addLayout(header);
    m_empty = new QLabel(QStringLiteral("队列为空。可播放目录或追加音轨。"), this);
    m_empty->setObjectName(QStringLiteral("playbackQueueEmptyLabel"));
    m_empty->setWordWrap(true);
    m_empty->setTextFormat(Qt::PlainText);
    m_empty->setProperty("secondaryText", true);
    layout->addWidget(m_empty);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("playbackQueueList"));
    m_list->setAccessibleName(QStringLiteral("播放队列音轨"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setWordWrap(false);
    m_list->setSpacing(3);
    m_list->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    layout->addWidget(m_list, 1);
    m_contextMenu = new QMenu(m_list);
    m_contextMenu->setObjectName(QStringLiteral("playbackQueueContextMenu"));
    m_remove = m_contextMenu->addAction(QStringLiteral("移除"));
    m_remove->setObjectName(QStringLiteral("removeQueueEntryAction"));
    m_remove->setToolTip(QStringLiteral("仅移除待播条目，不删除文件或历史"));
    setFocusProxy(m_list);
    connect(m_list, &QListWidget::currentItemChanged, this, [this] {
        dismissContextMenu();
        updateActions();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { requestPlay(item); });
    connect(m_remove, &QAction::triggered, this, [this] {
        const auto id = m_contextId;
        const bool allowed = !m_closing && isVisible() && id && id != m_currentId
            && id == selectedId() && m_entryIds.contains(id);
        dismissContextMenu();
        if (allowed) emit removeRequested(id);
    });
    connect(m_contextMenu, &QMenu::aboutToHide, this, [this] {
        const auto generation = m_contextGeneration;
        // QMenu hides before emitting its action. Expire an abandoned popup on
        // the next turn while allowing the selected action to consume its ID.
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_contextGeneration) dismissContextMenu();
        });
    });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        if (m_clear->isEnabled()) emit clearOthersRequested();
    });
    connect(m_close, &QPushButton::clicked, this, [this] {
        if (!m_closing) emit closeRequested();
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] {
        if (!m_closing) emit closeRequested();
    });
    updateActions();
    hide();
}

void PlaybackQueuePanel::setQueue(const PlaybackQueue &queue)
{
    dismissContextMenu();
    const auto selected = selectedId();
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    m_playable.clear();
    m_entryIds.clear();
    m_currentId = queue.currentId();
    QListWidgetItem *restore = nullptr;
    QListWidgetItem *current = nullptr;
    for (const auto &entry : queue.entries()) {
        const QString title = entry.displayName.isEmpty() ? entry.relativePath : entry.displayName;
        const QString location = entry.workName.isEmpty() ? entry.relativePath
            : entry.workName + QStringLiteral(" / ") + entry.relativePath;
        const bool isCurrent = entry.id == m_currentId;
        QString state = isCurrent ? QStringLiteral("当前音轨") : QString();
        if (!entry.available) state += (state.isEmpty() ? QString() : QStringLiteral(" · "))
            + QStringLiteral("不可用");
        if (!entry.issue.isEmpty()) state += (state.isEmpty() ? QString() : QStringLiteral(" · ")) + entry.issue;
        QString label = (isCurrent ? QStringLiteral("▶  ") : QString()) + title;
        if (!state.isEmpty()) label += QLatin1Char('\n') + state;
        const QString details = label + QLatin1Char('\n') + location;
        auto *item = new QListWidgetItem(label, m_list);
        item->setData(EntryIdRole, QVariant::fromValue(entry.id));
        item->setData(Qt::AccessibleTextRole, details);
        item->setToolTip(QStringLiteral("<qt>%1</qt>")
                             .arg(details.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"))));
        if (isCurrent) {
            QFont font = item->font();
            font.setWeight(QFont::DemiBold);
            item->setFont(font);
            item->setForeground(QColor(QStringLiteral("#7FC5B4")));
            current = item;
        } else if (!entry.available) {
            item->setForeground(QColor(QStringLiteral("#ADB5C1")));
        }
        if (entry.available) m_playable.insert(entry.id);
        m_entryIds.insert(entry.id);
        if (entry.id == selected) restore = item;
    }
    m_title->setText(QStringLiteral("播放队列 · %1").arg(queue.entries().size()));
    m_empty->setVisible(queue.entries().isEmpty());
    m_list->setCurrentItem(restore ? restore : current);
    updateActions();
}

void PlaybackQueuePanel::setClosing(bool closing)
{
    if (closing) dismissContextMenu();
    m_closing = closing;
    updateActions();
}

quint64 PlaybackQueuePanel::selectedId() const
{
    const auto *item = m_list->currentItem();
    return item ? item->data(EntryIdRole).toULongLong() : 0;
}

void PlaybackQueuePanel::updateActions()
{
    const auto selected = selectedId();
    m_list->setEnabled(!m_closing);
    m_close->setEnabled(!m_closing);
    m_remove->setEnabled(!m_closing && m_contextId != 0 && m_contextId == selected
        && m_entryIds.contains(selected) && selected != m_currentId);
    m_clear->setEnabled(!m_closing && m_list->count() > (m_currentId ? 1 : 0));
}

void PlaybackQueuePanel::dismissContextMenu()
{
    ++m_contextGeneration;
    m_contextId = 0;
    m_remove->setEnabled(false);
    m_contextMenu->close();
}

void PlaybackQueuePanel::requestPlay(QListWidgetItem *item)
{
    if (!item || m_closing) return;
    const auto id = item->data(EntryIdRole).toULongLong();
    if (m_playable.contains(id)) emit playRequested(id);
}

bool PlaybackQueuePanel::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_list || watched == m_list->viewport())
        && event->type() == QEvent::ContextMenu) {
        auto *context = static_cast<QContextMenuEvent *>(event);
        dismissContextMenu();
        if (m_closing || !isVisible()) return true;
        QListWidgetItem *item = context->reason() == QContextMenuEvent::Keyboard
            ? m_list->currentItem() : m_list->itemAt(m_list->viewport()->mapFromGlobal(context->globalPos()));
        if (!item) return true;
        m_list->setCurrentItem(item);
        m_contextId = selectedId();
        updateActions();
        const auto position = context->reason() == QContextMenuEvent::Keyboard
            ? m_list->viewport()->mapToGlobal(m_list->visualItemRect(item).center()) : context->globalPos();
        m_contextMenu->popup(position);
        return true;
    }
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            requestPlay(m_list->currentItem());
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void PlaybackQueuePanel::hideEvent(QHideEvent *event)
{
    dismissContextMenu();
    QFrame::hideEvent(event);
}

} // namespace asmr
