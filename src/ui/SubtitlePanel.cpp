#include "ui/SubtitlePanel.h"
#include "ui/CoverArtWidget.h"
#include "ui/NavigationIcons.h"

#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>

#include <algorithm>

namespace asmr {
namespace {

class TranscriptTextEdit final : public QTextEdit
{
public:
    explicit TranscriptTextEdit(QWidget *parent) : QTextEdit(parent)
    {
        setAcceptRichText(false);
        setUndoRedoEnabled(false);
        connect(document(), &QTextDocument::contentsChanged, this, [this] {
            m_layoutDirty = true;
        });
    }

    void invalidateLayout() { m_layoutDirty = true; }

    void prepareLayout() const
    {
        if (!m_layoutDirty) return;
        // Finish pending layout once per content/width/font change. Subsequent
        // animation frames only move QTextEdit's native pixel scroll position.
        (void)document()->documentLayout()->documentSize();
        m_layoutDirty = false;
    }

    void setDetailInsetsEnabled(bool enabled)
    {
        m_detailInsetsEnabled = enabled;
        updateReadingInsets();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        m_layoutDirty = true;
        QTextEdit::resizeEvent(event);
        updateReadingInsets();
    }

    void paintEvent(QPaintEvent *event) override
    {
        if (!document()->isEmpty() || placeholderText().isEmpty()) {
            QTextEdit::paintEvent(event);
            return;
        }
        // The empty-state hint belongs to the entire reading space below the
        // cover, rather than the first editable line of an empty document.
        QPainter painter(viewport());
        if (viewport()->autoFillBackground())
            painter.fillRect(event->rect(), viewport()->palette().brush(QPalette::Base));
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.setFont(font());
        painter.drawText(viewport()->rect().adjusted(8, 8, -8, -8),
                         Qt::AlignCenter | Qt::TextWordWrap, placeholderText());
    }

public:
    [[nodiscard]] int saveTopPosition()
    {
        const QTextCursor anchor = cursorForPosition(QPoint(0, 0));
        m_savedAnchorY = cursorRect(anchor).top();
        return anchor.position();
    }

    void restoreTopPosition(int position)
    {
        QTextCursor anchor(document());
        anchor.setPosition(std::clamp(position, 0, document()->characterCount() - 1));
        prepareLayout();
        verticalScrollBar()->setValue(verticalScrollBar()->value()
            + cursorRect(anchor).top() - m_savedAnchorY);
    }

    [[nodiscard]] int scrollPositionForRange(const QTextCursor &first, const QTextCursor &last) const
    {
        prepareLayout();
        const QRect firstRect = cursorRect(first);
        const QRect lastRect = cursorRect(last);
        const qreal readableHeight = std::max(0, viewport()->height() - 16);
        const qreal cueHeight = lastRect.bottom() - firstRect.top() + 1;
        // A cue taller than the reading area starts at its head; shorter cues
        // center exactly, without quantizing the target to whole text lines.
        const qreal precedingHeight = std::max<qreal>(0, (readableHeight - cueHeight) / 2);
        return qRound(verticalScrollBar()->value() + firstRect.top() - 8 - precedingHeight);
    }

    [[nodiscard]] bool positionIsVisible(int position) const
    {
        const int first = cursorForPosition(QPoint(0, 0)).position();
        const int last = cursorForPosition(viewport()->rect().bottomRight()).position();
        return position >= first && position <= last;
    }

    [[nodiscard]] bool textLineContains(const QPoint &point, const QTextCursor &cursor) const
    {
        const QTextBlock block = cursor.block();
        if (!block.isValid() || !block.layout()) return false;
        const QRectF blockRect = document()->documentLayout()->blockBoundingRect(block)
            .translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        // A click on the trailing half of a wrapped glyph can put the nearest
        // insertion cursor at the next line's start. Hit-test the visual line
        // under the mouse rather than the line containing that cursor.
        for (int index = 0; index < block.layout()->lineCount(); ++index) {
            const QTextLine line = block.layout()->lineAt(index);
            // The glyph origin includes the layout's alignment offset. A line's
            // x() alone still starts at the left margin for centered text.
            const QRectF lineRect = line.naturalTextRect().translated(blockRect.topLeft());
            if (lineRect.adjusted(-2, 0, 2, 0).contains(point)) return true;
        }
        return false;
    }

private:
    void updateReadingInsets()
    {
        // Keep the first screen of wide details away from the upper edge.
        // Symmetric insets preserve the current cue's existing center axis;
        // short windows give the space back to the wrapped transcript.
        const int inset = m_detailInsetsEnabled
            ? std::min(48, std::max(0, (height() - 160) / 8)) : 0;
        const QMargins margins(0, inset, 0, inset);
        if (viewportMargins() != margins) setViewportMargins(margins);
    }

    bool m_detailInsetsEnabled = false;
    mutable bool m_layoutDirty = true;
    int m_savedAnchorY = 0;
};

// The artwork itself has a fixed square geometry, but must not impose that
// minimum on the panel when the available reading height becomes smaller.
class TranscriptCoverContainer final : public QWidget
{
public:
    explicit TranscriptCoverContainer(QWidget *parent) : QWidget(parent)
    {
        m_cover = new CoverArtWidget(this);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

    [[nodiscard]] CoverArtWidget *cover() const { return m_cover; }
    [[nodiscard]] QSize sizeHint() const override { return QSize(m_side, m_side); }
    [[nodiscard]] QSize minimumSizeHint() const override { return QSize(0, 0); }

    void setPreferredSide(int side)
    {
        if (m_side == side) return;
        m_side = side;
        setMaximumSize(side, side);
        updateGeometry();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        const int side = std::min(width(), height());
        m_cover->setFixedSize(side, side);
        m_cover->move((width() - side) / 2, (height() - side) / 2);
    }

private:
    CoverArtWidget *m_cover = nullptr;
    int m_side = 0;
};

} // namespace

SubtitlePanel::SubtitlePanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("subtitlePanel"));
    setProperty("uiRole", QStringLiteral("surface"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumWidth(0);

    m_collapseButton = new QPushButton(this);
    m_collapseButton->setObjectName(QStringLiteral("collapseSubtitlePanelButton"));
    m_collapseButton->setIcon(navigationChevronIcon(Qt::RightArrow));
    m_collapseButton->setIconSize(QSize(16, 16));
    m_collapseButton->setFixedSize(24, 24);
    m_collapseButton->setToolTip(QStringLiteral("收起侧栏字幕"));
    m_collapseButton->setAccessibleName(QStringLiteral("收起侧栏字幕"));
    m_collapseButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid transparent; padding: 0;"
        " min-height: 0; min-width: 0; border-radius: 4px; }"
        "QPushButton:hover { background: #343D47; }"
        "QPushButton:focus { border-color: #69C8A5; }"));
    connect(m_collapseButton, &QPushButton::clicked, this, [this] {
        if (!m_detailMode) emit collapseRequested();
    });

    auto *coverContainer = new TranscriptCoverContainer(this);
    m_coverContainer = coverContainer;
    m_coverContainer->setObjectName(QStringLiteral("subtitleCoverContainer"));
    m_cover = coverContainer->cover();
    m_cover->setObjectName(QStringLiteral("subtitleCoverArt"));

    m_resumeFollowingButton = new QPushButton(QStringLiteral("回到当前字幕"), this);
    m_resumeFollowingButton->setObjectName(QStringLiteral("subtitleResumeFollowingButton"));
    m_resumeFollowingButton->setAccessibleDescription(QStringLiteral("恢复跟随当前播放字幕"));
    m_resumeFollowingButton->hide();

    m_textDisplay = new TranscriptTextEdit(this);
    m_textDisplay->setObjectName(QStringLiteral("subtitleTextDisplay"));
    m_textDisplay->setAccessibleName(QStringLiteral("字幕全文"));
    m_textDisplay->setReadOnly(true);
    m_textDisplay->setTextInteractionFlags(Qt::NoTextInteraction);
    m_textDisplay->setFocusPolicy(Qt::StrongFocus);
    m_textDisplay->setCursorWidth(0);
    m_textDisplay->viewport()->setCursor(Qt::ArrowCursor);
    m_textDisplay->setContextMenuPolicy(Qt::PreventContextMenu);
    m_textDisplay->setFrameShape(QFrame::NoFrame);
    m_textDisplay->setLineWrapMode(QTextEdit::WidgetWidth);
    // Keep scrolling and its range, without exposing scrollbar chrome.
    m_textDisplay->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textDisplay->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textDisplay->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_textDisplay->setStyleSheet(QStringLiteral(
        "QTextEdit#subtitleTextDisplay { color: #8F98A6; }"));
    m_textDisplay->document()->setDocumentMargin(8);
    m_textDisplay->setMinimumHeight(80);
    m_textDisplay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_textDisplay->installEventFilter(this);
    m_textDisplay->viewport()->installEventFilter(this);
    m_textDisplay->verticalScrollBar()->installEventFilter(this);
    m_followAnimation = new QPropertyAnimation(m_textDisplay->verticalScrollBar(), "value", this);
    m_followAnimation->setObjectName(QStringLiteral("subtitleFollowAnimation"));
    m_followAnimation->setDuration(160);
    m_followAnimation->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_followAnimation, &QPropertyAnimation::finished, this, [this] {
        m_seekFollowActive = false;
    });
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    m_clickTimer->setTimerType(Qt::PreciseTimer);
    connect(m_clickTimer, &QTimer::timeout, this, [this]() {
        const int cue = m_pendingCue;
        cancelPendingClick();
        if (!m_seekEnabled || m_loading || !isVisible()
            || m_clickGeneration != m_entriesGeneration
            || cue < 0 || cue >= m_cueRanges.size()
            || m_textDisplay->textCursor().hasSelection()) return;
        // MainWindow revalidates the media transition before accepting this intent.
        emit seekRequested(m_cueRanges.at(cue).entry.startMs);
    });
    connect(m_textDisplay->verticalScrollBar(), &QScrollBar::sliderPressed,
            this, &SubtitlePanel::pauseFollowing);
    connect(m_textDisplay->verticalScrollBar(), &QScrollBar::actionTriggered,
            this, [this]() { pauseFollowing(); });
    connect(m_resumeFollowingButton, &QPushButton::clicked,
            this, &SubtitlePanel::resumeFollowing);

    m_contentLayout = new QGridLayout(this);
    m_contentLayout->setContentsMargins(14, 14, 14, 14);
    m_contentLayout->setSpacing(8);
    updateDetailLayout();

    setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(m_textDisplay);
    setTabOrder(m_collapseButton, m_textDisplay);
    setTabOrder(m_textDisplay, m_resumeFollowingButton);

    updatePresentation();
}

void SubtitlePanel::setImportEnabled(bool enabled)
{
    m_importEnabled = enabled;
    updatePresentation();
}

void SubtitlePanel::setSelectionEnabled(bool enabled)
{
    m_selectionEnabled = enabled;
    updatePresentation();
}

void SubtitlePanel::setLoading(bool loading)
{
    m_loading = loading;
    if (loading) {
        m_loadFailed = false;
        cancelPendingClick();
        m_seekFollowPending = false;
        stopFollowingAnimation();
    }
    updatePresentation();
}

void SubtitlePanel::setFileInfo(const QString &summary, const QString &diagnostics)
{
    m_hasFileInfo = !summary.isEmpty();
    m_fileSummary = summary;
    m_diagnosticDetails = diagnostics;
    m_loadFailed = false;
    updatePresentation();
}

void SubtitlePanel::clearFileInfo()
{
    m_hasFileInfo = false;
    m_fileSummary.clear();
    m_diagnosticDetails.clear();
    m_loadFailed = false;
    // A binding change does not own the current request or the shared subtitle text.
    updatePresentation();
}

void SubtitlePanel::showLoadFailure()
{
    m_loading = false;
    m_loadFailed = true;
    updatePresentation();
}

void SubtitlePanel::setLibraryStatus(const QString &message)
{
    m_libraryStatus = message;
    updatePresentation();
}

void SubtitlePanel::setSubtitleText(const QString &text)
{
    m_activeText = text;
    // Playback snapshots update only the active caption once a transcript is installed.
    if (m_cueRanges.isEmpty() && m_textDisplay->toPlainText() != text)
        m_textDisplay->setPlainText(text);
    updatePresentation();
}

void SubtitlePanel::setSubtitleEntries(const QList<SubtitleEntry> &entries)
{
    cancelPendingClick();
    m_seekFollowPending = false;
    stopFollowingAnimation();
    ++m_entriesGeneration;
    m_cueRanges.clear();
    m_activeCueIndices.clear();
    m_activeText.clear();
    m_manualViewPosition = -1;
    m_following = true;
    m_mousePressed = false;
    m_pressedCue = -1;
    m_cueRanges.reserve(entries.size());
    for (const SubtitleEntry &entry : entries)
        m_cueRanges.append(CueRange{entry, 0, 0});
    std::stable_sort(m_cueRanges.begin(), m_cueRanges.end(),
                     [](const CueRange &left, const CueRange &right) {
        return left.entry.startMs < right.entry.startMs;
    });

    QString transcript;
    bool firstCue = true;
    for (CueRange &cue : m_cueRanges) {
        if (!firstCue) transcript += QStringLiteral("\n\n");
        firstCue = false;
        cue.documentStart = transcript.size();
        // Match the document's newline normalization before recording cursor offsets.
        QString displayText = cue.entry.text;
        displayText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        displayText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        transcript += displayText;
        cue.documentEnd = transcript.size();
    }
    m_textDisplay->setExtraSelections({});
    m_textDisplay->setPlainText(transcript);
    m_textDisplay->verticalScrollBar()->setValue(0);
    updatePresentation();
}

void SubtitlePanel::setPlaybackPosition(qint64 positionMs, bool playbackStopped, bool playing)
{
    const bool wasPlaying = m_playing;
    m_playing = playing && !playbackStopped;
    // Paused media still sends position snapshots. A click's presentation
    // animation is independent of playback and must survive those snapshots.
    const bool clicked = m_seekFollowActive && !playbackStopped;
    if (!m_playing && !clicked) stopFollowingAnimation();
    QList<int> active;
    if (!playbackStopped && positionMs >= 0) {
        for (int index = 0; index < m_cueRanges.size(); ++index) {
            const SubtitleEntry &entry = m_cueRanges.at(index).entry;
            if (entry.startMs > positionMs) break;
            if (positionMs < entry.endMs) active.append(index);
        }
    }
    if (active == m_activeCueIndices) {
        if (m_playing && !wasPlaying && !clicked) followCurrentCue(true);
        return;
    }
    stopFollowingAnimation();
    m_activeCueIndices = active;
    QList<QTextEdit::ExtraSelection> highlights;
    for (int index : active) {
        const CueRange &cue = m_cueRanges.at(index);
        QTextEdit::ExtraSelection highlight;
        highlight.cursor = QTextCursor(m_textDisplay->document());
        highlight.cursor.setPosition(cue.documentStart);
        highlight.cursor.setPosition(cue.documentEnd, QTextCursor::KeepAnchor);
        highlight.format.setForeground(QColor(QStringLiteral("#41BFA1")));
        highlights.append(highlight);
    }
    m_textDisplay->setExtraSelections(highlights);
    followCurrentCue(m_playing || clicked, clicked);
}

void SubtitlePanel::setSeekEnabled(bool enabled)
{
    m_seekEnabled = enabled;
    if (!enabled) {
        cancelPendingClick();
        m_seekFollowPending = false;
        if (m_seekFollowActive) stopFollowingAnimation();
    }
}

void SubtitlePanel::cancelPendingSeek()
{
    cancelPendingClick();
    m_seekFollowPending = false;
    if (m_seekFollowActive) stopFollowingAnimation();
}

void SubtitlePanel::prepareForSubtitleSeek()
{
    cancelPendingClick();
    stopFollowingAnimation();
    m_seekFollowPending = true;
    m_following = true;
    // Hiding the return button can resize the reading area. Keep media
    // notifications from starting an animation before that layout settles.
    updatePresentation();
    scheduleFollowing();
}

void SubtitlePanel::resumeFollowing()
{
    cancelPendingClick();
    m_seekFollowPending = false;
    m_following = true;
    // Keep any internal cursor position inert while resuming timeline following.
    QTextCursor cursor = m_textDisplay->textCursor();
    cursor.clearSelection();
    m_textDisplay->setTextCursor(cursor);
    updatePresentation();
    followCurrentCue(m_playing);
}

void SubtitlePanel::setCoverImage(const QImage &image, const QString &sourcePath,
                                 const QString &detail)
{
    m_cover->setImage(image, sourcePath, detail);
}

void SubtitlePanel::setCoverPlaceholder(const QString &message)
{
    m_cover->clearImage(message);
}

void SubtitlePanel::setDetailMode(bool enabled)
{
    if (m_detailMode == enabled) return;
    if (!m_following && m_manualViewPosition < 0)
        m_manualViewPosition = static_cast<TranscriptTextEdit *>(m_textDisplay)->saveTopPosition();
    cancelPendingClick();
    m_seekFollowPending = false;
    stopFollowingAnimation();
    m_detailMode = enabled;
    // Apply alignment at layout level so entering/leaving details does not
    // change subtitle text, cue offsets, block formats or document revision.
    QTextOption textOption = m_textDisplay->document()->defaultTextOption();
    textOption.setAlignment(enabled ? Qt::AlignHCenter : Qt::AlignLeft);
    m_textDisplay->document()->setDefaultTextOption(textOption);
    static_cast<TranscriptTextEdit *>(m_textDisplay)->invalidateLayout();
    setProperty("uiRole", enabled ? QStringLiteral("detailContent") : QStringLiteral("surface"));
    // The enclosing detail page owns the artwork-derived background.
    setStyleSheet(enabled ? QStringLiteral(
        "QWidget#subtitlePanel { background: transparent; border: none; }") : QString());
    m_textDisplay->setStyleSheet(enabled ? QStringLiteral(
        "QTextEdit#subtitleTextDisplay { color: #ADB5C1; background: transparent; border: none; }")
        : QStringLiteral("QTextEdit#subtitleTextDisplay { color: #8F98A6; }"));
    m_textDisplay->viewport()->setAutoFillBackground(!enabled);
    updateDetailLayout();
    updateCoverSize();
    scheduleFollowing();
}

QString SubtitlePanel::subtitleText() const
{
    return m_activeText;
}

QString SubtitlePanel::fileSummary() const
{
    return m_hasFileInfo ? m_fileSummary : QStringLiteral("未加载字幕");
}

bool SubtitlePanel::canOpenSubtitle() const
{
    return m_importEnabled && !m_loading;
}

bool SubtitlePanel::canSelectSubtitle() const
{
    return m_selectionEnabled;
}

bool SubtitlePanel::hasSubtitleInformation() const
{
    return m_hasFileInfo || !m_libraryStatus.isEmpty() || !m_diagnosticDetails.isEmpty();
}

QString SubtitlePanel::libraryStatus() const
{
    return m_libraryStatus;
}

QString SubtitlePanel::diagnosticDetails() const
{
    return m_diagnosticDetails;
}

void SubtitlePanel::cancelPendingClick()
{
    const bool wasPending = m_pendingCue >= 0;
    m_clickTimer->stop();
    m_pendingCue = -1;
    m_pressedCue = -1;
    // A rejected/cancelled intent must also release the temporary hold. Read
    // the latest cue in the queued callback, never the earlier clicked cue.
    if (wasPending) scheduleFollowing();
}

void SubtitlePanel::pauseFollowing()
{
    cancelPendingClick();
    m_seekFollowPending = false;
    stopFollowingAnimation();
    if (m_cueRanges.isEmpty() || !m_following) return;
    m_following = false;
    updatePresentation();
}

void SubtitlePanel::stopFollowingAnimation()
{
    m_seekFollowActive = false;
    m_followAnimation->stop();
}

void SubtitlePanel::followCurrentCue(bool animate, bool clicked)
{
    if (!m_following || m_loading || m_mousePressed || m_pendingCue >= 0
        || m_seekFollowPending || !isVisible()
        || m_activeCueIndices.isEmpty()) return;
    const CueRange &cue = m_cueRanges.at(m_activeCueIndices.first());
    QTextCursor first(m_textDisplay->document());
    first.setPosition(cue.documentStart);
    QTextCursor last(m_textDisplay->document());
    last.setPosition(cue.documentEnd);
    const auto *transcript = static_cast<TranscriptTextEdit *>(m_textDisplay);
    const bool nearby = transcript->positionIsVisible(first.position());
    const int wantedPosition = transcript->scrollPositionForRange(first, last);
    QScrollBar *scroll = m_textDisplay->verticalScrollBar();
    const int target = std::clamp(wantedPosition,
        scroll->minimum(), scroll->maximum());
    stopFollowingAnimation();
    if (target == scroll->value()) return;
    // Layout is stable before starting. A clicked long cue may begin above the
    // viewport; ordinary distant timeline changes retain their direct placement.
    if (animate && (nearby || clicked)) {
        m_seekFollowActive = clicked;
        m_followAnimation->setDuration(clicked ? 220 : 160);
        m_followAnimation->setEasingCurve(clicked ? QEasingCurve::OutCubic : QEasingCurve::InOutQuad);
        m_followAnimation->setStartValue(scroll->value());
        m_followAnimation->setEndValue(target);
        m_followAnimation->start();
    } else {
        scroll->setValue(target);
    }
}

void SubtitlePanel::scheduleFollowing()
{
    if (m_followQueued) return;
    m_followQueued = true;
    const quint64 generation = ++m_followRequestGeneration;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != m_followRequestGeneration || !m_followQueued) return;
        applyFollowingLayout();
    });
}

void SubtitlePanel::prepareForSnapshot()
{
    stopFollowingAnimation();
    ++m_followRequestGeneration;
    layout()->activate();
    const int manualPosition = m_following ? -1 : (m_manualViewPosition >= 0
        ? m_manualViewPosition : static_cast<TranscriptTextEdit *>(m_textDisplay)->saveTopPosition());
    // Settle this scroll area's pending geometry (including detail reading
    // insets) before capture, without dispatching input or timers. Viewport
    // changes can rewrap nearby text, so repeat a bounded number of times
    // while keeping the same reading anchor.
    for (int pass = 0; pass < 3; ++pass) {
        QEvent beforeFollowing(QEvent::LayoutRequest);
        QApplication::sendEvent(m_textDisplay, &beforeFollowing);
        const QSize followedViewport = m_textDisplay->viewport()->size();
        if (manualPosition >= 0) m_manualViewPosition = manualPosition;
        applyFollowingLayout(false);
        QEvent afterFollowing(QEvent::LayoutRequest);
        QApplication::sendEvent(m_textDisplay, &afterFollowing);
        if (m_textDisplay->viewport()->size() == followedViewport) break;
    }
    // Viewport resizes above may queue fresh follow work. It has already been
    // applied synchronously; neither it nor an older callback may move the view.
    ++m_followRequestGeneration;
    m_followQueued = false;
}

void SubtitlePanel::applyFollowingLayout(bool animateClick)
{
    // Reparenting can hide the view until its destination is ready. Retain a
    // manual anchor until then, whether called by the timer or snapshot path.
    if (!m_following && m_manualViewPosition >= 0 && !isVisible()) {
        m_followQueued = false;
        return;
    }
    if (m_seekFollowPending || m_manualViewPosition >= 0) {
        layout()->activate();
        // The return button and the editor's viewport/insets must agree before
        // resolving the target. Keep queued work coalesced during this resize.
        QEvent request(QEvent::LayoutRequest);
        QApplication::sendEvent(m_textDisplay, &request);
    }
    const bool clicked = m_seekFollowPending;
    m_followQueued = false;
    m_seekFollowPending = false;
    const int manualPosition = m_manualViewPosition;
    m_manualViewPosition = -1;
    if (!m_following && manualPosition >= 0)
        static_cast<TranscriptTextEdit *>(m_textDisplay)->restoreTopPosition(manualPosition);
    else if (clicked || !m_seekFollowActive)
        followCurrentCue(clicked && animateClick, clicked && animateClick);
}
int SubtitlePanel::cueAt(const QPoint &position) const
{
    if (!m_textDisplay->viewport()->rect().contains(position)) return -1;
    const QTextCursor cursor = m_textDisplay->cursorForPosition(position);
    if (!static_cast<TranscriptTextEdit *>(m_textDisplay)->textLineContains(position, cursor))
        return -1;
    for (int index = 0; index < m_cueRanges.size(); ++index) {
        const CueRange &cue = m_cueRanges.at(index);
        if (cursor.position() >= cue.documentStart && cursor.position() <= cue.documentEnd
            && cue.documentEnd > cue.documentStart) return index;
    }
    return -1;
}

bool SubtitlePanel::eventFilter(QObject *watched, QEvent *event)
{
    const bool viewport = watched == m_textDisplay->viewport();
    const bool editor = watched == m_textDisplay;
    const bool scrollbar = watched == m_textDisplay->verticalScrollBar();
    if (editor && event->type() == QEvent::FocusOut)
        cancelPendingClick();
    if ((viewport || scrollbar) && event->type() == QEvent::Wheel)
        pauseFollowing();
    if (scrollbar && event->type() == QEvent::MouseButtonPress)
        pauseFollowing();

    if (editor && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        cancelPendingClick();
        if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll)) {
            event->accept();
            return true;
        }
        switch (key->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            pauseFollowing();
            break;
        default:
            break;
        }
    }
    if (viewport && event->type() == QEvent::Resize) {
        stopFollowingAnimation();
        scheduleFollowing();
    }
    if (editor && (event->type() == QEvent::FontChange
                   || event->type() == QEvent::ApplicationFontChange
                   || event->type() == QEvent::StyleChange)) {
        static_cast<TranscriptTextEdit *>(m_textDisplay)->invalidateLayout();
        stopFollowingAnimation();
        scheduleFollowing();
    }

    if (viewport && event->type() == QEvent::MouseButtonDblClick) {
        cancelPendingClick();
        m_mousePressed = false;
        m_pressedCue = -1;
    } else if (viewport && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        cancelPendingClick();
        m_mousePressed = mouse->button() == Qt::LeftButton;
        m_mouseDragged = false;
        m_pressPosition = mouse->position().toPoint();
        m_pressedCue = m_mousePressed && mouse->modifiers() == Qt::NoModifier
            && m_seekEnabled && !m_loading ? cueAt(m_pressPosition) : -1;
        m_seekFollowPending = false;
        stopFollowingAnimation();
    } else if (viewport && event->type() == QEvent::MouseMove && m_mousePressed) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if ((mouse->position().toPoint() - m_pressPosition).manhattanLength()
            >= QApplication::startDragDistance()) {
            m_mouseDragged = true;
            cancelPendingClick();
        }
    } else if (viewport && event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const int pressed = m_pressedCue;
        const bool wasPressed = m_mousePressed;
        m_mousePressed = false;
        m_pressedCue = -1;
        if (mouse->button() == Qt::LeftButton && wasPressed && !m_mouseDragged
            && mouse->modifiers() == Qt::NoModifier && pressed >= 0
            && cueAt(mouse->position().toPoint()) == pressed
            && !m_textDisplay->textCursor().hasSelection()) {
            m_pendingCue = pressed;
            m_clickGeneration = m_entriesGeneration;
            m_clickTimer->start(QApplication::doubleClickInterval());
        }
        if (m_pendingCue < 0) scheduleFollowing();
    }
    return QWidget::eventFilter(watched, event);
}

void SubtitlePanel::hideEvent(QHideEvent *event)
{
    cancelPendingClick();
    m_seekFollowPending = false;
    stopFollowingAnimation();
    m_mousePressed = false;
    m_pressedCue = -1;
    QWidget::hideEvent(event);
}

void SubtitlePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateCoverSize();
    scheduleFollowing();
}

void SubtitlePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_collapseButton->move(std::max(0, width() - m_collapseButton->width() - 6), 6);
    m_collapseButton->raise();
    if (m_detailColumns != (m_detailMode && width() >= 600)) {
        if (!m_following && m_manualViewPosition < 0)
            m_manualViewPosition = static_cast<TranscriptTextEdit *>(m_textDisplay)->saveTopPosition();
        updateDetailLayout();
        scheduleFollowing();
    }
    updateCoverSize();
}

void SubtitlePanel::updateDetailLayout()
{
    m_collapseButton->setVisible(!m_detailMode);
    m_collapseButton->setEnabled(!m_detailMode);
    if (!m_detailMode) m_collapseButton->raise();
    m_detailColumns = m_detailMode && width() >= 600;
    static_cast<TranscriptTextEdit *>(m_textDisplay)->setDetailInsetsEnabled(m_detailColumns);
    for (QWidget *widget : {m_coverContainer, static_cast<QWidget *>(m_textDisplay),
        static_cast<QWidget *>(m_resumeFollowingButton)})
        m_contentLayout->removeWidget(widget);
    for (int row = 0; row < 6; ++row) m_contentLayout->setRowStretch(row, 0);
    m_contentLayout->setColumnStretch(0, m_detailColumns ? 44 : 1);
    m_contentLayout->setColumnStretch(1, m_detailColumns ? 56 : 0);
    if (m_detailColumns) {
        // Span both reading and return-button rows so showing the button does
        // not move or shrink the independently centered artwork.
        m_contentLayout->addWidget(m_coverContainer, 0, 0, 2, 1, Qt::AlignCenter);
        m_contentLayout->addWidget(m_textDisplay, 0, 1);
        m_contentLayout->addWidget(m_resumeFollowingButton, 1, 1, Qt::AlignHCenter);
        m_contentLayout->setRowStretch(0, 1);
    } else {
        m_contentLayout->addWidget(m_coverContainer, 0, 0, Qt::AlignHCenter);
        m_contentLayout->addWidget(m_textDisplay, 1, 0);
        m_contentLayout->addWidget(m_resumeFollowingButton, 2, 0, Qt::AlignHCenter);
        m_contentLayout->setRowStretch(1, 1);
    }
}

void SubtitlePanel::updateCoverSize()
{
    const QMargins margins = layout()->contentsMargins();
    const int spacing = layout()->spacing();
    const int usableHeight = std::max(0, height() - margins.top() - margins.bottom());
    const int usableWidth = std::max(0, width() - margins.left() - margins.right());
    const int coverWidth = m_detailColumns
        ? std::max(0, (usableWidth - spacing) * 44 / 100) : usableWidth;
    const int resumeHeight = m_resumeFollowingButton->isHidden() ? 0
        : m_resumeFollowingButton->sizeHint().height() + spacing;
    // Wide details use the whole left column. Stacked/narrow layouts reserve
    // at least half the height for reading and shrink artwork first.
    const int readingHeight = m_detailColumns ? 0
        : std::max(m_textDisplay->minimumHeight(), usableHeight / 2);
    const int coverHeight = m_detailColumns ? usableHeight
        : usableHeight - readingHeight - resumeHeight - spacing;
    const int coverLimit = m_detailColumns ? 440 : (m_detailMode ? 180 : 200);
    const int side = std::max(0, std::min({coverLimit, coverWidth, coverHeight}));
    static_cast<TranscriptCoverContainer *>(m_coverContainer)->setPreferredSide(side);
}

void SubtitlePanel::updatePresentation()
{
    m_resumeFollowingButton->setVisible(!m_following && !m_cueRanges.isEmpty());
    m_resumeFollowingButton->setEnabled(!m_loading);
    updateCoverSize();

    QString state;
    if (m_loading) {
        state = QStringLiteral("正在加载字幕...");
    } else if (m_loadFailed) {
        state = m_hasFileInfo ? QStringLiteral("字幕加载失败，保留原字幕")
                              : QStringLiteral("字幕加载失败，未加载字幕");
    } else if (m_hasFileInfo) {
        state = QStringLiteral("当前暂无字幕");
    } else {
        state = QStringLiteral("未加载字幕");
    }
    // A placeholder is painted only over an empty document. Loading/failure states
    // never replace a valid transcript or disturb its reading position.
    m_textDisplay->setPlaceholderText(state);
    const bool openEnabled = canOpenSubtitle();
    const bool selectionEnabled = canSelectSubtitle();
    const bool informationAvailable = hasSubtitleInformation();
    if (m_lastOpenEnabled != openEnabled || m_lastSelectionEnabled != selectionEnabled
        || m_lastInformationAvailable != informationAvailable) {
        m_lastOpenEnabled = openEnabled;
        m_lastSelectionEnabled = selectionEnabled;
        m_lastInformationAvailable = informationAvailable;
        emit subtitleActionsChanged(openEnabled, selectionEnabled, informationAvailable);
    }
}

} // namespace asmr
