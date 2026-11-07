#pragma once

#include <QFrame>
#include <QSet>

class QLabel;
class QAction;
class QHideEvent;
class QMenu;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace asmr {

class PlaybackQueue;

// Main-window-owned overlay. Projects state and emits intentions only.
class PlaybackQueuePanel final : public QFrame {
    Q_OBJECT
public:
    explicit PlaybackQueuePanel(QWidget *parent = nullptr);
    void setQueue(const PlaybackQueue &queue);
    void setClosing(bool closing);

signals:
    void playRequested(quint64 id);
    void removeRequested(quint64 id);
    void clearOthersRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void updateActions();
    void dismissContextMenu();
    void requestPlay(QListWidgetItem *item);
    [[nodiscard]] quint64 selectedId() const;

    QLabel *m_title = nullptr;
    QLabel *m_empty = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_close = nullptr;
    QMenu *m_contextMenu = nullptr;
    QAction *m_remove = nullptr;
    QPushButton *m_clear = nullptr;
    QSet<quint64> m_playable;
    QSet<quint64> m_entryIds;
    quint64 m_currentId = 0;
    quint64 m_contextId = 0;
    quint64 m_contextGeneration = 0;
    bool m_closing = false;
};

} // namespace asmr
