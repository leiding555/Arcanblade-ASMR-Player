#pragma once

#include "playback/PlaybackQueue.h"

#include <QWidget>

class QLabel;
class QImage;
class QGridLayout;
class QPushButton;
class QSlider;
class QMenu;

namespace asmr {

class CoverArtWidget;

// Displays player state and emits user intentions; it owns no playback backend.
class PlaybackControlsWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit PlaybackControlsWidget(QWidget *parent = nullptr);

    void setAudioName(const QString &name);
    void setWorkName(const QString &name);
    void setCoverImage(const QImage &image, const QString &sourcePath = {}, const QString &detail = {});
    void setCoverPlaceholder(const QString &message = QStringLiteral("暂无封面"));
    void setPlaybackDetailVisible(bool visible);
    void setSubtitleSelectionAvailable(bool available);
    void setSubtitlePanelVisible(bool visible);
    void setFloatingSubtitleVisible(bool visible);
    void setQueueNavigationAvailable(bool previous, bool next);
    void setQueueState(int count, bool visible);
    void setMediaAvailable(bool available);
    void setPosition(qint64 positionMs);
    void setDuration(qint64 durationMs);
    void setSeekable(bool seekable);
    void setPlaying(bool playing);
    void setVolumePercent(int volume);
    void setPlaybackMode(PlaybackMode mode);
    void setResumePending(bool pending);

signals:
    void playPauseRequested();
    void seekRequested(qint64 positionMs);
    void volumeChangeRequested(int volume);
    void playbackModeRequested(asmr::PlaybackMode mode);
    void selectSubtitleRequested();
    void toggleSubtitlePanelRequested();
    void toggleFloatingSubtitleRequested();
    void previousRequested();
    void nextRequested();
    void toggleQueueRequested();
    void togglePlaybackDetailRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    [[nodiscard]] bool canSeek() const;
    void updateSeekControls();
    void cancelSeekDrag();
    void updateTimeDisplay(qint64 positionMs);
    void updateBarLayout();
    void updatePlayAction(bool playing);
    void updateSubtitlePanelAction();
    void togglePopup(QMenu *popup, QPushButton *opener);
    void hidePopups();
    void updateVolumeDisplay();

    QLabel *m_audioNameLabel = nullptr;
    CoverArtWidget *m_coverLabel = nullptr;
    QPushButton *m_coverButton = nullptr;
    QLabel *m_workNameLabel = nullptr;
    QPushButton *m_playPauseButton = nullptr;
    QPushButton *m_playbackModeButton = nullptr;
    QPushButton *m_volumeButton = nullptr;
    QPushButton *m_muteButton = nullptr;
    QMenu *m_playbackModePopup = nullptr;
    QMenu *m_volumePopup = nullptr;
    QLabel *m_volumePercentLabel = nullptr;
    PlaybackMode m_playbackMode = PlaybackMode::Sequential;
    int m_lastNonzeroVolume = 70;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_toggleQueueButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    QPushButton *m_selectSubtitleButton = nullptr;
    QPushButton *m_toggleSubtitlePanelButton = nullptr;
    QPushButton *m_toggleFloatingButton = nullptr;
    QWidget *m_identity = nullptr;
    QWidget *m_transport = nullptr;
    QWidget *m_timeline = nullptr;
    QWidget *m_auxiliary = nullptr;
    QGridLayout *m_barLayout = nullptr;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    bool m_mediaAvailable = false;
    bool m_playbackDetailVisible = false;
    bool m_subtitlePanelVisible = false;
    bool m_seekable = false;
    bool m_resumePending = false;
    int m_barLayoutMode = 2;
    // Validity of this UI drag only, not another playback state machine.
    bool m_userSeeking = false;
};

} // namespace asmr
