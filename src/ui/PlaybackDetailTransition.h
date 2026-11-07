#pragma once

#include <QPixmap>
#include <QWidget>

class QPropertyAnimation;

namespace asmr {

// Paints two already-laid-out pages without moving or laying out their live
// widgets. Playback and keyboard focus remain owned by the main window.
class PlaybackDetailTransition final : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal progress READ progress WRITE setProgress)

public:
    explicit PlaybackDetailTransition(QWidget *parent = nullptr);

    // Snapshots may have different DPRs, but must have the same logical size.
    // Invalid input cancels the transition, clears both snapshots and hides it.
    bool setSnapshots(const QPixmap &library, const QPixmap &detail);
    bool hasSnapshots() const;
    void clearSnapshots();

    qreal progress() const;
    void setProgress(qreal progress);
    bool isRunning() const;
    bool targetDetail() const;

    void animateTo(bool detail);
    void snapTo(bool detail);
    void cancel();

signals:
    // Only a completed animateTo emits this signal. snapTo/cancel never do.
    void finished(bool detail);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap m_librarySnapshot;
    QPixmap m_detailSnapshot;
    QPropertyAnimation *m_animation = nullptr;
    qreal m_progress = 0.0;
    quint64 m_completionGeneration = 0;
    bool m_targetDetail = false;
    bool m_completionPending = false;
};

} // namespace asmr
