#pragma once

#include "library/LibraryRepository.h"
#include "playback/LibrarySubtitleResolver.h"
#include "subtitle/GeneratedSubtitleInfo.h"

#include <QDialog>

class QAction;
class QHideEvent;
class QLabel;
class QPushButton;
class QTableView;
class QMenu;

namespace asmr {

class BoundSubtitleTableModel;
class DialogFrame;

enum class BoundSubtitleChoiceKind { Bound, Recommended, Other };

// Plain worker values: filename language markers are never verified languages.
struct BoundSubtitleChoiceItem {
    LibraryFile file;
    SubtitleBindingSource source = SubtitleBindingSource::Auto;
    bool available = false; // Actual path/readability check; used for preference loading.
    QString unavailableReason;
    BoundSubtitleChoiceKind kind = BoundSubtitleChoiceKind::Bound;
    QString matchReason;
    // Bounded manifest data read by the query worker, never by this dialog.
    std::optional<GeneratedSubtitleInfo> generation;
    LibrarySubtitleOrderKey order;
};

// Presents committed values and user intentions only. MainWindow owns async I/O.
class BoundSubtitleDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit BoundSubtitleDialog(QWidget *parent = nullptr);

    void setTrack(qint64 workId, qint64 audioFileId, const QString &title,
                  const LibrarySubtitlePreference &preference,
                  const QVector<BoundSubtitleChoiceItem> &items);
    void setFeedback(const QString &message, bool error = false);
    void setQuerying(bool querying);
    void setImportEnabled(bool enabled);
    [[nodiscard]] qint64 workId() const { return m_workId; }
    [[nodiscard]] qint64 audioFileId() const { return m_audioFileId; }

signals:
    void importRequested();
    void preferenceRequested(SubtitlePreferenceState state, qint64 subtitleFileId);
    void bindingRequested(qint64 subtitleFileId);
    void unbindRequested(qint64 subtitleFileId);
    void translationRequested(qint64 subtitleFileId);
    void openFolderRequested(qint64 subtitleFileId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void updateActions();
    void updatePageSummary();
    void requestPreference(SubtitlePreferenceState state, qint64 subtitleFileId);
    void requestBinding();
    void requestUnbind();
    [[nodiscard]] const BoundSubtitleChoiceItem *selectedItem() const;
    [[nodiscard]] qint64 selectedSubtitleId() const;

    QLabel *m_trackLabel = nullptr;
    QLabel *m_preferenceLabel = nullptr;
    QLabel *m_feedbackLabel = nullptr;
    QLabel *m_pageLabel = nullptr;
    QTableView *m_table = nullptr;
    BoundSubtitleTableModel *m_model = nullptr;
    QPushButton *m_useButton = nullptr;
    QPushButton *m_restoreButton = nullptr;
    QAction *m_bindAction = nullptr;
    QAction *m_unbindAction = nullptr;
    QMenu *m_contextMenu = nullptr;
    QAction *m_openFolderAction = nullptr;
    QPushButton *m_moreButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QAction *m_translateAction = nullptr;
    qint64 m_workId = 0;
    qint64 m_audioFileId = 0;
    bool m_querying = true;
    bool m_importEnabled = false;
    DialogFrame *m_dialogFrame = nullptr;
};

} // namespace asmr
