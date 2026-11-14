#pragma once

#include "subtitle/SubtitleEntry.h"

#include <QImage>
#include <QList>
#include <QPoint>
#include <QString>
#include <QWidget>

class QTextEdit;
class QGridLayout;
class QPushButton;
class QPropertyAnimation;
class QTimer;

namespace asmr {

class CoverArtWidget;

// Presents subtitle data and user intentions; loading and synchronization stay outside.
class SubtitlePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SubtitlePanel(QWidget *parent = nullptr);

    void setImportEnabled(bool enabled);
    void setSelectionEnabled(bool enabled);
    void setLoading(bool loading);
    void setFileInfo(const QString &summary, const QString &diagnostics);
    void clearFileInfo();
    void showLoadFailure();
    void setLibraryStatus(const QString &message);
    void setSubtitleText(const QString &text);
    void setSubtitleEntries(const QList<SubtitleEntry> &entries);
    void setPlaybackPosition(qint64 positionMs, bool playbackStopped = false,
                             bool playing = true);
    void setSeekEnabled(bool enabled);
    void cancelPendingSeek();
    // Call immediately before an accepted media seek. Coalesce its snapshots
    // and layout changes before starting one short centering animation.
    void prepareForSubtitleSeek();
    void resumeFollowing();
    void setCoverImage(const QImage &image, const QString &sourcePath = {},
                       const QString &detail = {});
    void setCoverPlaceholder(const QString &message = QStringLiteral("暂无封面"));
    void setDetailMode(bool enabled);
    // Settle pending reading position before an endpoint snapshot is captured.
    void prepareForSnapshot();

    [[nodiscard]] bool detailMode() const { return m_detailMode; }

    [[nodiscard]] QString subtitleText() const;
    [[nodiscard]] bool canOpenSubtitle() const;
    [[nodiscard]] bool canSelectSubtitle() const;
    [[nodiscard]] bool hasSubtitleInformation() const;
    [[nodiscard]] QString libraryStatus() const;
    [[nodiscard]] QString fileSummary() const;
    [[nodiscard]] QString diagnosticDetails() const;

signals:
    void subtitleActionsChanged(bool openEnabled, bool selectionEnabled,
                                bool informationAvailable);
    void seekRequested(qint64 positionMs);
    void collapseRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct CueRange {
        SubtitleEntry entry;
        int documentStart = 0;
        int documentEnd = 0;
    };

    void updatePresentation();
    void updateCoverSize();
    void updateDetailLayout();
    void pauseFollowing();
    void stopFollowingAnimation();
    void followCurrentCue(bool animate, bool clicked = false);
    void scheduleFollowing();
    void applyFollowingLayout(bool animateClick = true);
    void cancelPendingClick();
    [[nodiscard]] int cueAt(const QPoint &position) const;

    QTextEdit *m_textDisplay = nullptr;
    QWidget *m_coverContainer = nullptr;
    CoverArtWidget *m_cover = nullptr;
    QPushButton *m_resumeFollowingButton = nullptr;
    QPushButton *m_collapseButton = nullptr;
    QGridLayout *m_contentLayout = nullptr;
    QPropertyAnimation *m_followAnimation = nullptr;
    QTimer *m_clickTimer = nullptr;
    QList<CueRange> m_cueRanges;
    QList<int> m_activeCueIndices;
    QString m_activeText;
    QString m_fileSummary;
    QString m_libraryStatus;
    QString m_diagnosticDetails;
    quint64 m_entriesGeneration = 0;
    quint64 m_clickGeneration = 0;
    int m_pressedCue = -1;
    int m_pendingCue = -1;
    int m_manualViewPosition = -1;
    QPoint m_pressPosition;
    bool m_following = true;
    bool m_followQueued = false;
    quint64 m_followRequestGeneration = 0;
    bool m_seekFollowPending = false;
    bool m_seekFollowActive = false;
    bool m_mousePressed = false;
    bool m_mouseDragged = false;
    bool m_seekEnabled = false;
    bool m_playing = false;
    bool m_detailMode = false;
    bool m_detailColumns = false;
    // Presentation flags only; MainWindow owns the task and subtitle binding.
    bool m_importEnabled = false;
    bool m_selectionEnabled = false;
    bool m_loading = false;
    bool m_loadFailed = false;
    bool m_hasFileInfo = false;
    bool m_lastOpenEnabled = false;
    bool m_lastSelectionEnabled = false;
    bool m_lastInformationAvailable = false;
};

} // namespace asmr
