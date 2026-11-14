#pragma once

#include <QDialog>
#include <QStringList>

class QAction;
class QCheckBox;
class QHideEvent;
class QLabel;
class QMenu;
class QPushButton;
class QTableView;

namespace asmr {

class ScanRootsModel;
class DialogFrame;

// Presentation only. The owner persists configuration and schedules work;
// statuses describe this run only and are never interpreted as configuration.
class ScanRootManagerDialog final : public QDialog
{
    Q_OBJECT

public:
    enum DataRole { RootPathRole = Qt::UserRole + 1 };
    explicit ScanRootManagerDialog(QWidget *parent = nullptr);
    void setConfiguration(const QStringList &roots, bool startupEnabled);
    void setBusy(bool busy);
    void setRootStatus(const QString &root, const QString &text);
    void setRootStatus(const QString &root, const QString &details,
                       const QString &summary, bool reviewRequired = false);

signals:
    void addRootRequested();
    void removeRootRequested(const QString &root);
    void refreshRootRequested(const QString &root);
    void refreshAllRequested();
    void startupScanChanged(bool enabled);
    void reviewOverlapsRequested(const QString &root);
    void openRootRequested(const QString &root);
    void hidden();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    [[nodiscard]] QString selectedRoot() const;
    void updateActions();

    ScanRootsModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QCheckBox *m_startup = nullptr;
    QPushButton *m_add = nullptr;
    QAction *m_remove = nullptr;
    QAction *m_refresh = nullptr;
    QPushButton *m_refreshAll = nullptr;
    QAction *m_reviewOverlaps = nullptr;
    QMenu *m_contextMenu = nullptr;
    QAction *m_open = nullptr;
    QPushButton *m_reviewPending = nullptr;
    QLabel *m_busyLabel = nullptr;
    bool m_busy = false;
    DialogFrame *m_dialogFrame = nullptr;
};

} // namespace asmr
