#pragma once

#include "artwork/CoverDirectoryBrowser.h"

#include <QDialog>
#include <QFuture>

#include <optional>

class QHBoxLayout;
class QCloseEvent;
class QLabel;
class QModelIndex;
class QPushButton;
class QScrollArea;
class QStandardItemModel;
class QTableView;
class QToolButton;

namespace asmr {
class CoverArtService;
class CoverArtWidget;
class DialogFrame;

class CoverSelectionDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CoverSelectionDialog(const QString &workName, const QString &rootPath,
                                  QWidget *parent = nullptr);
    ~CoverSelectionDialog() override;
    void setManualSelection(const QString &relativePath, bool unavailable = false);
    void setSaving(bool saving);
    void setFeedback(const QString &message);
    void shutdown();
    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] QString currentDirectory() const { return m_currentDirectory; }
    [[nodiscard]] QString selectedRelativePath() const { return m_selectedPath; }

signals:
    void coverChosen(const QString &relativePath);
    void shutdownFinished();

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void done(int result) override;

private:
    enum EntryRole { RelativePathRole = Qt::UserRole + 1, DirectoryRole };
    struct Pending { quint64 id; QString directory; };
    void navigate(const QString &relativeDirectory);
    void startListing(Pending pending);
    void applyPage(const CoverDirectoryPage &page);
    void rebuildBreadcrumbs();
    void selectEntry(const QModelIndex &index);
    void activateEntry(const QModelIndex &index);
    void navigateUp();
    void updateControls();
    void finishShutdownIfReady();

    QString m_rootPath;
    QString m_currentDirectory;
    QString m_selectedPath;
    QString m_manualPath;
    quint64 m_browseGeneration = 0;
    quint64 m_previewRequest = 0;
    bool m_active = false;
    bool m_loading = false;
    bool m_previewReady = false;
    bool m_saving = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    bool m_previewShutdownFinished = false;
    std::optional<int> m_requestedResult;
    std::optional<Pending> m_pending;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFuture<CoverDirectoryPage> m_lifetime;
    CoverArtService *m_previewService = nullptr;
    QStandardItemModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    CoverArtWidget *m_preview = nullptr;
    QWidget *m_breadcrumbBar = nullptr;
    QHBoxLayout *m_breadcrumbLayout = nullptr;
    QScrollArea *m_breadcrumbScroll = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_reloadButton = nullptr;
    QPushButton *m_chooseButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QLabel *m_selectionLabel = nullptr;
    QLabel *m_manualLabel = nullptr;
    QLabel *m_feedbackLabel = nullptr;
    DialogFrame *m_dialogFrame = nullptr;
};
} // namespace asmr
