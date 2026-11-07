#include "ui/LibraryPanel.h"
#include "ui/ApplicationBrand.h"
#include "ui/CoverArtWidget.h"
#include "artwork/CoverArtLoader.h"

#include <QAbstractListModel>
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPersistentModelIndex>
#include <QPolygonF>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QToolButton>
#include <QTimer>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QMargins>
#include <QSet>
#include <QStringList>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <utility>

namespace asmr {
namespace {

QString safeToolTip(const QString &text)
{
    return QStringLiteral("<qt>%1</qt>").arg(
        text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
}

QLabel *plainLabel(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

enum class LibraryIcon { Folder, Audio, Add, Ai, Settings, Refresh, Edit };

class SidebarButton final : public QPushButton {
public:
    using QPushButton::QPushButton;

    [[nodiscard]] QPoint iconCenter() const
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.features.setFlag(QStyleOptionButton::HasMenu, false);
        const QRect contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        // The sidebar's stylesheet left-aligns the icon/text label. Query its
        // content box so borders and compact-style padding are included.
        return QPoint(contents.left() + option.iconSize.width() / 2, contents.center().y());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        // The indicator is hidden by the sidebar theme. Normalize its layout
        // hint too, so menu and action rows share icon/text positions.
        option.features.setFlag(QStyleOptionButton::HasMenu, false);
        QPainter painter(this);
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
    }
};

QIcon lineIcon(LibraryIcon kind, const QColor &color = QColor(QStringLiteral("#B8BEC8")))
{
    QPixmap pixmap(48, 48);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (kind) {
    case LibraryIcon::Folder: {
        QPainterPath path;
        path.moveTo(3, 7); path.lineTo(3, 19); path.lineTo(21, 19);
        path.lineTo(21, 7); path.lineTo(12, 7); path.lineTo(10, 4); path.lineTo(3, 4);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case LibraryIcon::Audio:
        painter.drawLine(QPointF(9, 17), QPointF(9, 6));
        painter.drawLine(QPointF(9, 6), QPointF(19, 4));
        painter.drawLine(QPointF(19, 4), QPointF(19, 15));
        painter.drawEllipse(QRectF(3, 16, 6, 4));
        painter.drawEllipse(QRectF(13, 14, 6, 4));
        break;
    case LibraryIcon::Add:
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        painter.drawLine(QPointF(7, 12), QPointF(17, 12));
        painter.drawLine(QPointF(12, 7), QPointF(12, 17));
        break;
    case LibraryIcon::Ai: {
        painter.drawEllipse(QRectF(2, 2, 20, 20));
        painter.save();
        painter.translate(12, 12);
        painter.scale(0.65, 0.65);
        painter.translate(-12, -12);
        painter.setPen(QPen(color, 2.6, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        QPainterPath letter;
        letter.moveTo(10, 20);
        letter.cubicTo(0, 20, 0, 8, 8, 8);
        letter.cubicTo(12.5, 8, 15.5, 11, 15.5, 15.5);
        letter.lineTo(15.5, 20);
        painter.drawPath(letter);
        painter.drawLine(QPointF(20, 8), QPointF(20, 20));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{QPointF(18.5, 6), QPointF(21.5, 3), QPointF(21.5, 6)});
        painter.restore();
        break;
    }
    case LibraryIcon::Settings:
        painter.drawEllipse(QRectF(5, 5, 14, 14));
        painter.drawEllipse(QRectF(9, 9, 6, 6));
        for (int turn = 0; turn < 8; ++turn) {
            painter.save(); painter.translate(12, 12); painter.rotate(turn * 45);
            painter.drawLine(QPointF(0, -7), QPointF(0, -10)); painter.restore();
        }
        break;
    case LibraryIcon::Refresh: {
        QPainterPath path;
        path.moveTo(19, 10); path.cubicTo(17, 3, 7, 3, 5, 9);
        path.cubicTo(2, 16, 10, 22, 16, 18);
        painter.drawPath(path);
        painter.drawPolyline(QPolygonF{QPointF(19, 5), QPointF(19, 10), QPointF(14, 10)});
        break;
    }
    case LibraryIcon::Edit: {
        auto editPen = painter.pen();
        editPen.setWidthF(1.5);
        painter.setPen(editPen);
        QPainterPath outline;
        outline.moveTo(11, 4); outline.lineTo(6, 4);
        outline.quadTo(4, 4, 4, 6); outline.lineTo(4, 18);
        outline.quadTo(4, 20, 6, 20); outline.lineTo(18, 20);
        outline.quadTo(20, 20, 20, 18); outline.lineTo(20, 13);
        painter.drawPath(outline);
        painter.drawLine(QPointF(10, 14), QPointF(21, 3));
        break;
    }
    }
    painter.end();
    return QIcon(pixmap);
}

QIcon workCoverIcon(const QImage &image = {})
{
    QPixmap pixmap(72, 72);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath rounded;
    rounded.addRoundedRect(QRectF(0, 0, 36, 36), 5, 5);
    painter.fillPath(rounded, QColor("#2B313A"));
    if (image.isNull()) {
        lineIcon(LibraryIcon::Audio).paint(&painter, QRect(6, 6, 24, 24));
    } else {
        const qreal side = std::min(image.width(), image.height());
        painter.setClipPath(rounded);
        painter.drawImage(QRectF(0, 0, 36, 36), image,
            QRectF((image.width() - side) / 2, (image.height() - side) / 2, side, side));
    }
    painter.end();
    // Photos keep their actual colors when selected, hovered or disabled.
    // Supplying every mode avoids the style-generated highlight tint.
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled})
        for (const auto state : {QIcon::Off, QIcon::On})
            icon.addPixmap(pixmap, mode, state);
    return icon;
}

// Keep the full plain-text title available to accessibility and tests while
// showing at most two lines, with an ellipsis on the final line.
class WorkTitleLabel final : public QLabel
{
public:
    explicit WorkTitleLabel(QWidget *parent) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    void setMaximumLines(int lines)
    {
        if (m_maximumLines == lines) return;
        m_maximumLines = lines;
        updateGeometry();
        update();
    }
    QSize sizeHint() const override { return {140, fontMetrics().lineSpacing() * m_maximumLines}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().lineSpacing()}; }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.setFont(font());
        QString displayed = text();
        displayed.replace(u'\n', u' ');
        if (m_maximumLines == 1) {
            painter.drawText(QRect(0, 0, width(), fontMetrics().lineSpacing()),
                             Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                             fontMetrics().elidedText(displayed, Qt::ElideRight, width()));
            return;
        }
        QTextLayout layout(displayed, font());
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        auto first = layout.createLine();
        if (!first.isValid()) { layout.endLayout(); return; }
        first.setLineWidth(std::max(0, width()));
        const int remaining = first.textStart() + first.textLength();
        layout.endLayout();
        first.draw(&painter, QPointF(0, 0));
        if (remaining < displayed.size()) {
            const QString tail = fontMetrics().elidedText(displayed.mid(remaining), Qt::ElideRight, width());
            painter.drawText(QRect(0, fontMetrics().lineSpacing(), width(), fontMetrics().lineSpacing()),
                             Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, tail);
        }
    }
private:
    int m_maximumLines = 2;
};

// Keep decoration in the view: the library model retains its existing data and
// sorting contract, including its hidden relative-path column.
class LibraryItemDelegate final : public QStyledItemDelegate
{
public:
    explicit LibraryItemDelegate(bool works, QObject *parent)
        : QStyledItemDelegate(parent), m_works(works), m_folderIcon(lineIcon(LibraryIcon::Folder)),
          m_audioIcon(lineIcon(LibraryIcon::Audio)),
          m_playingIcon(lineIcon(LibraryIcon::Audio, QColor(QStringLiteral("#7FC5B4")))) {}

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() != 0)
            return;
        if (m_works) {
            option->icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            option->features |= QStyleOptionViewItem::HasDecoration;
            option->decorationSize = QSize(36, 36);
            return;
        }
        const bool directory = index.data(LibraryPanel::DirectoryRole).toBool();
        option->icon = directory ? m_folderIcon
            : index.data(LibraryPanel::PlayingRole).toBool() ? m_playingIcon : m_audioIcon;
        option->features |= QStyleOptionViewItem::HasDecoration;
        option->decorationSize = QSize(22, 22);
    }

private:
    bool m_works;
    QIcon m_folderIcon;
    QIcon m_audioIcon;
    QIcon m_playingIcon;
};

// This view owns only presentation state. A whole-row hover is independent of
// the column under the pointer; ordinary QTableView selection and input remain
// responsible for navigation, playback and context-menu intent.
class LibraryTracksTableView final : public QTableView
{
public:
    explicit LibraryTracksTableView(QWidget *parent) : QTableView(parent)
    {
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
    }
    bool isHovered(const QModelIndex &index) const
    {
        return m_hovered.isValid() && m_hovered.row() == index.row()
            && m_hovered.parent() == index.parent();
    }
protected:
    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_pointerInside = true;
        m_pointer = event->position().toPoint();
        updateHoveredRow(indexAt(m_pointer));
        QTableView::mouseMoveEvent(event);
    }
    bool viewportEvent(QEvent *event) override
    {
        if (event->type() == QEvent::Leave) {
            m_pointerInside = false;
            updateHoveredRow({});
        }
        return QTableView::viewportEvent(event);
    }
    void scrollContentsBy(int dx, int dy) override
    {
        QTableView::scrollContentsBy(dx, dy);
        if (m_pointerInside) updateHoveredRow(indexAt(m_pointer));
    }
private:
    void updateHoveredRow(const QModelIndex &index)
    {
        const QModelIndex row = index.isValid() ? index.siblingAtColumn(0) : QModelIndex();
        if (m_hovered == row) return;
        const auto repaintRow = [this](const QModelIndex &entry) {
            if (!entry.isValid()) return;
            const QRect rect = visualRect(entry);
            viewport()->update(QRect(0, rect.top(), viewport()->width(), rect.height()));
        };
        repaintRow(m_hovered);
        m_hovered = row;
        repaintRow(m_hovered);
    }
    QPersistentModelIndex m_hovered;
    QPoint m_pointer;
    bool m_pointerInside = false;
};

constexpr int trackDurationRightPadding = 24;

// Explicit section painting keeps the central header flush with its panel.
// Other library tables and dialogs continue to use their existing Qt style.
class LibraryTracksHeader final : public QHeaderView
{
public:
    explicit LibraryTracksHeader(QWidget *parent) : QHeaderView(Qt::Horizontal, parent) {}
protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override
    {
        if (!rect.isValid()) return;
        painter->save();
        painter->fillRect(rect, QColor(QStringLiteral("#1F232A")));
        painter->setPen(QColor(QStringLiteral("#ADB5C1")));
        painter->setFont(font());
        const int right = logicalIndex == 1 ? trackDurationRightPadding : 6;
        const QRect textRect = rect.adjusted(6, 0, -right, 0);
        const auto alignment = logicalIndex == 1 ? Qt::AlignRight : Qt::AlignLeft;
        const QString title = model()->headerData(logicalIndex, Qt::Horizontal).toString();
        painter->drawText(textRect, alignment | Qt::AlignVCenter | Qt::TextSingleLine,
                          fontMetrics().elidedText(title, Qt::ElideRight, textRect.width()));
        painter->restore();
    }
    QSize sectionSizeFromContents(int logicalIndex) const override
    {
        QSize size = QHeaderView::sectionSizeFromContents(logicalIndex);
        if (logicalIndex == 1) {
            const QString title = model()->headerData(logicalIndex, Qt::Horizontal).toString();
            size.setWidth(std::max(size.width(), fontMetrics().horizontalAdvance(title)
                + 6 + trackDurationRightPadding));
        }
        return size;
    }
};

class LibraryTrackDelegate final : public QStyledItemDelegate
{
public:
    explicit LibraryTrackDelegate(LibraryTracksTableView *view)
        : QStyledItemDelegate(view), m_view(view), m_folderIcon(lineIcon(LibraryIcon::Folder)),
          m_audioIcon(lineIcon(LibraryIcon::Audio)),
          m_playingIcon(lineIcon(LibraryIcon::Audio, QColor(QStringLiteral("#7FC5B4")))) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (index.column() == 1) {
            QStyleOptionViewItem styled(option);
            initStyleOption(&styled, index);
            size.setWidth(std::max(size.width(), styled.fontMetrics.horizontalAdvance(styled.text)
                + 6 + trackDurationRightPadding));
        }
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool playing = index.data(LibraryPanel::PlayingRole).toBool();
        const QColor base(QStringLiteral("#1F232A"));
        const QColor background = selected ? QColor(QStringLiteral("#244B40"))
            : m_view->isHovered(index) ? QColor(QStringLiteral("#2C363B"))
            : playing ? QColor(QStringLiteral("#263E3D"))
            : index.row() % 2 ? QColor(QStringLiteral("#252A32")) : base;
        painter->save();
        painter->setClipRect(option.rect, Qt::IntersectClip);
        painter->fillRect(option.rect, base);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(background);
        // Every cell uses the same row geometry and only paints its own clip.
        // This produces one continuous rounded stripe across column boundaries.
        const QRect row(0, option.rect.top(), m_view->viewport()->width(), option.rect.height());
        painter->drawRoundedRect(QRectF(row.adjusted(0, 1, 0, -1)), 7, 7);
        painter->setFont(styled.font);
        QColor textColor(QStringLiteral("#F0F2F5"));
        const auto foreground = index.data(Qt::ForegroundRole);
        if (foreground.isValid()) textColor = qvariant_cast<QBrush>(foreground).color();
        if (selected) textColor = QColor(QStringLiteral("#F2FFF8"));
        painter->setPen(textColor);
        if (index.column() == 1) {
            painter->drawText(option.rect.adjusted(6, 0, -trackDurationRightPadding, 0),
                              Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, styled.text);
        } else if (index.column() == 0) {
            const bool batch = m_view->property("trackBatchMode").toBool();
            const auto checkState = index.data(Qt::CheckStateRole);
            const int checkOffset = batch ? 26 : 0;
            if (checkState.isValid()) {
                const QRectF check(option.rect.left() + 7, option.rect.center().y() - 8, 16, 16);
                const bool checked = checkState.toInt() == Qt::Checked;
                painter->setBrush(checked ? QColor(QStringLiteral("#45A88E")) : Qt::transparent);
                painter->setPen(QPen(checked ? QColor(QStringLiteral("#45A88E"))
                                             : QColor(QStringLiteral("#88939E")), 1.4));
                painter->drawRoundedRect(check, 4, 4);
                if (checked) {
                    painter->setPen(QPen(Qt::white, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    painter->drawPolyline(QPolygonF{check.topLeft() + QPointF(3, 8),
                        check.topLeft() + QPointF(6.5, 11), check.topLeft() + QPointF(13, 4.5)});
                }
                painter->setPen(textColor);
            }
            const QRect iconRect(option.rect.left() + 6 + checkOffset, option.rect.center().y() - 11, 22, 22);
            const bool directory = index.data(LibraryPanel::DirectoryRole).toBool();
            const QIcon &icon = directory ? m_folderIcon : playing ? m_playingIcon : m_audioIcon;
            icon.paint(painter, iconRect);
            const QRect textRect = option.rect.adjusted(36 + checkOffset, 0, -6, 0);
            const bool subtitle = index.data(LibraryTrackTreeModel::SubtitleAvailableRole).toBool();
            QFont badgeFont = styled.font;
            if (badgeFont.pixelSize() > 0) badgeFont.setPixelSize(std::max(10, badgeFont.pixelSize() - 2));
            else badgeFont.setPointSizeF(std::max(7.5, badgeFont.pointSizeF() - 1));
            const QFontMetrics badgeMetrics(badgeFont);
            const QString badgeText = QStringLiteral("字幕");
            const int badgeWidth = badgeMetrics.horizontalAdvance(badgeText) + 10;
            const int nameWidth = std::max(0, textRect.width() - (subtitle ? badgeWidth + 8 : 0));
            const QString displayed = styled.fontMetrics.elidedText(styled.text, Qt::ElideRight, nameWidth);
            painter->drawText(textRect.adjusted(0, 0, -(textRect.width() - nameWidth), 0),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, displayed);
            if (subtitle && textRect.width() >= badgeWidth) {
                const int x = textRect.left() + std::min(nameWidth,
                    styled.fontMetrics.horizontalAdvance(displayed)) + (displayed.isEmpty() ? 0 : 8);
                const int height = badgeMetrics.height() + 4;
                const QRectF badge(x, QRectF(option.rect).center().y() - height / 2.0,
                                   badgeWidth, height);
                painter->setBrush(QColor(QStringLiteral("#322D22")));
                painter->setPen(QPen(QColor(QStringLiteral("#8F743D")), 1));
                painter->drawRoundedRect(badge.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
                painter->setFont(badgeFont);
                painter->setPen(QColor(QStringLiteral("#D7AC57")));
                // Font metrics and hinted text baselines can leave asymmetric
                // visible padding. Cache these two glyph outlines by font and
                // center the actual contour, preserving fractional geometry.
                if (m_badgeTextPath.isEmpty() || m_badgeTextFont != badgeFont) {
                    m_badgeTextFont = badgeFont;
                    m_badgeTextPath = {};
                    m_badgeTextPath.addText(QPointF(0, 0), badgeFont, badgeText);
                }
                painter->translate(badge.center() - m_badgeTextPath.boundingRect().center());
                painter->fillPath(m_badgeTextPath, QColor(QStringLiteral("#D7AC57")));
            }
        } else {
            // The path column stays hidden in the application, but embedders
            // may expose it without losing its ordinary read-only rendering.
            const QRect textRect = option.rect.adjusted(6, 0, -6, 0);
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                styled.fontMetrics.elidedText(styled.text, Qt::ElideRight, textRect.width()));
        }
        painter->restore();
    }
private:
    LibraryTracksTableView *m_view;
    mutable QFont m_badgeTextFont;
    mutable QPainterPath m_badgeTextPath;
    QIcon m_folderIcon;
    QIcon m_audioIcon;
    QIcon m_playingIcon;
};

} // namespace

class LibraryWorksModel final : public QAbstractListModel
{
public:
    explicit LibraryWorksModel(QObject *parent)
        : QAbstractListModel(parent), m_placeholder(workCoverIcon()) {}

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_works.size());
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_works.size())
            return {};
        const auto &work = m_works.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            return work.displayName;
        case Qt::DecorationRole:
            return m_icons.value(work.id, m_placeholder);
        case Qt::ToolTipRole:
            return safeToolTip(work.displayName + QLatin1Char('\n') + work.rootPath);
        case Qt::SizeHintRole:
            return QSize(0, 52);
        case Qt::AccessibleTextRole:
            return work.displayName + QStringLiteral("，已收录，") + work.rootPath;
        case LibraryPanel::WorkIdRole:
            return work.id;
        case LibraryPanel::RootPathRole:
            return work.rootPath;
        default:
            return {};
        }
    }

    void setWorks(const QVector<LibraryWork> &works)
    {
        if (m_works == works)
            return;
        for (auto it = m_covers.begin(); it != m_covers.end();) {
            const auto oldRow = rowForId(it.key());
            const auto fresh = std::find_if(works.cbegin(), works.cend(),
                [id = it.key()](const auto &work) { return work.id == id; });
            if (oldRow < 0 || fresh == works.cend()
                || fresh->rootPath != m_works.at(oldRow).rootPath
                || fresh->importedAtMs != m_works.at(oldRow).importedAtMs) {
                m_icons.remove(it.key());
                it = m_covers.erase(it);
            } else ++it;
        }
        beginResetModel();
        m_works = works;
        endResetModel();
    }

    int rowForId(qint64 id) const
    {
        if (id <= 0)
            return -1;
        const auto it = std::find_if(m_works.cbegin(), m_works.cend(),
                                     [id](const auto &work) { return work.id == id; });
        return it == m_works.cend() ? -1 : static_cast<int>(it - m_works.cbegin());
    }

    void setCover(qint64 id, const CoverArtResult &result)
    {
        const int row = rowForId(id);
        if (row < 0) return;
        m_covers.insert(id, result);
        if (result.status == CoverArtStatus::Ready && !result.image.isNull())
            m_icons.insert(id, workCoverIcon(result.image));
        else m_icons.remove(id);
        emit dataChanged(index(row), index(row), {Qt::DecorationRole});
    }

    void clearCover(qint64 id)
    {
        const bool hadCover = m_covers.remove(id);
        const bool hadIcon = m_icons.remove(id);
        const int row = rowForId(id);
        if (row >= 0 && (hadCover || hadIcon))
            emit dataChanged(index(row), index(row), {Qt::DecorationRole});
    }

    void retainCovers(const QSet<qint64> &wanted)
    {
        const auto ids = m_covers.keys();
        for (const qint64 id : ids)
            if (!wanted.contains(id)) clearCover(id);
    }

    const CoverArtResult *cover(qint64 id) const
    {
        const auto it = m_covers.constFind(id);
        return it == m_covers.cend() ? nullptr : &it.value();
    }

    bool matchesIdentity(const LibraryWork &work) const
    {
        const int row = rowForId(work.id);
        return row >= 0 && m_works.at(row).rootPath == work.rootPath
            && m_works.at(row).importedAtMs == work.importedAtMs;
    }

    std::optional<LibraryWork> workAt(const QModelIndex &index) const
    {
        if (!index.isValid() || index.model() != this || index.row() < 0
            || index.row() >= m_works.size())
            return std::nullopt;
        return m_works.at(index.row());
    }

private:
    QVector<LibraryWork> m_works;
    QHash<qint64, CoverArtResult> m_covers;
    QHash<qint64, QIcon> m_icons;
    QIcon m_placeholder;
};

LibraryPanel::LibraryPanel(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumWidth(0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("librarySplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);

    auto *sidebar = new QWidget(splitter);
    m_sidebar = sidebar;
    sidebar->setObjectName(QStringLiteral("librarySidebar"));
    sidebar->setMinimumWidth(180);
    auto *brand = plainLabel(QStringLiteral("applicationTitle"), sidebar);
    brand->setText(applicationDisplayName());
    brand->setAlignment(Qt::AlignCenter);
    brand->setWordWrap(false);
    brand->setMinimumWidth(0);
    brand->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *worksTitle = plainLabel(QStringLiteral("libraryWorksTitle"), sidebar);
    worksTitle->setText(QStringLiteral("本地作品"));
    worksTitle->setProperty("subtleLabel", true);
    m_workCountLabel = plainLabel(QStringLiteral("libraryWorkCountLabel"), sidebar);
    m_workCountLabel->setProperty("subtleLabel", true);
    m_importMenuButton = new SidebarButton(QStringLiteral("导入作品"), sidebar);
    m_importMenuButton->setIcon(lineIcon(LibraryIcon::Add));
    m_importMenuButton->setIconSize(QSize(22, 22));
    m_importMenuButton->setObjectName(QStringLiteral("libraryImportMenuButton"));
    m_importMenuButton->setAccessibleName(QStringLiteral("导入作品菜单"));
    m_importMenuButton->setMinimumWidth(0);
    auto *importMenu = new QMenu(m_importMenuButton);
    importMenu->setObjectName(QStringLiteral("libraryImportMenu"));
    m_importMenuButton->setMenu(importMenu);
    const auto addAction = [](QMenu *menu, const QString &text, const QString &name) {
        auto *action = menu->addAction(text);
        action->setObjectName(name);
        return action;
    };
    m_importAction = addAction(importMenu, QStringLiteral("导入单个作品…"), QStringLiteral("libraryImportAction"));
    m_addScanRootAction = addAction(importMenu, QStringLiteral("添加扫描根目录…"), QStringLiteral("libraryAddScanRootAction"));
    m_addScanRootAction->setToolTip(QStringLiteral("打开扫描目录管理，添加持续扫描的总目录。"));
    m_aiSubtitlesButton = new SidebarButton(QStringLiteral("AI 字幕"), sidebar);
    m_aiSubtitlesButton->setObjectName(QStringLiteral("libraryAiSubtitlesButton"));
    m_aiSubtitlesButton->setIcon(lineIcon(LibraryIcon::Ai));
    m_aiSubtitlesButton->setIconSize(QSize(22, 22));
    m_aiSubtitlesButton->setAccessibleName(QStringLiteral("AI 字幕"));
    connect(m_aiSubtitlesButton, &QPushButton::clicked, this, [this] {
        if (!m_closing) emit aiSubtitlesRequested();
    });
    m_settingsButton = new SidebarButton(QStringLiteral("设置"), sidebar);
    m_settingsButton->setIcon(lineIcon(LibraryIcon::Settings));
    m_settingsButton->setIconSize(QSize(22, 22));
    m_settingsButton->setObjectName(QStringLiteral("librarySettingsButton"));
    m_settingsButton->setAccessibleName(QStringLiteral("设置菜单"));
    m_settingsMenu = new QMenu(m_settingsButton);
    m_settingsMenu->setObjectName(QStringLiteral("librarySettingsMenu"));
    m_settingsButton->setMenu(m_settingsMenu);
    m_manageScanRootsAction = addAction(m_settingsMenu, QStringLiteral("扫描目录管理…"), QStringLiteral("libraryManageScanRootsAction"));
    m_manageScanRootsAction->setToolTip(QStringLiteral("管理持续扫描的总目录、启动扫描设置和本次刷新结果。"));
    m_refreshScanRootsButton = new QPushButton(sidebar);
    m_refreshScanRootsButton->setIcon(lineIcon(LibraryIcon::Refresh));
    m_refreshScanRootsButton->setIconSize(QSize(20, 20));
    m_refreshScanRootsButton->setObjectName(QStringLiteral("libraryRefreshScanRootsButton"));
    m_refreshScanRootsButton->setMinimumWidth(0);
    m_refreshScanRootsButton->setAccessibleName(QStringLiteral("刷新全部扫描根目录"));
    m_refreshScanRootsButton->setFixedWidth(34);
    m_refreshScanRootsButton->setProperty("sidebarRefresh", true);
    m_refreshScanRootsButton->setToolTip(QStringLiteral("后台刷新所有已保存的扫描根目录，发现新增作品并清理已删除作品的库记录。"));
    m_cancelButton = new QPushButton(QStringLiteral("取消扫描"), sidebar);
    m_cancelButton->setObjectName(QStringLiteral("libraryCancelButton"));

    m_worksModel = new LibraryWorksModel(this);
    m_worksView = new QListView(sidebar);
    m_worksView->setObjectName(QStringLiteral("libraryWorksView"));
    m_worksView->setAccessibleName(QStringLiteral("本地作品列表"));
    m_worksView->setModel(m_worksModel);
    m_worksView->setItemDelegate(new LibraryItemDelegate(true, m_worksView));
    m_worksView->setFrameShape(QFrame::NoFrame);
    m_worksView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_worksView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_worksView->setTextElideMode(Qt::ElideRight);
    m_worksView->setUniformItemSizes(true);
    m_worksView->setSpacing(3);
    m_worksView->setMinimumSize(0, 60);
    m_worksView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_worksView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_workContextMenu = new QMenu(m_worksView);
    m_workContextMenu->setObjectName(QStringLiteral("libraryWorkContextMenu"));
    m_workReferencesAction = m_workContextMenu->addAction(QStringLiteral("作品参考资料…"));
    m_workReferencesAction->setObjectName(QStringLiteral("libraryWorkReferencesAction"));
    m_workReferencesAction->setEnabled(false);
    connect(m_workReferencesAction, &QAction::triggered, this, [this] {
        const auto context = m_workMenuContext;
        if (!canRequestWorkRemoval() || context.revision != m_workMenuRevision
            || context.work.id <= 0 || !m_worksModel->matchesIdentity(context.work)) return;
        invalidateWorkContextMenu();
        emit workReferencesRequested(context.work.id);
    });
    m_removeWorkAction = m_workContextMenu->addAction(QStringLiteral("删除作品…"));
    m_removeWorkAction->setObjectName(QStringLiteral("libraryRemoveWorkAction"));
    m_removeWorkAction->setEnabled(false);
    connect(m_removeWorkAction, &QAction::triggered, this, [this] {
        const auto context = m_workMenuContext;
        if (!canRequestWorkRemoval() || context.revision != m_workMenuRevision
            || context.work.id <= 0 || !m_worksModel->matchesIdentity(context.work))
            return;
        // Consume the captured target before notifying the owner, which may
        // synchronously open a confirmation dialog or update the snapshot.
        invalidateWorkContextMenu();
        emit workRemovalRequested(context.work.id);
    });
    m_coverDemandTimer = new QTimer(this);
    m_coverDemandTimer->setSingleShot(true);
    connect(m_coverDemandTimer, &QTimer::timeout, this, &LibraryPanel::publishWorkCoverDemand);
    connect(m_worksView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { scheduleWorkCoverDemand(); });
    connect(m_worksView->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { scheduleWorkCoverDemand(); });
    m_worksEmptyLabel = plainLabel(QStringLiteral("libraryWorksEmptyLabel"), sidebar);
    m_worksEmptyLabel->setProperty("subtleLabel", true);
    m_worksEmptyLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_worksEmptyLabel->setContentsMargins(6, 48, 6, 0);
    m_worksStack = new QStackedWidget(sidebar);
    m_worksStack->addWidget(m_worksEmptyLabel);
    m_worksStack->addWidget(m_worksView);

    m_statusText = QStringLiteral("尚未导入作品目录");
    m_queryLabel = plainLabel(QStringLiteral("libraryQueryStateLabel"), sidebar);
    m_errorLabel = plainLabel(QStringLiteral("libraryErrorLabel"), sidebar);
    m_errorLabel->setProperty("errorLabel", true);
    for (auto *label : {m_queryLabel, m_errorLabel}) {
        // Keep long task paths accessible without growing the permanent footer.
        label->setWordWrap(false);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        label->setProperty("subtleLabel", true);
    }
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(16, 4, 16, 16);
    sidebarLayout->setSpacing(12);
    sidebarLayout->addWidget(brand);
    sidebarLayout->addWidget(m_importMenuButton);
    auto *worksHeading = new QHBoxLayout;
    worksHeading->addWidget(worksTitle, 1);
    worksHeading->addWidget(m_refreshScanRootsButton);
    sidebarLayout->addLayout(worksHeading);
    sidebarLayout->addWidget(m_workCountLabel);
    sidebarLayout->addWidget(m_worksStack, 1);
    sidebarLayout->addWidget(m_queryLabel);
    sidebarLayout->addWidget(m_errorLabel);
    auto *taskActions = new QBoxLayout(QBoxLayout::TopToBottom);
    taskActions->setObjectName(QStringLiteral("libraryTaskActionsLayout"));
    taskActions->addWidget(m_cancelButton);
    sidebarLayout->addLayout(taskActions);
    sidebarLayout->addWidget(m_aiSubtitlesButton);
    sidebarLayout->addWidget(m_settingsButton);

    auto *contentWorkspace = new QWidget(splitter);
    contentWorkspace->setObjectName(QStringLiteral("contentWorkspace"));
    contentWorkspace->setMinimumWidth(220);
    m_workspaceLayout = new QVBoxLayout(contentWorkspace);
    m_workspaceLayout->setContentsMargins(0, 0, 0, 0);
    m_workspaceLayout->setSpacing(12);
    m_workspaceSplitter = new QSplitter(Qt::Horizontal, contentWorkspace);
    m_workspaceSplitter->setObjectName(QStringLiteral("workspaceSplitter"));
    m_workspaceSplitter->setChildrenCollapsible(false);
    m_workspaceSplitter->setHandleWidth(6);
    m_workspaceLayout->addWidget(m_workspaceSplitter, 1);
    auto *tracksPane = new QWidget(m_workspaceSplitter);
    tracksPane->setObjectName(QStringLiteral("libraryTracksPane"));
    tracksPane->setMinimumWidth(220);
    m_workInformation = new QWidget(tracksPane);
    m_workInformation->setObjectName(QStringLiteral("libraryWorkInformation"));
    m_workInformation->setMinimumWidth(0);
    m_workCover = new CoverArtWidget(m_workInformation);
    m_workCover->setObjectName(QStringLiteral("libraryWorkCoverWidget"));
    m_workCover->setContextName(QStringLiteral("当前浏览作品封面"));
    m_workCover->setFixedSize(144, 144);
    m_browseTitleLabel = new WorkTitleLabel(m_workInformation);
    m_browseTitleLabel->setObjectName(QStringLiteral("libraryBrowseTitleLabel"));
    m_browseTitleLabel->setProperty("sectionTitle", true);
    m_navigationBar = new QWidget(tracksPane);
    m_navigationBar->setObjectName(QStringLiteral("libraryDirectoryNavigationBar"));
    m_navigationBar->setMinimumWidth(0);
    m_navigateUpButton = new QPushButton(QStringLiteral("上一级"), m_navigationBar);
    m_navigateUpButton->setObjectName(QStringLiteral("libraryNavigateUpButton"));
    m_navigateUpButton->setAccessibleName(QStringLiteral("上一级"));
    m_navigateUpButton->setToolTip(QStringLiteral("返回作品内上一级目录；列表获得焦点时可按 Backspace 或 Alt+↑。"));
    m_breadcrumbBar = new QWidget(m_navigationBar);
    m_breadcrumbBar->setObjectName(QStringLiteral("libraryBreadcrumbBar"));
    m_breadcrumbBar->setMinimumWidth(0);
    m_breadcrumbBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbBar);
    m_breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    m_breadcrumbLayout->setSpacing(4);
    m_breadcrumbLayout->setSizeConstraint(QLayout::SetNoConstraint);
    m_breadcrumbRootButton = new QToolButton(m_breadcrumbBar);
    m_breadcrumbRootButton->setObjectName(QStringLiteral("libraryBreadcrumbRootButton"));
    m_breadcrumbRootButton->setProperty("relativePath", QString());
    m_breadcrumbRootButton->setText(QStringLiteral("作品根"));
    m_breadcrumbRootButton->setAutoRaise(true);
    m_breadcrumbRootButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_breadcrumbLayout->addWidget(m_breadcrumbRootButton);
    m_breadcrumbOverflowSeparator = new QLabel(QStringLiteral("›"), m_breadcrumbBar);
    m_breadcrumbOverflowSeparator->setTextFormat(Qt::PlainText);
    m_breadcrumbOverflowSeparator->hide();
    m_breadcrumbLayout->addWidget(m_breadcrumbOverflowSeparator);
    m_breadcrumbOverflowButton = new QToolButton(m_breadcrumbBar);
    m_breadcrumbOverflowButton->setObjectName(QStringLiteral("libraryBreadcrumbOverflowButton"));
    m_breadcrumbOverflowButton->setText(QStringLiteral("…"));
    m_breadcrumbOverflowButton->setAccessibleName(QStringLiteral("显示省略的上级目录"));
    m_breadcrumbOverflowButton->setToolTip(QStringLiteral("选择省略的上级目录"));
    m_breadcrumbOverflowButton->setAutoRaise(true);
    m_breadcrumbOverflowButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_breadcrumbOverflowButton->hide();
    m_breadcrumbLayout->addWidget(m_breadcrumbOverflowButton);
    m_breadcrumbOverflowMenu = new QMenu(m_breadcrumbOverflowButton);
    m_breadcrumbOverflowMenu->setObjectName(QStringLiteral("libraryBreadcrumbOverflowMenu"));
    m_breadcrumbOverflowMenu->setToolTipsVisible(true);
    connect(m_breadcrumbOverflowButton, &QToolButton::clicked, this, [this] {
        // A second keyboard gesture can arrive before the coalesced layout of
        // the new page. Never open the previous page's (or an empty) menu.
        updateBreadcrumbLayout();
        if (m_breadcrumbOverflowButton->isVisible() && !m_breadcrumbOverflowMenu->isEmpty())
            m_breadcrumbOverflowMenu->popup(m_breadcrumbOverflowButton->mapToGlobal(
                QPoint(0, m_breadcrumbOverflowButton->height())));
    });
    m_breadcrumbLayout->addStretch();
    m_breadcrumbBar->setAutoFillBackground(false);
    m_breadcrumbBar->setFixedHeight(m_breadcrumbRootButton->sizeHint().height());
    auto *navigationLayout = new QHBoxLayout(m_navigationBar);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    navigationLayout->setSpacing(8);
    navigationLayout->addWidget(m_navigateUpButton, 0, Qt::AlignTop);
    navigationLayout->addWidget(m_breadcrumbBar, 1);

    m_tracksModel = new LibraryTrackTreeModel(this);
    auto *tracksView = new LibraryTracksTableView(tracksPane);
    m_tracksView = tracksView;
    m_tracksView->setObjectName(QStringLiteral("libraryTracksView"));
    m_tracksView->setAccessibleName(QStringLiteral("所选作品当前目录的文件夹和音轨"));
    m_tracksView->setModel(m_tracksModel);
    m_tracksView->setItemDelegate(new LibraryTrackDelegate(tracksView));
    auto *durationHeader = new LibraryTracksHeader(m_tracksView);
    durationHeader->setObjectName(QStringLiteral("libraryTracksHeader"));
    m_tracksView->setHorizontalHeader(durationHeader);
    m_tracksView->setFrameShape(QFrame::NoFrame);
    m_tracksView->setShowGrid(false);
    m_tracksView->verticalHeader()->hide();
    m_tracksView->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tracksView->verticalHeader()->setDefaultSectionSize(48);
    m_tracksView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tracksView->setTabKeyNavigation(false);
    m_tracksView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tracksView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tracksView->setTextElideMode(Qt::ElideRight);
    m_tracksView->setWordWrap(false);
    m_tracksView->setSortingEnabled(false);
    m_tracksView->setMinimumSize(0, 60);
    m_tracksView->horizontalHeader()->setMinimumSectionSize(48);
    m_tracksView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_tracksView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tracksView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tracksView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_tracksView->setColumnWidth(2, 180);
    m_tracksView->setColumnHidden(2, true);
    m_durationDemandTimer = new QTimer(this);
    m_durationDemandTimer->setSingleShot(true);
    connect(m_durationDemandTimer, &QTimer::timeout, this, &LibraryPanel::publishTrackDurationDemand);
    for (auto *scroll : {m_tracksView->verticalScrollBar(), m_tracksView->horizontalScrollBar()}) {
        connect(scroll, &QScrollBar::valueChanged, this, [this] { scheduleTrackDurationDemand(); });
        connect(scroll, &QScrollBar::rangeChanged, this, [this] { scheduleTrackDurationDemand(); });
    }
    m_tracksView->setToolTip(QStringLiteral("目录双击或按 Enter 进入；音轨双击或按 Enter 播放；点击“批量”可勾选多首音轨。完整路径可悬停查看。"));
    m_trackContextMenu = new QMenu(m_tracksView);
    m_trackContextMenu->setObjectName(QStringLiteral("libraryTrackContextMenu"));
    m_enqueueTrackAction = m_trackContextMenu->addAction(QStringLiteral("加入播放队列"));
    m_enqueueTrackAction->setObjectName(QStringLiteral("libraryEnqueueTrackAction"));
    m_offlineTranslateTracksAction = m_trackContextMenu->addAction(QStringLiteral("离线直译…"));
    m_offlineTranslateTracksAction->setObjectName(QStringLiteral("libraryOfflineTranslateTracksAction"));
    m_japaneseAsrTracksAction = m_trackContextMenu->addAction(QStringLiteral("日语识别…"));
    m_japaneseAsrTracksAction->setObjectName(QStringLiteral("libraryJapaneseAsrTracksAction"));
    m_batchButton = new QPushButton(QStringLiteral("批量"), m_workInformation);
    m_batchButton->setObjectName(QStringLiteral("libraryBatchButton"));
    m_batchButton->setMinimumWidth(0);
    m_batchButton->setToolTip(QStringLiteral("勾选当前目录的多首音轨，添加到播放队列、离线直译或日语识别。"));
    m_playDirectoryButton = new QPushButton(QStringLiteral("播放此目录"), m_workInformation);
    m_playDirectoryButton->setObjectName(QStringLiteral("libraryPlayDirectoryButton"));
    m_playDirectoryButton->setProperty("primary", true);
    m_playDirectoryButton->setMinimumWidth(0);
    m_playDirectoryButton->setToolTip(QStringLiteral("按自然排序播放当前目录本层的可用音轨，不包含子目录。"));
    auto *directoryActions = new QHBoxLayout;
    directoryActions->setObjectName(QStringLiteral("libraryDirectoryActionsLayout"));
    directoryActions->setSpacing(6);
    directoryActions->addWidget(m_playDirectoryButton, 0, Qt::AlignBottom);
    directoryActions->addWidget(m_batchButton, 0, Qt::AlignBottom);
    directoryActions->addStretch();

    auto *welcomePage = new QWidget(tracksPane);
    m_welcomePage = welcomePage;
    welcomePage->setObjectName(QStringLiteral("libraryWelcomePage"));
    // QStackedLayout includes hidden pages in its minimum size. The welcome
    // group must not impose its icon/button height on the directory browser,
    // otherwise the pane cannot become short enough to enter compact mode.
    welcomePage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    m_welcomeContent = new QWidget(welcomePage);
    m_welcomeContent->setObjectName(QStringLiteral("libraryWelcomeContent"));
    m_welcomeIconLabel = new QLabel(m_welcomeContent);
    m_welcomeIconLabel->setObjectName(QStringLiteral("libraryWelcomeIcon"));
    m_welcomeIconLabel->setAlignment(Qt::AlignCenter);
    m_welcomeIconLabel->setAccessibleName(applicationDisplayName());
    m_welcomeIconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_welcomeTitleLabel = plainLabel(QStringLiteral("libraryWelcomeTitle"), m_welcomeContent);
    m_welcomeTitleLabel->setProperty("welcomeTitle", true);
    m_welcomeTitleLabel->setAlignment(Qt::AlignCenter);
    // Loading/empty-work messages use the page width, independently of the
    // fixed-width welcome actions and their changing visibility/size hints.
    m_tracksEmptyLabel = plainLabel(QStringLiteral("libraryTracksEmptyLabel"), welcomePage);
    m_tracksEmptyLabel->setProperty("subtleLabel", true);
    m_tracksEmptyLabel->setAlignment(Qt::AlignCenter);
    m_welcomeImportButton = new QPushButton(QStringLiteral("导入单个作品"), m_welcomeContent);
    m_welcomeImportButton->setObjectName(QStringLiteral("libraryWelcomeImportButton"));
    m_welcomeImportButton->setProperty("primary", true);
    m_welcomeAddScanRootButton = new QPushButton(QStringLiteral("增加扫描根路径"), m_welcomeContent);
    m_welcomeAddScanRootButton->setObjectName(QStringLiteral("libraryWelcomeAddScanRootButton"));
    auto *welcomeContentLayout = new QVBoxLayout(m_welcomeContent);
    welcomeContentLayout->setContentsMargins(0, 0, 0, 0);
    welcomeContentLayout->setSpacing(12);
    welcomeContentLayout->addWidget(m_welcomeIconLabel, 0, Qt::AlignHCenter);
    welcomeContentLayout->addWidget(m_welcomeTitleLabel);
    for (auto *button : {m_welcomeImportButton, m_welcomeAddScanRootButton}) {
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        welcomeContentLayout->addWidget(button);
    }
    auto *welcomeLayout = new QVBoxLayout(welcomePage);
    welcomeLayout->setContentsMargins(12, 12, 12, 12);
    welcomeLayout->setSpacing(10);
    welcomeLayout->addStretch();
    welcomeLayout->addWidget(m_welcomeContent, 0, Qt::AlignHCenter);
    welcomeLayout->addWidget(m_tracksEmptyLabel);
    welcomeLayout->addStretch();
    m_tracksStack = new QStackedWidget(tracksPane);
    m_tracksStack->addWidget(welcomePage);
    m_tracksStack->addWidget(m_tracksView);
    m_coverMenuButton = new QToolButton(m_workInformation);
    m_coverMenuButton->setObjectName(QStringLiteral("libraryCoverMenuButton"));
    m_coverMenuButton->setText(QStringLiteral("编辑"));
    m_coverMenuButton->setIcon(lineIcon(LibraryIcon::Edit, QColor(QStringLiteral("#858A91"))));
    m_coverMenuButton->setIconSize(QSize(18, 18));
    m_coverMenuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_coverMenuButton->setAccessibleName(QStringLiteral("编辑当前浏览作品"));
    m_coverMenuButton->setToolTip(QStringLiteral("编辑当前浏览作品的标题和封面。"));
    auto *tracksHeader = new QGridLayout(m_workInformation);
    tracksHeader->setObjectName(QStringLiteral("libraryWorkDetailsLayout"));
    tracksHeader->setContentsMargins(0, 0, 0, 0);
    tracksHeader->setHorizontalSpacing(20);
    tracksHeader->setVerticalSpacing(8);
    tracksHeader->addWidget(m_workCover, 0, 0, 2, 1, Qt::AlignTop);
    tracksHeader->addWidget(m_browseTitleLabel, 0, 1, Qt::AlignTop);
    tracksHeader->addWidget(m_coverMenuButton, 0, 2, Qt::AlignTop | Qt::AlignRight);
    tracksHeader->addLayout(directoryActions, 1, 1, 1, 2, Qt::AlignBottom);
    tracksHeader->setColumnStretch(1, 1);
    tracksHeader->setRowStretch(1, 1);
    auto *tracksLayout = new QVBoxLayout(tracksPane);
    tracksLayout->setContentsMargins(20, 20, 20, 16);
    tracksLayout->setSpacing(8);
    tracksLayout->addWidget(m_workInformation);
    tracksLayout->addWidget(m_navigationBar);
    m_batchToolbar = new QWidget(tracksPane);
    m_batchToolbar->setObjectName(QStringLiteral("libraryBatchToolbar"));
    m_batchSelectAll = new QCheckBox(QStringLiteral("全选"), m_batchToolbar);
    m_batchSelectAll->setObjectName(QStringLiteral("libraryBatchSelectAll"));
    m_batchSelectAll->setToolTip(QStringLiteral("仅选择当前目录本层的可用音轨，不包含子目录。"));
    m_batchSelectionLabel = new QLabel(m_batchToolbar);
    m_batchSelectionLabel->setObjectName(QStringLiteral("libraryBatchSelectionLabel"));
    m_batchSelectionLabel->setTextFormat(Qt::PlainText);
    m_batchAddButton = new QPushButton(QStringLiteral("添加"), m_batchToolbar);
    m_batchAddButton->setObjectName(QStringLiteral("libraryBatchAddButton"));
    m_batchAddMenu = new QMenu(m_batchAddButton);
    m_batchAddMenu->setObjectName(QStringLiteral("libraryBatchAddMenu"));
    m_batchEnqueueAction = m_batchAddMenu->addAction(QStringLiteral("添加到播放队列"));
    m_batchEnqueueAction->setObjectName(QStringLiteral("libraryBatchEnqueueAction"));
    m_batchOfflineTranslateAction = m_batchAddMenu->addAction(QStringLiteral("离线直译"));
    m_batchOfflineTranslateAction->setObjectName(QStringLiteral("libraryBatchOfflineTranslateAction"));
    m_batchJapaneseAsrAction = m_batchAddMenu->addAction(QStringLiteral("日语识别"));
    m_batchJapaneseAsrAction->setObjectName(QStringLiteral("libraryBatchJapaneseAsrAction"));
    m_batchExitButton = new QPushButton(QStringLiteral("退出批量"), m_batchToolbar);
    m_batchExitButton->setObjectName(QStringLiteral("libraryBatchExitButton"));
    auto *batchLayout = new QHBoxLayout(m_batchToolbar);
    batchLayout->setContentsMargins(0, 0, 0, 0);
    batchLayout->setSpacing(8);
    batchLayout->addWidget(m_batchSelectAll);
    batchLayout->addWidget(m_batchSelectionLabel);
    batchLayout->addWidget(m_batchAddButton);
    batchLayout->addStretch();
    batchLayout->addWidget(m_batchExitButton);
    m_batchToolbar->hide();
    tracksLayout->addWidget(m_batchToolbar);
    tracksLayout->addWidget(m_tracksStack, 1);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({240, 720});
    m_workspaceSplitter->setStretchFactor(0, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(splitter, 1);

    setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(m_importMenuButton);
    setTabOrder(m_importMenuButton, m_refreshScanRootsButton);
    setTabOrder(m_refreshScanRootsButton, m_cancelButton);
    setTabOrder(m_cancelButton, m_worksView);
    setTabOrder(m_worksView, m_navigateUpButton);
    setTabOrder(m_navigateUpButton, m_breadcrumbRootButton);
    setTabOrder(m_breadcrumbRootButton, m_tracksView);
    setTabOrder(m_tracksView, m_aiSubtitlesButton);
    setTabOrder(m_aiSubtitlesButton, m_settingsButton);
    setTabOrder(m_worksView, m_playDirectoryButton);
    setTabOrder(m_playDirectoryButton, m_navigateUpButton);
    setTabOrder(m_worksView, m_coverMenuButton);
    setTabOrder(m_coverMenuButton, m_playDirectoryButton);
    connect(m_coverMenuButton, &QToolButton::clicked, this, [this] {
        if (canRequestQueuePlayback())
            emit editWorkRequested(m_displayedWorkId);
    });
    connect(m_playDirectoryButton, &QPushButton::clicked, this, [this] {
        if (canRequestQueuePlayback() && hasAvailableDirectoryAudio())
            emit directoryPlayRequested(m_displayedWorkId, m_currentDirectory, false);
    });
    connect(m_enqueueTrackAction, &QAction::triggered, this, [this] {
        const auto context = m_trackMenuContext;
        if (context.revision != m_browseRevision || context.workId != m_displayedWorkId
            || context.directory != m_currentDirectory || !canRequestQueuePlayback())
            return;
        const auto index = m_tracksModel->indexForFileId(context.fileId);
        if (isCurrentPageIndex(index) && !index.data(MissingRole).toBool()
            && index.data(RelativePathRole).toString() == context.relativePath)
            emit trackEnqueueRequested(context.fileId);
    });
    connect(m_tracksView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { updateTranslationActions(); });
    connect(m_batchButton, &QPushButton::clicked, this, [this] { setTrackBatchMode(true); });
    connect(m_batchExitButton, &QPushButton::clicked, this, [this] { setTrackBatchMode(false); });
    connect(m_batchSelectAll, &QCheckBox::clicked, this, [this] {
        if (!m_trackBatchMode || !canRequestQueuePlayback()) return;
        const int state = m_batchSelectAll->checkState() == Qt::Unchecked ? Qt::Unchecked : Qt::Checked;
        const auto root = m_tracksView->rootIndex();
        // Coalesce the toolbar update instead of rebuilding it for every row.
        const int rows = m_tracksModel->rowCount(root);
        {
            const QSignalBlocker blocker(m_tracksModel);
            for (int row = 0; row < rows; ++row)
                m_tracksModel->setData(m_tracksModel->index(row, 0, root), state, Qt::CheckStateRole);
        }
        // One model notification keeps observers and accessibility in sync.
        if (rows > 0)
            m_tracksModel->dataChanged(m_tracksModel->index(0, 0, root),
                m_tracksModel->index(rows - 1, 0, root), {Qt::CheckStateRole});
        else updateTrackBatchActions();
    });
    connect(m_batchAddButton, &QPushButton::clicked, this, [this] {
        if (!m_trackBatchMode || !canRequestQueuePlayback()) return;
        m_batchMenuIds = checkedTrackFileIds();
        if (m_batchMenuIds.isEmpty()) return;
        m_batchMenuRevision = m_browseRevision;
        m_batchMenuWorkId = m_displayedWorkId;
        m_batchMenuDirectory = m_currentDirectory;
        m_batchAddMenu->popup(m_batchAddButton->mapToGlobal(QPoint(0, m_batchAddButton->height())));
    });
    connect(m_batchEnqueueAction, &QAction::triggered, this, [this] { triggerTrackBatchAction({}); });
    connect(m_batchOfflineTranslateAction, &QAction::triggered, this,
            [this] { triggerTrackBatchAction(QStringLiteral("offline-direct")); });
    connect(m_batchJapaneseAsrAction, &QAction::triggered, this,
            [this] { triggerTrackBatchAction(QStringLiteral("offline-asr-ja")); });
    connect(m_tracksModel, &QAbstractItemModel::modelAboutToBeReset, this,
            [this] { setTrackBatchMode(false); });
    connect(m_tracksModel, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
        if (m_trackBatchMode && (roles.isEmpty() || roles.contains(Qt::CheckStateRole)))
            updateTrackBatchActions();
    });
    connect(m_offlineTranslateTracksAction, &QAction::triggered, this,
            [this] { triggerTrackSubtitleAction(QStringLiteral("offline-direct")); });
    connect(m_japaneseAsrTracksAction, &QAction::triggered, this,
            [this] { triggerTrackSubtitleAction(QStringLiteral("offline-asr-ja")); });
    connect(m_importAction, &QAction::triggered,
            this, &LibraryPanel::importDirectoryRequested);
    connect(m_welcomeImportButton, &QPushButton::clicked,
            this, &LibraryPanel::importDirectoryRequested);
    connect(m_welcomeAddScanRootButton, &QPushButton::clicked,
            m_addScanRootAction, &QAction::trigger);
    connect(m_addScanRootAction, &QAction::changed, this, [this] {
        m_welcomeAddScanRootButton->setEnabled(m_addScanRootAction->isEnabled());
        m_welcomeAddScanRootButton->setToolTip(m_addScanRootAction->toolTip());
    });
    connect(m_manageScanRootsAction, &QAction::triggered,
            this, &LibraryPanel::manageScanRootsRequested);
    connect(m_addScanRootAction, &QAction::triggered,
            this, &LibraryPanel::manageScanRootsRequested);
    connect(m_refreshScanRootsButton, &QPushButton::clicked,
            this, &LibraryPanel::refreshScanRootsRequested);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &LibraryPanel::cancelImportRequested);
    connect(m_navigateUpButton, &QPushButton::clicked, this, &LibraryPanel::navigateUp);
    connect(m_breadcrumbRootButton, &QToolButton::clicked, this, [this] {
        navigateToDirectory({});
    });
    connect(m_worksView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &current) {
        const qint64 workId = current.data(WorkIdRole).toLongLong();
        if (m_selectedWorkId == workId)
            return;
        rememberBrowseState();
        setTrackBatchMode(false);
        ++m_browseRevision;
        m_selectedWorkId = workId;
        // Do not briefly present the preceding work's files under a new selection.
        m_displayedWorkId = 0;
        m_currentDirectory.clear();
        m_tracksView->setRootIndex({});
        m_tracksModel->setFiles(0, {});
        m_browseTitleLabel->setText(current.data(Qt::DisplayRole).toString());
        m_browseTitleLabel->setToolTip(current.data(Qt::ToolTipRole).toString());
        updateBrowsedCover();
        rebuildBreadcrumbs();
        updatePresentation();
        scheduleWorkCoverDemand();
        emit workSelected(workId);
    });
    // activated can follow a platform style that activates on a single click.
    // Explicit gestures keep selection separate from playback on every style.
    connect(m_tracksView, &QTableView::doubleClicked, this, &LibraryPanel::activateItem);
    // Installing filters after every control exists prevents layout/style
    // events during construction from observing a partially built workspace.
    m_worksView->installEventFilter(this);
    m_worksView->viewport()->installEventFilter(this);
    tracksPane->installEventFilter(this);
    m_breadcrumbBar->installEventFilter(this);
    m_breadcrumbRootButton->installEventFilter(this);
    m_breadcrumbOverflowButton->installEventFilter(this);
    m_tracksView->installEventFilter(this);
    m_tracksView->viewport()->installEventFilter(this);
    welcomePage->installEventFilter(this);
    sidebar->installEventFilter(this);
    installEventFilter(this);
    updatePresentation();
}

bool LibraryPanel::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_welcomePage
            && (event->type() == QEvent::Resize || event->type() == QEvent::Show
                || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange))
        || (watched == this
            && (event->type() == QEvent::Show || event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
                || event->type() == QEvent::DevicePixelRatioChange
#endif
            )))
        updateWelcomeGeometry();
    if (event->type() == QEvent::KeyPress) {
        auto *button = qobject_cast<QToolButton *>(watched);
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (button && button->parentWidget() == m_breadcrumbBar
            && (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            && keyEvent->modifiers() == Qt::NoModifier) {
            if (button->isEnabled() && !keyEvent->isAutoRepeat()) button->click();
            return true;
        }
    }
    if ((watched == this || (m_tracksView
            && (watched == m_tracksView || watched == m_tracksView->viewport())))
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide
            || event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        scheduleTrackDurationDemand();
    if ((watched == this || (m_worksView
            && (watched == m_worksView || watched == m_worksView->viewport())))
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide
            || event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        scheduleWorkCoverDemand();
    if (watched->objectName() == QStringLiteral("libraryTracksPane")
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange))
        updateWorkHeaderGeometry();
    if (watched->objectName() == QStringLiteral("librarySidebar")
        && event->type() == QEvent::Resize) {
        auto *sidebar = qobject_cast<QWidget *>(watched);
        const bool compact = sidebar && sidebar->height() < 560;
        const bool tight = sidebar && sidebar->height() < 360;
        if (sidebar && (!sidebar->property("compactLibrarySidebar").isValid()
            || sidebar->property("compactLibrarySidebar").toBool() != compact
            || sidebar->property("tightLibrarySidebar").toBool() != tight)) {
            // Set the guard before changing geometry: stylesheet/layout events
            // may synchronously request another sidebar layout.
            sidebar->setProperty("compactLibrarySidebar", compact);
            sidebar->setProperty("tightLibrarySidebar", tight);
            auto *sidebarLayout = qobject_cast<QVBoxLayout *>(sidebar->layout());
            sidebarLayout->setContentsMargins(tight ? QMargins(10, 4, 10, 6)
                                                      : compact ? QMargins(10, 4, 10, 10)
                                                      : QMargins(16, 4, 16, 16));
            sidebarLayout->setSpacing(tight ? 2 : compact ? 4 : 12);
            m_worksEmptyLabel->setContentsMargins(6, tight ? 16 : compact ? 32 : 48, 6, 0);
            sidebar->findChild<QLabel *>(QStringLiteral("applicationTitle"))->setStyleSheet(
                compact ? QStringLiteral("font-size: 18px;") : QString{});
            if (auto *actions = sidebar->findChild<QBoxLayout *>("libraryTaskActionsLayout")) {
                actions->setDirection(compact ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
                actions->setSpacing(compact ? 4 : 12);
            }
            for (auto *button : {m_importMenuButton, m_aiSubtitlesButton, m_settingsButton,
                                 m_refreshScanRootsButton, m_cancelButton}) {
                const bool wasCompact = button->property("libraryRegularStyleSheet").isValid();
                if (compact && !wasCompact) {
                    button->setProperty("libraryRegularStyleSheet", button->styleSheet());
                    button->setStyleSheet(button->styleSheet()
                        + QStringLiteral("\nQPushButton { padding: 2px 8px; min-height: 20px; }"));
                } else if (!compact && wasCompact) {
                    button->setStyleSheet(button->property("libraryRegularStyleSheet").toString());
                    button->setProperty("libraryRegularStyleSheet", QVariant());
                }
            }
            updatePresentation();
        }
    }
    if (watched == m_breadcrumbBar && event->type() == QEvent::Resize)
        m_breadcrumbOverflowMenu->close();
    if (watched == m_breadcrumbBar
        && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest
            || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
        scheduleBreadcrumbVisibility();
    if (watched == m_worksView->viewport()
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)
        && static_cast<QMouseEvent *>(event)->button() == Qt::RightButton) {
        // A context gesture targets the pointed work without changing the
        // independently browsed work or starting its asynchronous query.
        return true;
    }
    if ((watched == m_worksView || watched == m_worksView->viewport())
        && event->type() == QEvent::ContextMenu) {
        const auto *contextEvent = static_cast<QContextMenuEvent *>(event);
        const bool keyboard = contextEvent->reason() == QContextMenuEvent::Keyboard;
        const auto index = keyboard ? m_worksView->currentIndex()
            : m_worksView->indexAt(m_worksView->viewport()->mapFromGlobal(contextEvent->globalPos()));
        const auto position = keyboard ? m_worksView->viewport()->mapToGlobal(
            m_worksView->visualRect(index).center()) : contextEvent->globalPos();
        showWorkContextMenu(index, position);
        return true;
    }
    if (watched == m_worksView && event->type() == QEvent::KeyPress) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Menu && keyEvent->modifiers() == Qt::NoModifier)
            || (keyEvent->key() == Qt::Key_F10 && keyEvent->modifiers() == Qt::ShiftModifier)) {
            if (!keyEvent->isAutoRepeat()) {
                const auto index = m_worksView->currentIndex();
                showWorkContextMenu(index, m_worksView->viewport()->mapToGlobal(
                    m_worksView->visualRect(index).center()));
            }
            return true;
        }
    }
    if (m_trackBatchMode && watched == m_tracksView->viewport()
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const auto index = m_tracksView->indexAt(mouse->position().toPoint()).siblingAtColumn(0);
        if (mouse->button() == Qt::LeftButton && isCurrentPageIndex(index)
            && !index.data(DirectoryRole).toBool()) {
            if (event->type() == QEvent::MouseButtonPress) {
                m_tracksView->setFocus(Qt::MouseFocusReason);
                m_tracksView->setCurrentIndex(index);
                const auto state = index.data(Qt::CheckStateRole);
                if (state.isValid())
                    m_tracksModel->setData(index, state.toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                           Qt::CheckStateRole);
            }
            return true;
        }
    }
    if (m_tracksView && (watched == m_tracksView || watched == m_tracksView->viewport())
        && event->type() == QEvent::ContextMenu) {
        if (m_trackBatchMode) return true;
        const auto *contextEvent = static_cast<QContextMenuEvent *>(event);
        const auto index = contextEvent->reason() == QContextMenuEvent::Keyboard
            ? m_tracksView->currentIndex()
            : m_tracksView->indexAt(m_tracksView->viewport()->mapFromGlobal(contextEvent->globalPos()));
        showTrackContextMenu(index, contextEvent->globalPos());
        return true;
    }
    if (watched == m_tracksView && event->type() == QEvent::KeyPress) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (m_trackBatchMode && keyEvent->key() == Qt::Key_Escape) {
            if (!keyEvent->isAutoRepeat()) setTrackBatchMode(false);
            return true;
        }
        if (m_trackBatchMode && keyEvent->key() == Qt::Key_Space) {
            if (!keyEvent->isAutoRepeat()) {
                const auto index = m_tracksView->currentIndex().siblingAtColumn(0);
                const auto state = index.data(Qt::CheckStateRole);
                if (isCurrentPageIndex(index) && state.isValid())
                    m_tracksModel->setData(index, state.toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                           Qt::CheckStateRole);
            }
            return true;
        }
        if (m_trackBatchMode && keyEvent->key() == Qt::Key_A && keyEvent->modifiers() == Qt::ControlModifier) {
            if (!keyEvent->isAutoRepeat()) {
                m_batchSelectAll->setCheckState(Qt::Unchecked);
                m_batchSelectAll->click();
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Menu
            || (keyEvent->key() == Qt::Key_F10 && keyEvent->modifiers() == Qt::ShiftModifier)) {
            if (!m_trackBatchMode && !keyEvent->isAutoRepeat()) {
                const auto index = m_tracksView->currentIndex();
                showTrackContextMenu(index, m_tracksView->viewport()->mapToGlobal(
                    m_tracksView->visualRect(index).center()));
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (!keyEvent->isAutoRepeat())
                activateItem(m_tracksView->currentIndex());
            return true;
        }
        const bool up = (keyEvent->key() == Qt::Key_Backspace
                         && keyEvent->modifiers() == Qt::NoModifier)
            || (keyEvent->key() == Qt::Key_Up && keyEvent->modifiers() == Qt::AltModifier);
        if (up && m_tracksView->hasFocus()) {
            if (!keyEvent->isAutoRepeat())
                navigateUp();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QPoint LibraryPanel::importIconCenter() const
{
    const auto *button = static_cast<const SidebarButton *>(m_importMenuButton);
    return button ? button->mapTo(this, button->iconCenter()) : QPoint();
}

void LibraryPanel::setWorkspaceFooter(QWidget *footer)
{
    // Installing the supplied instance never rebuilds its controls. Detail
    // view can temporarily take it and later return that same instance.
    if (!footer || m_workspaceFooter)
        return;
    m_workspaceFooter = footer;
    auto *workspace = m_workspaceLayout->parentWidget();
    workspace->setMinimumWidth(std::max(workspace->minimumWidth(), footer->minimumWidth()));
    m_workspaceLayout->addWidget(footer);
}

QWidget *LibraryPanel::takeWorkspaceFooter()
{
    QWidget *footer = m_workspaceFooter;
    if (footer) m_workspaceLayout->removeWidget(footer);
    m_workspaceFooter = nullptr;
    return footer;
}

void LibraryPanel::prepareForSnapshot()
{
    auto *paneLayout = m_workInformation->parentWidget()->layout();
    // Showing the previously hidden directory page can resize its pane, which
    // changes the cover/header minimums and then the navigation row's width.
    // Settle those specific nested layouts before freezing their rectangles.
    // The bounded passes cover parent size -> header size -> breadcrumb width;
    // no event-loop turn or unrelated callback is needed during capture.
    for (int pass = 0; pass < 3; ++pass) {
        layout()->activate();
        m_workspaceLayout->activate();
        updateWorkHeaderGeometry();
        m_workInformation->layout()->activate();
        updateBreadcrumbLayout();
        paneLayout->invalidate();
        paneLayout->activate();
        m_workInformation->layout()->activate();
        m_navigationBar->layout()->activate();
    }
}

void LibraryPanel::setApplicationTitleVisible(bool visible)
{
    m_applicationTitleVisible = visible;
    updatePresentation();
}

bool LibraryPanel::isTrackViewTarget(const QWidget *widget) const
{
    return !m_trackBatchMode && widget && (widget == m_tracksView || widget == m_tracksView->viewport());
}

bool LibraryPanel::isCurrentPageIndex(const QModelIndex &index) const
{
    return !m_closing && m_displayedWorkId > 0 && m_displayedWorkId == m_selectedWorkId
        && m_tracksView->isEnabled() && index.isValid() && index.model() == m_tracksModel
        && index.parent() == m_tracksView->rootIndex();
}

void LibraryPanel::activateItem(const QModelIndex &index)
{
    if (!isCurrentPageIndex(index))
        return;
    if (index.data(DirectoryRole).toBool())
        navigateToDirectory(index.data(RelativePathRole).toString());
    else if (!m_trackBatchMode)
        requestTrackPlayback(index);
}

void LibraryPanel::requestTrackPlayback(const QModelIndex &index)
{
    if (!isCurrentPageIndex(index) || index.data(DirectoryRole).toBool())
        return;
    const qint64 fileId = index.data(FileIdRole).toLongLong();
    if (fileId > 0)
        emit trackPlayRequested(fileId);
}

bool LibraryPanel::canRequestQueuePlayback() const
{
    return !m_closing && m_importAvailable && !m_querying && m_displayedWorkId > 0
        && m_displayedWorkId == m_selectedWorkId;
}

bool LibraryPanel::canRequestWorkRemoval() const
{
    return !m_closing && m_importAvailable && !m_active && !m_querying;
}

void LibraryPanel::invalidateWorkContextMenu()
{
    ++m_workMenuRevision;
    m_workMenuContext = {};
    m_removeWorkAction->setEnabled(false);
    m_workContextMenu->close();
    m_workReferencesAction->setEnabled(false);
}

void LibraryPanel::showWorkContextMenu(const QModelIndex &index, const QPoint &globalPosition)
{
    invalidateWorkContextMenu();
    const auto work = m_worksModel->workAt(index);
    if (m_closing || !work || work->id <= 0 || !m_worksView->isEnabled())
        return;
    m_workMenuContext = {m_workMenuRevision, *work};
    m_removeWorkAction->setEnabled(canRequestWorkRemoval());
    m_workReferencesAction->setEnabled(canRequestWorkRemoval());
    m_workContextMenu->popup(globalPosition);
}

bool LibraryPanel::hasAvailableDirectoryAudio() const
{
    const auto root = m_currentDirectory.isEmpty() ? QModelIndex()
        : m_tracksModel->indexForDirectory(m_currentDirectory);
    if (!m_currentDirectory.isEmpty() && !root.isValid())
        return false;
    for (int row = 0; row < m_tracksModel->rowCount(root); ++row) {
        const auto index = m_tracksModel->index(row, 0, root);
        if (!index.data(DirectoryRole).toBool() && index.data(FileIdRole).toLongLong() > 0
            && !index.data(MissingRole).toBool())
            return true;
    }
    return false;
}

QVector<qint64> LibraryPanel::selectedTranslationFileIds() const
{
    QVector<qint64> ids;
    if (!canRequestQueuePlayback() || !m_tracksView->selectionModel()) return ids;
    auto rows = m_tracksView->selectionModel()->selectedRows(0);
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.row() < b.row(); });
    for (const auto &index : rows) {
        const auto id = index.data(FileIdRole).toLongLong();
        if (isCurrentPageIndex(index) && !index.data(DirectoryRole).toBool()
            && !index.data(MissingRole).toBool() && id > 0 && !ids.contains(id))
            ids.append(id);
    }
    return ids;
}

void LibraryPanel::setTrackBatchMode(bool enabled)
{
    if (enabled && (!canRequestQueuePlayback() || !hasAvailableDirectoryAudio())) return;
    if (m_trackBatchMode == enabled) return;
    m_trackBatchMode = enabled;
    m_batchMenuRevision = 0;
    m_batchMenuIds.clear();
    m_batchAddMenu->close();
    m_trackContextMenu->close();
    m_trackMenuContext = {};
    m_translationMenuIds.clear();
    m_tracksView->setProperty("trackBatchMode", enabled);
    m_tracksModel->setBatchSelectionEnabled(enabled);
    m_tracksView->setSelectionMode(enabled ? QAbstractItemView::SingleSelection
                                         : QAbstractItemView::ExtendedSelection);
    m_tracksView->clearSelection();
    m_tracksView->viewport()->update();
    updatePresentation();
    if (enabled) m_tracksView->setFocus(Qt::OtherFocusReason);
}

QVector<qint64> LibraryPanel::checkedTrackFileIds() const
{
    QVector<qint64> ids;
    if (!m_trackBatchMode || !canRequestQueuePlayback()) return ids;
    const auto root = m_tracksView->rootIndex();
    for (int row = 0; row < m_tracksModel->rowCount(root); ++row) {
        const auto index = m_tracksModel->index(row, 0, root);
        const auto id = index.data(FileIdRole).toLongLong();
        if (id > 0 && index.data(Qt::CheckStateRole).isValid()
            && index.data(Qt::CheckStateRole).toInt() == Qt::Checked && !ids.contains(id))
            ids.append(id);
    }
    return ids;
}

void LibraryPanel::updateTrackBatchActions()
{
    if (!m_batchToolbar) return;
    m_batchToolbar->setVisible(m_trackBatchMode && m_selectedWorkId > 0);
    const auto ids = checkedTrackFileIds();
    int available = 0;
    const auto root = m_tracksView->rootIndex();
    for (int row = 0; row < m_tracksModel->rowCount(root); ++row)
        if (m_tracksModel->index(row, 0, root).data(Qt::CheckStateRole).isValid()) ++available;
    const QSignalBlocker blocker(m_batchSelectAll);
    m_batchSelectAll->setCheckState(ids.isEmpty() ? Qt::Unchecked
        : ids.size() == available ? Qt::Checked : Qt::PartiallyChecked);
    m_batchSelectAll->setEnabled(m_trackBatchMode && canRequestQueuePlayback() && available > 0);
    m_batchSelectionLabel->setText(QStringLiteral("已选 %1 首").arg(ids.size()));
    m_batchAddButton->setEnabled(!ids.isEmpty());
    for (auto *action : {m_batchEnqueueAction, m_batchOfflineTranslateAction, m_batchJapaneseAsrAction})
        action->setEnabled(!ids.isEmpty());
}

void LibraryPanel::triggerTrackBatchAction(const QString &modeId)
{
    if (!m_trackBatchMode || !canRequestQueuePlayback() || m_batchMenuIds.isEmpty()
        || m_batchMenuRevision != m_browseRevision || m_batchMenuWorkId != m_displayedWorkId
        || m_batchMenuDirectory != m_currentDirectory || m_batchMenuIds != checkedTrackFileIds()) return;
    const auto workId = m_displayedWorkId;
    const auto ids = m_batchMenuIds;
    m_batchMenuRevision = 0;
    m_batchAddMenu->close();
    if (modeId.isEmpty()) emit tracksEnqueueRequested(workId, ids);
    else emit generateSubtitlesRequested(workId, ids, modeId);
}

void LibraryPanel::updateTranslationActions()
{
    if (!m_batchButton || !m_offlineTranslateTracksAction || !m_japaneseAsrTracksAction) return;
    m_batchButton->setVisible(m_selectedWorkId > 0 && !m_trackBatchMode);
    m_batchButton->setEnabled(canRequestQueuePlayback() && hasAvailableDirectoryAudio());
    updateTrackBatchActions();
    const bool canGenerate = canRequestQueuePlayback()
        && m_trackMenuContext.revision == m_browseRevision && !m_translationMenuIds.isEmpty();
    m_offlineTranslateTracksAction->setEnabled(canGenerate);
    m_japaneseAsrTracksAction->setEnabled(canGenerate);
}

void LibraryPanel::triggerTrackSubtitleAction(const QString &modeId)
{
    const auto context = m_trackMenuContext;
    if (context.revision != m_browseRevision || context.workId != m_displayedWorkId
        || context.directory != m_currentDirectory || !canRequestQueuePlayback())
        return;
    QVector<qint64> ids;
    for (const auto id : m_translationMenuIds) {
        const auto index = m_tracksModel->indexForFileId(id);
        if (!isCurrentPageIndex(index) || index.data(DirectoryRole).toBool()
            || index.data(MissingRole).toBool()) return;
        ids.append(id);
    }
    if (!ids.isEmpty()) emit generateSubtitlesRequested(context.workId, ids, modeId);
}

void LibraryPanel::showTrackContextMenu(const QModelIndex &index, const QPoint &globalPosition)
{
    if (!canRequestQueuePlayback() || !isCurrentPageIndex(index)
        || index.data(DirectoryRole).toBool() || index.data(FileIdRole).toLongLong() <= 0)
        return;
    if (!m_tracksView->selectionModel()->isRowSelected(index.row(), index.parent()))
        m_tracksView->selectionModel()->setCurrentIndex(index,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_translationMenuIds = selectedTranslationFileIds();
    m_trackMenuContext = {m_browseRevision, m_displayedWorkId,
        index.data(FileIdRole).toLongLong(), m_currentDirectory,
        index.data(RelativePathRole).toString()};
    m_enqueueTrackAction->setEnabled(!index.data(MissingRole).toBool());
    updateTranslationActions();
    m_trackContextMenu->popup(globalPosition);
}

void LibraryPanel::navigateToDirectory(const QString &path, const QString &selectedDirectory)
{
    if (m_closing || m_displayedWorkId <= 0 || m_displayedWorkId != m_selectedWorkId
        || !m_tracksView->isEnabled() || path == m_currentDirectory)
        return;
    if (!path.isEmpty() && !m_tracksModel->indexForDirectory(path).isValid())
        return;
    rememberBrowseState();
    setTrackBatchMode(false);
    ++m_browseRevision;
    // Focus before changing pages so a focus event cannot select the first row
    // after a newly visited page has deliberately cleared its selection.
    m_tracksView->setFocus(Qt::OtherFocusReason);
    m_browseStates[m_displayedWorkId].currentDirectory = path;
    restoreBrowseState();
    if (!selectedDirectory.isEmpty()) {
        const auto selected = m_tracksModel->indexForDirectory(selectedDirectory);
        if (isCurrentPageIndex(selected)) {
            const QSignalBlocker blocker(m_tracksView->selectionModel());
            m_tracksView->selectionModel()->setCurrentIndex(selected,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_tracksView->scrollTo(selected, QAbstractItemView::EnsureVisible);
        }
    }
    rememberBrowseState();
    rebuildBreadcrumbs();
    updatePresentation();
}

void LibraryPanel::navigateUp()
{
    if (m_currentDirectory.isEmpty())
        return;
    const QString leaving = m_currentDirectory;
    const auto separator = leaving.lastIndexOf(QLatin1Char('/'));
    const QString parent = separator < 0 ? QString() : leaving.left(separator);
    navigateToDirectory(parent, leaving);
}

void LibraryPanel::rebuildBreadcrumbs()
{
    m_breadcrumbOverflowMenu->close();
    m_breadcrumbOverflowMenu->clear();
    m_hiddenBreadcrumbPaths.clear();
    m_breadcrumbSegments.clear();
    // Retire old widgets after their clicked handlers return. Their path
    // property is removed immediately so UI lookup cannot mistake them for
    // current breadcrumb buttons before deferred deletion is processed.
    // Root and overflow controls remain stable keyboard targets; only the
    // directory-specific tail is retired.
    while (m_breadcrumbLayout->count() > 3) {
        auto *item = m_breadcrumbLayout->takeAt(3);
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->disconnect(this);
            widget->setProperty("relativePath", QVariant());
            widget->setParent(this);
            widget->deleteLater();
        }
        delete item;
    }
    m_breadcrumbRootButton->setAccessibleName(QStringLiteral("作品根"));
    m_breadcrumbRootButton->setToolTip(safeToolTip(m_browseTitleLabel->text()
        + QLatin1Char('\n') + m_worksModel->index(m_worksModel->rowForId(m_displayedWorkId))
            .data(RootPathRole).toString()));
    QString relative;
    const auto workId = m_displayedWorkId;
    for (const auto &component : m_currentDirectory.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        relative = relative.isEmpty() ? component : relative + QLatin1Char('/') + component;
        auto *separator = new QLabel(QStringLiteral("›"), m_breadcrumbBar);
        separator->setTextFormat(Qt::PlainText);
        m_breadcrumbLayout->addWidget(separator);
        auto *button = new QToolButton(m_breadcrumbBar);
        button->setProperty("relativePath", relative);
        button->setAccessibleName(component);
        button->setText(QString(component).replace(QLatin1Char('&'), QStringLiteral("&&")));
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setToolTip(safeToolTip(relative));
        button->installEventFilter(this);
        m_breadcrumbLayout->addWidget(button);
        const QString path = relative;
        connect(button, &QToolButton::clicked, this, [this, path, workId] {
            if (workId == m_displayedWorkId)
                navigateToDirectory(path);
        });
        m_breadcrumbSegments.append({button, separator, path, component});
    }
    m_breadcrumbLayout->addStretch();
    scheduleBreadcrumbVisibility();
}

void LibraryPanel::scheduleBreadcrumbVisibility()
{
    if (m_breadcrumbVisibilityScheduled) return;
    m_breadcrumbVisibilityScheduled = true;
    // Resolve the latest page after the navigation row has adopted its width.
    QTimer::singleShot(0, this, [this] {
        m_breadcrumbVisibilityScheduled = false;
        if (m_closing) return;
        updateBreadcrumbLayout();
    });
}

void LibraryPanel::updateBreadcrumbLayout()
{
    const int available = std::max(1, m_breadcrumbBar->width());
    const int spacing = m_breadcrumbLayout->spacing();
    // These text-only buttons use the same style as the root. Its complete
    // label gives the actual style padding, including large-font/DPI rules.
    const QString rootName = QStringLiteral("作品根");
    const auto metrics = m_breadcrumbRootButton->fontMetrics();
    const int padding = std::max(0, m_breadcrumbRootButton->sizeHint().width()
        - metrics.horizontalAdvance(m_breadcrumbRootButton->text()));
    const int separatorWidth = std::max(8, metrics.horizontalAdvance(QStringLiteral("›")));
    const int overflowWidth = std::max(28, metrics.horizontalAdvance(QStringLiteral("…")) + padding);
    const int naturalRootWidth = metrics.horizontalAdvance(rootName) + padding;
    const int rootWidth = available < 240
        ? metrics.horizontalAdvance(QStringLiteral("根")) + padding : naturalRootWidth;
    const int count = static_cast<int>(m_breadcrumbSegments.size());
    QVector<int> widths;
    int completeWidth = naturalRootWidth;
    for (const auto &segment : std::as_const(m_breadcrumbSegments)) {
        // Integer font metrics can round down a fractional native-font advance.
        // Use the same floating metrics for measurement and elision so a label
        // that fits does not lose its final characters to an unnecessary ellipsis.
        const QFontMetricsF segmentMetrics(segment.button->fontMetrics());
        const int width = std::min(180, qCeil(segmentMetrics.horizontalAdvance(segment.name)) + padding);
        widths.append(width);
        completeWidth += separatorWidth + width + spacing * 2;
    }
    int firstVisible = 0;
    bool overflow = false;
    if (completeWidth > available && count > 1) {
        firstVisible = count - 1;
        overflow = true;
        // Keep the immediate parent when both it and the current directory can
        // retain readable labels. Older ancestors remain reachable in order.
        const int parentAndCurrentMinimum = rootWidth + overflowWidth
            + separatorWidth * 3 + spacing * 6 + padding * 2
            + metrics.horizontalAdvance(QStringLiteral("目录")) * 2;
        if (count > 2 && available >= parentAndCurrentMinimum)
            firstVisible = count - 2;
    }
    if (!overflow && completeWidth <= available) {
        m_breadcrumbRootButton->setText(rootName);
        m_breadcrumbRootButton->setFixedWidth(naturalRootWidth);
    } else {
        m_breadcrumbRootButton->setText(available < 240 ? QStringLiteral("根") : rootName);
        m_breadcrumbRootButton->setFixedWidth(std::min(rootWidth, available));
    }
    m_breadcrumbOverflowButton->setVisible(overflow);
    m_breadcrumbOverflowSeparator->setVisible(overflow);
    m_breadcrumbOverflowButton->setFixedWidth(overflowWidth);
    m_breadcrumbOverflowSeparator->setFixedWidth(separatorWidth);
    int remainder = available - m_breadcrumbRootButton->width()
        - (overflow ? overflowWidth + separatorWidth + spacing * 2 : 0)
        - (count - firstVisible) * (separatorWidth + spacing * 2);
    QVector<int> visibleWidths(count, 0);
    // Allocate from the current directory backwards; the newest name keeps
    // priority if either of the final two components needs elision.
    for (int index = count - 1; index >= firstVisible; --index) {
        const int precedingMinimum = index > firstVisible
            ? padding + metrics.horizontalAdvance(QStringLiteral("…")) : 0;
        visibleWidths[index] = std::max(1, std::min(widths[index], remainder - precedingMinimum));
        remainder -= visibleWidths[index];
    }
    QStringList hiddenPaths;
    auto *previous = m_breadcrumbRootButton;
    if (overflow) {
        setTabOrder(previous, m_breadcrumbOverflowButton);
        previous = m_breadcrumbOverflowButton;
    }
    for (int index = 0; index < count; ++index) {
        const auto &segment = m_breadcrumbSegments[index];
        const bool visible = index >= firstVisible;
        segment.button->setVisible(visible);
        segment.separator->setVisible(visible);
        if (!visible) {
            hiddenPaths.append(segment.path);
            continue;
        }
        segment.separator->setFixedWidth(separatorWidth);
        segment.button->setFixedWidth(visibleWidths[index]);
        const QString text = QFontMetricsF(segment.button->fontMetrics()).elidedText(segment.name, Qt::ElideRight,
            std::max(0, visibleWidths[index] - padding));
        segment.button->setText(QString(text).replace(QLatin1Char('&'), QStringLiteral("&&")));
        setTabOrder(previous, segment.button);
        previous = segment.button;
    }
    setTabOrder(previous, m_tracksView);
    if (hiddenPaths != m_hiddenBreadcrumbPaths) {
        m_breadcrumbOverflowMenu->close();
        m_breadcrumbOverflowMenu->clear();
        m_hiddenBreadcrumbPaths = hiddenPaths;
        const auto revision = m_browseRevision;
        const auto workId = m_displayedWorkId;
        for (const QString &path : std::as_const(hiddenPaths)) {
            const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
            auto *action = m_breadcrumbOverflowMenu->addAction(QString(name).replace(QLatin1Char('&'), QStringLiteral("&&")));
            action->setProperty("relativePath", path);
            action->setToolTip(safeToolTip(path));
            connect(action, &QAction::triggered, this, [this, path, revision, workId] {
                if (revision == m_browseRevision && workId == m_displayedWorkId)
                    navigateToDirectory(path);
            });
        }
    }
    const int height = std::max(m_breadcrumbRootButton->sizeHint().height(),
        m_breadcrumbOverflowButton->sizeHint().height());
    if (m_breadcrumbBar->height() != height) m_breadcrumbBar->setFixedHeight(height);
    m_breadcrumbLayout->activate();
}

void LibraryPanel::setLibrary(const QVector<LibraryWork> &works,
                              const QVector<LibraryFile> &files, qint64 selectedWorkId,
                              const QSet<qint64> &subtitleAudioFileIds)
{
    invalidateWorkContextMenu();
    rememberBrowseState();
    ++m_browseRevision;
    for (auto it = m_manualCovers.begin(); it != m_manualCovers.end();) {
        const auto work = std::find_if(works.cbegin(), works.cend(),
            [id = it.key()](const auto &entry) { return entry.id == id; });
        if (work == works.cend() || !m_worksModel->matchesIdentity(*work))
            it = m_manualCovers.erase(it);
        else ++it;
    }
    for (const auto &work : works) {
        if (work.id == m_displayedWorkId && !m_worksModel->matchesIdentity(work)) {
            setTrackBatchMode(false);
            m_tracksModel->clearWorkDurations(work.id);
        }
    }
    const QSignalBlocker blocker(m_worksView->selectionModel());
    m_worksModel->setWorks(works);
    const int selectedRow = m_worksModel->rowForId(selectedWorkId);
    m_selectedWorkId = selectedRow < 0 ? 0 : selectedWorkId;
    restoreVisibleWorkSelection();
    const bool reset = m_tracksModel->setFiles(m_selectedWorkId, files);
    m_tracksModel->setSubtitleTracks(m_selectedWorkId, subtitleAudioFileIds);
    m_displayedWorkId = m_selectedWorkId;
    // Deleted works lose their cached pages; surviving works keep navigation and selection.
    for (auto it = m_browseStates.begin(); it != m_browseStates.end();) {
        if (m_worksModel->rowForId(it.key()) < 0)
            it = m_browseStates.erase(it);
        else
            ++it;
    }
    if (reset)
        restoreBrowseState();
    const QString title = selectedRow < 0 ? QStringLiteral("音轨")
        : works.at(selectedRow).displayName;
    m_browseTitleLabel->setText(title);
    m_browseTitleLabel->setToolTip(safeToolTip(title));
    updateBrowsedCover();
    rebuildBreadcrumbs();
    m_libraryErrorText.clear();
    updatePresentation();
    scheduleWorkCoverDemand();
    scheduleTrackDurationDemand(true);
}

void LibraryPanel::rememberBrowseState()
{
    if (m_displayedWorkId <= 0)
        return;
    DirectoryPageState page;
    const auto current = m_tracksView->currentIndex();
    if (isCurrentPageIndex(current)) {
        page.column = current.column();
        if (current.data(DirectoryRole).toBool())
            page.selectedDirectory = current.data(RelativePathRole).toString();
        else
            page.fileId = current.data(FileIdRole).toLongLong();
    }
    page.horizontalScroll = m_tracksView->horizontalScrollBar()->value();
    page.verticalScroll = m_tracksView->verticalScrollBar()->value();
    auto &state = m_browseStates[m_displayedWorkId];
    state.currentDirectory = m_currentDirectory;
    state.pages.insert(m_currentDirectory, std::move(page));
}

void LibraryPanel::restoreBrowseState()
{
    const auto found = m_browseStates.constFind(m_displayedWorkId);
    QString path = found == m_browseStates.cend() ? QString() : found->currentDirectory;
    while (!path.isEmpty() && !m_tracksModel->indexForDirectory(path).isValid()) {
        const auto separator = path.lastIndexOf(QLatin1Char('/'));
        path = separator < 0 ? QString() : path.left(separator);
    }
    const DirectoryPageState page = found == m_browseStates.cend()
        ? DirectoryPageState() : found->pages.value(path);
    m_currentDirectory = path;
    const auto root = path.isEmpty() ? QModelIndex() : m_tracksModel->indexForDirectory(path);
    const QSignalBlocker blocker(m_tracksView->selectionModel());
    m_tracksView->setRootIndex(root);
    m_tracksView->selectionModel()->clear();
    const auto current = page.fileId > 0 ? m_tracksModel->indexForFileId(page.fileId)
        : page.selectedDirectory.isEmpty() ? QModelIndex()
            : m_tracksModel->indexForDirectory(page.selectedDirectory);
    if (isCurrentPageIndex(current)) {
        const int column = std::clamp(page.column, 0, m_tracksModel->columnCount() - 1);
        m_tracksView->selectionModel()->setCurrentIndex(current.siblingAtColumn(column),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_tracksView->doItemsLayout();
    m_tracksView->horizontalScrollBar()->setValue(page.horizontalScroll);
    m_tracksView->verticalScrollBar()->setValue(page.verticalScroll);
    m_tracksView->viewport()->update();
    if (m_displayedWorkId > 0)
        m_browseStates[m_displayedWorkId].currentDirectory = path;
}

void LibraryPanel::restoreVisibleWorkSelection()
{
    const int sourceRow = m_worksModel->rowForId(m_selectedWorkId);
    const auto visible = sourceRow < 0 ? QModelIndex()
        : m_worksModel->index(sourceRow);
    if (visible.isValid()) {
        m_worksView->selectionModel()->setCurrentIndex(visible,
            QItemSelectionModel::ClearAndSelect);
    } else {
        m_worksView->selectionModel()->clear();
    }
}

void LibraryPanel::setPlayingTrack(qint64 workId, qint64 fileId)
{
    m_tracksModel->setPlayingTrack(workId, fileId);
}

void LibraryPanel::setQuerying(bool querying)
{
    const bool changed = m_querying != querying;
    if (changed) invalidateWorkContextMenu();
    if (querying) setTrackBatchMode(false);
    if (changed)
        ++m_browseRevision;
    m_querying = querying;
    if (changed) rebuildBreadcrumbs();
    updatePresentation();
    if (changed && !querying) scheduleTrackDurationDemand(true);
}

void LibraryPanel::setImportAvailable(bool available)
{
    if (m_importAvailable != available) invalidateWorkContextMenu();
    if (!available) setTrackBatchMode(false);
    m_importAvailable = available;
    updatePresentation();
}

void LibraryPanel::setImportState(LibraryImportState state, const QString &text, bool active, bool batch)
{
    if (m_active != active) invalidateWorkContextMenu();
    m_importState = state;
    m_active = active;
    m_batchImportMode = batch;
    m_statusText = text;
    if (!active && state == LibraryImportState::Failed)
        m_taskErrorText = text;
    else if (state == LibraryImportState::Scanning || state == LibraryImportState::Completed
             || state == LibraryImportState::Cancelled)
        m_taskErrorText.clear();
    updatePresentation();
}

void LibraryPanel::setWorkRemovalActive(bool active)
{
    m_workRemovalActive = active;
    updatePresentation();
}

void LibraryPanel::setImportFeedback(const QString &text, const QString &details)
{
    m_statusText = text;
    m_diagnosticDetails = details;
    m_taskErrorText.clear();
    updatePresentation();
}

void LibraryPanel::setImportError(const QString &message)
{
    m_statusText = message;
    m_taskErrorText = message;
    updatePresentation();
}

void LibraryPanel::setLibraryError(const QString &message)
{
    // An unavailable query never destroys the last successful browse snapshot.
    m_libraryErrorText = message;
    updatePresentation();
}

void LibraryPanel::setClosing(bool closing)
{
    if (m_closing != closing) invalidateWorkContextMenu();
    if (closing) setTrackBatchMode(false);
    if (m_closing != closing)
        ++m_browseRevision;
    m_closing = closing;
    if (closing) {
        m_importMenuButton->menu()->close();
        m_settingsMenu->close();
        m_trackContextMenu->close();
        m_breadcrumbOverflowMenu->close();
    }
    if (!closing) rebuildBreadcrumbs();
    updatePresentation();
}

void LibraryPanel::setManualCoverAvailable(qint64 workId, bool manual)
{
    if (workId <= 0) return;
    if (manual) m_manualCovers.insert(workId, true);
    else m_manualCovers.remove(workId);
    updatePresentation();
}

void LibraryPanel::setWorkCover(qint64 workId, const CoverArtResult &result)
{
    if (m_closing || workId <= 0 || m_worksModel->rowForId(workId) < 0)
        return;
    const auto index = m_worksModel->index(m_worksModel->rowForId(workId));
    const bool visible = m_worksView->isVisible()
        && m_worksView->visualRect(index).intersects(m_worksView->viewport()->rect());
    if (workId != m_selectedWorkId && !visible)
        return;
    m_worksModel->setCover(workId, result);
    if (workId == m_selectedWorkId) updateBrowsedCover();
}

void LibraryPanel::clearWorkCover(qint64 workId)
{
    m_worksModel->clearCover(workId);
    if (workId == m_selectedWorkId) updateBrowsedCover();
}

void LibraryPanel::updateBrowsedCover()
{
    const auto *result = m_worksModel->cover(m_selectedWorkId);
    if (!result) {
        m_workCover->clearImage(QStringLiteral("正在读取作品封面…"));
        return;
    }
    if (result->status == CoverArtStatus::Ready && !result->image.isNull()) {
        QString detail = result->manual ? QStringLiteral("手动指定封面") : QStringLiteral("目录自动封面");
        if (result->manualFallback) detail += QStringLiteral("；手动图片不可用，已回退自动查找");
        m_workCover->setImage(result->image, result->sourcePath, detail);
    } else {
        m_workCover->clearImage(result->manualFallback
            ? QStringLiteral("手动图片不可用，自动查找暂无封面") : QStringLiteral("暂无封面"));
    }
}

void LibraryPanel::requestWorkCoverDemand()
{
    scheduleWorkCoverDemand(true);
}

void LibraryPanel::requestTrackDurationDemand()
{
    scheduleTrackDurationDemand(true);
}

void LibraryPanel::setTrackDuration(qint64 workId, qint64 fileId,
                                   std::optional<qint64> milliseconds)
{
    if (!m_closing) m_tracksModel->setTrackDuration(workId, fileId, milliseconds);
}

void LibraryPanel::clearTrackDurations(qint64 workId)
{
    m_tracksModel->clearWorkDurations(workId);
}

void LibraryPanel::setSubtitleTracks(qint64 workId, const QSet<qint64> &audioFileIds)
{
    if (!m_closing) m_tracksModel->setSubtitleTracks(workId, audioFileIds);
}

void LibraryPanel::scheduleTrackDurationDemand(bool force)
{
    m_forceDurationDemand = m_forceDurationDemand || force;
    if (m_durationDemandTimer && !m_durationDemandTimer->isActive())
        m_durationDemandTimer->start(0);
}

void LibraryPanel::publishTrackDurationDemand()
{
    QVector<qint64> visible;
    const bool available = !m_closing && !m_querying && m_displayedWorkId > 0
        && m_displayedWorkId == m_selectedWorkId && m_tracksView->isVisible();
    const qint64 workId = available ? m_displayedWorkId : 0;
    if (available) {
        const auto root = m_tracksView->rootIndex();
        const QRect viewport = m_tracksView->viewport()->rect();
        int first = m_tracksView->rowAt(0);
        // Row spacing/headers may leave the first few pixels without an item.
        for (int y = 1; first < 0 && y < viewport.height(); y += 4)
            first = m_tracksView->rowAt(y);
        for (int row = first; row >= 0 && row < m_tracksModel->rowCount(root); ++row) {
            const auto index = m_tracksModel->index(row, 0, root);
            const QRect bounds = m_tracksView->visualRect(index);
            if (bounds.top() >= viewport.height()) break;
            const qint64 id = index.data(FileIdRole).toLongLong();
            if (bounds.intersects(viewport) && id > 0 && !index.data(DirectoryRole).toBool()
                && !index.data(MissingRole).toBool())
                visible.append(id);
        }
    }
    const bool force = std::exchange(m_forceDurationDemand, false);
    if (!force && workId == m_lastDurationDemandWork && visible == m_lastDurationDemandVisible)
        return;
    m_lastDurationDemandWork = workId;
    m_lastDurationDemandVisible = visible;
    emit trackDurationDemandChanged(workId, visible);
}

void LibraryPanel::scheduleWorkCoverDemand(bool force)
{
    m_forceCoverDemand = m_forceCoverDemand || force;
    if (m_coverDemandTimer && !m_coverDemandTimer->isActive())
        m_coverDemandTimer->start(0);
}

void LibraryPanel::publishWorkCoverDemand()
{
    QVector<qint64> visible;
    if (!m_closing && m_worksView->isVisible() && m_worksModel->rowCount() > 0) {
        const QRect viewport = m_worksView->viewport()->rect();
        QModelIndex first;
        // The first pixels may be spacing above the first row. Locate one row,
        // then visit only the intersecting rows instead of walking the library.
        for (int y = 0; y < viewport.height() && !first.isValid(); y += 4)
            first = m_worksView->indexAt(QPoint(viewport.width() / 2, y));
        if (first.isValid()) {
            for (int row = first.row(); row < m_worksModel->rowCount(); ++row) {
                const auto index = m_worksModel->index(row);
                const QRect bounds = m_worksView->visualRect(index);
                if (bounds.top() >= viewport.height()) break;
                if (bounds.intersects(viewport)) visible.append(index.data(WorkIdRole).toLongLong());
            }
        }
    }
    const qint64 selected = m_closing ? 0 : m_selectedWorkId;
    QSet<qint64> wanted(visible.cbegin(), visible.cend());
    if (selected > 0) wanted.insert(selected);
    m_worksModel->retainCovers(wanted);
    const bool changed = selected != m_lastCoverDemandSelected || visible != m_lastCoverDemandVisible;
    const bool force = std::exchange(m_forceCoverDemand, false);
    if (!changed && !force) return;
    m_lastCoverDemandSelected = selected;
    m_lastCoverDemandVisible = visible;
    emit workCoverDemandChanged(selected, visible);
}

void LibraryPanel::updateWorkHeaderGeometry()
{
    if (!m_workCover || !m_workInformation || !m_browseTitleLabel || m_updatingWorkHeaderGeometry) return;
    QScopedValueRollback updating(m_updatingWorkHeaderGeometry, true);
    auto *pane = m_workInformation->parentWidget();
    auto *paneLayout = pane->layout();
    // Switch before the regular header's size hint becomes a lower bound on
    // the pane; otherwise a requested short resize can never reach this mode.
    const bool compact = pane->height() < 400;
    // The dedicated navigation row remains below the work information even in
    // short windows. Recover space from margins instead of merging its actions.
    paneLayout->setContentsMargins(compact ? QMargins(12, 12, 12, 12)
                                          : QMargins(20, 20, 20, 16));
    paneLayout->setSpacing(compact ? 6 : 8);
    const auto margins = paneLayout->contentsMargins();
    const int width = pane->width() - margins.left() - margins.right();
    const bool narrow = width < 360;
    int side = compact || width < 360 ? 80 : width < 560 ? 96 : 144;
    auto *title = static_cast<WorkTitleLabel *>(m_browseTitleLabel);
    const bool smallActions = compact || narrow;
    if (m_workInformation->property("libraryCompactWorkActions").toBool() != smallActions) {
        m_workInformation->setProperty("libraryCompactWorkActions", smallActions);
        for (auto *button : {static_cast<QWidget *>(m_playDirectoryButton),
                             static_cast<QWidget *>(m_batchButton),
                             static_cast<QWidget *>(m_coverMenuButton)}) {
            if (smallActions) {
                button->setProperty("libraryRegularWorkActionStyleSheet", button->styleSheet());
                button->setStyleSheet(button->styleSheet() + QStringLiteral(
                    "\nQPushButton, QToolButton { padding: 4px 6px; min-height: 22px; }"));
            } else {
                button->setStyleSheet(button->property("libraryRegularWorkActionStyleSheet").toString());
                button->setProperty("libraryRegularWorkActionStyleSheet", QVariant());
            }
        }
    }
    auto *header = qobject_cast<QGridLayout *>(m_workInformation->layout());
    header->setHorizontalSpacing(narrow ? 8 : 20);
    header->setVerticalSpacing(compact ? 4 : 8);
    const int actionHeight = std::max(m_playDirectoryButton->sizeHint().height(),
                                      m_batchButton->sizeHint().height());
    const int lineHeight = title->fontMetrics().lineSpacing();
    // The right column must fit alongside the image. Large fonts use fewer
    // title lines before growing the square, retaining a true shared bottom.
    side = std::max(side, lineHeight + header->verticalSpacing() + actionHeight);
    const int titleLines = compact || narrow ? 1
        : std::clamp((side - actionHeight - header->verticalSpacing()) / std::max(1, lineHeight), 1, 2);
    title->setMaximumLines(titleLines);
    title->setMaximumHeight(lineHeight * titleLines);
    if (m_workCover->width() != side) m_workCover->setFixedSize(side, side);
    header->invalidate();
    // GridLayout also accounts for the edit control and style-specific
    // minimums. Use that actual bound, including fractional-DPI rounding,
    // rather than leaving the square one pixel shorter than its right column.
    side = std::max(side, header->minimumSize().height());
    if (m_workCover->width() != side) {
        m_workCover->setFixedSize(side, side);
        header->invalidate();
    }
    const auto *actions = m_workInformation->findChild<QHBoxLayout *>(
        QStringLiteral("libraryDirectoryActionsLayout"));
    // Allow both complete button labels beside an 80px image. This bound uses
    // their actual font/style metrics and fits the supported minimum window.
    const int minimumDetailsWidth = std::max(actions->minimumSize().width(),
        m_coverMenuButton->minimumSizeHint().width() + 8
            + title->fontMetrics().horizontalAdvance(QChar(0x2026)));
    pane->setMinimumWidth(std::max(220,
        margins.left() + margins.right() + 80 + 8 + minimumDetailsWidth));
    m_workInformation->setMinimumHeight(side);
    m_navigateUpButton->setText(compact ? QStringLiteral("↑") : QStringLiteral("上一级"));
    if (m_navigateUpButton->property("libraryCompactNavigation").toBool() != compact) {
        m_navigateUpButton->setProperty("libraryCompactNavigation", compact);
        if (compact) {
            m_navigateUpButton->setProperty("libraryRegularNavigationStyleSheet", m_navigateUpButton->styleSheet());
            m_navigateUpButton->setStyleSheet(m_navigateUpButton->styleSheet() + QStringLiteral(
                "\nQPushButton#libraryNavigateUpButton { padding: 6px; min-width: 0px; }"));
            m_navigateUpButton->setFixedWidth(36);
        } else {
            m_navigateUpButton->setMinimumWidth(0);
            m_navigateUpButton->setMaximumWidth(QWIDGETSIZE_MAX);
            m_navigateUpButton->setStyleSheet(
                m_navigateUpButton->property("libraryRegularNavigationStyleSheet").toString());
            m_navigateUpButton->setProperty("libraryRegularNavigationStyleSheet", QVariant());
        }
    }
}

QString LibraryPanel::statusText() const
{
    return m_statusText;
}

void LibraryPanel::updateWelcomeGeometry()
{
    // Keep the welcome controls as one centered group. A short embedded panel
    // gives the icon/spacing back before sacrificing either welcome action.
    const bool compact = m_welcomePage->height() < 300;
    const int side = compact ? 48 : 72;
    m_welcomeIconLabel->setFixedSize(side, side);
    m_welcomeIconLabel->setPixmap(applicationMarkIcon().pixmap(QSize(side, side), devicePixelRatioF()));
    m_welcomeContent->layout()->setSpacing(compact ? 8 : 12);
    const int preferredWidth = std::max({184, m_welcomeImportButton->sizeHint().width(),
                                        m_welcomeAddScanRootButton->sizeHint().width()});
    const int buttonHeight = std::max(m_welcomeImportButton->sizeHint().height(),
                                      m_welcomeAddScanRootButton->sizeHint().height());
    m_welcomeImportButton->setFixedHeight(buttonHeight);
    m_welcomeAddScanRootButton->setFixedHeight(buttonHeight);
    const int availableWidth = std::max(0, m_welcomePage->contentsRect().width() - 24);
    m_welcomeContent->setFixedWidth(std::min(preferredWidth, availableWidth));
    // Visibility/style changes invalidate the preferred height; measure both
    // actions now so returning from a query cannot retain a clipped group.
    m_welcomeContent->layout()->invalidate();
    m_welcomeContent->setFixedHeight(m_welcomeContent->layout()->totalSizeHint().height());
}

void LibraryPanel::updatePresentation()
{
    const bool importEnabled = m_importAvailable && !m_active && !m_closing;
    m_importMenuButton->setEnabled(!m_closing);
    m_aiSubtitlesButton->setEnabled(!m_closing);
    m_settingsButton->setEnabled(!m_closing);
    m_importAction->setEnabled(importEnabled);
    m_welcomeImportButton->setEnabled(importEnabled);
    m_addScanRootAction->setEnabled(importEnabled);
    m_welcomeAddScanRootButton->setEnabled(m_addScanRootAction->isEnabled());
    m_welcomeAddScanRootButton->setToolTip(m_addScanRootAction->toolTip());
    m_manageScanRootsAction->setEnabled(m_importAvailable && !m_closing);
    m_refreshScanRootsButton->setEnabled(importEnabled);
    m_cancelButton->setVisible(m_active);
    m_importAction->setToolTip(m_active
        ? QStringLiteral("已有导入任务正在运行，请等待完成或取消。")
        : QStringLiteral("递归发现作品内音频、字幕及参考资料，后台导入作品库并缓存资料正文。"));
    m_welcomeImportButton->setToolTip(m_importAction->toolTip());
    m_cancelButton->setEnabled(m_active && !m_closing
        && m_importState != LibraryImportState::Cancelling);
    m_cancelButton->setToolTip(m_workRemovalActive
        ? (m_importState == LibraryImportState::Committing
            ? QStringLiteral("数据库正在原子提交；收到删除结果前请等待完成。")
            : QStringLiteral("请求取消删除作品；本地媒体文件不会删除。"))
        : m_batchImportMode
        ? QStringLiteral("停止后续作品；当前仍在扫描时请求取消；已开始的提交或字幕关联将等待完成，再结束批次。")
        : m_importState == LibraryImportState::Committing
            ? QStringLiteral("数据库正在原子提交；此阶段取消将提示无法取消。")
            : m_importState == LibraryImportState::ProcessingReferences
                ? QStringLiteral("取消参考资料准备；已提交的作品音频和字幕保留。")
            : QStringLiteral("请求协作取消扫描，收到最终结果后才确认已取消。"));
    m_worksView->setEnabled(!m_closing);
    m_tracksView->setEnabled(!m_closing);
    const bool canQueue = canRequestQueuePlayback();
    updateTranslationActions();
    m_workInformation->setVisible(m_selectedWorkId > 0);
    m_coverMenuButton->setVisible(m_selectedWorkId > 0);
    m_coverMenuButton->setEnabled(canQueue);
    m_playDirectoryButton->setVisible(m_selectedWorkId > 0 && !m_trackBatchMode);
    m_playDirectoryButton->setEnabled(canQueue && hasAvailableDirectoryAudio());
    m_enqueueTrackAction->setEnabled(canQueue && m_trackMenuContext.revision == m_browseRevision
        && m_trackMenuContext.fileId > 0
        && !m_tracksModel->indexForFileId(m_trackMenuContext.fileId).data(MissingRole).toBool());
    const bool canNavigate = !m_closing && m_displayedWorkId > 0
        && m_displayedWorkId == m_selectedWorkId;
    m_navigationBar->setVisible(m_selectedWorkId > 0);
    m_navigateUpButton->setEnabled(canNavigate && !m_currentDirectory.isEmpty());
    for (auto *button : m_breadcrumbBar->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        const QString path = button->property("relativePath").toString();
        button->setEnabled(canNavigate && (button == m_breadcrumbOverflowButton
            || (button == m_breadcrumbRootButton ? !m_currentDirectory.isEmpty() : path != m_currentDirectory)));
    }
    m_queryLabel->setVisible(m_querying);
    m_queryLabel->setText(QStringLiteral("正在读取作品库…"));
    m_errorLabel->setText(m_libraryErrorText.isEmpty() ? m_taskErrorText : m_libraryErrorText);
    m_errorLabel->setVisible(!m_errorLabel->text().isEmpty());
    const int workCount = m_worksModel->rowCount();
    const qsizetype trackCount = m_tracksModel->trackCount();
    m_workCountLabel->setText(QStringLiteral("%1 部作品").arg(workCount));
    m_workCountLabel->setToolTip(QStringLiteral("共 %1 部作品。").arg(workCount));
    const bool welcome = workCount == 0 && !m_querying;
    m_welcomeContent->setVisible(welcome);
    m_welcomeIconLabel->setVisible(welcome);
    m_welcomeTitleLabel->setVisible(welcome);
    m_welcomeTitleLabel->setText(QStringLiteral("ASMR，启动"));
    // This label is shared with loading/no-selection/no-audio states. Hide only
    // the welcome copy, so query transitions and empty work feedback survive.
    m_tracksEmptyLabel->setVisible(!welcome);
    m_welcomeImportButton->setVisible(welcome);
    m_welcomeAddScanRootButton->setVisible(welcome);
    m_worksStack->setCurrentIndex(workCount == 0 ? 0 : 1);
    m_tracksStack->setCurrentIndex(trackCount == 0 ? 0 : 1);
    scheduleWorkCoverDemand();
    scheduleTrackDurationDemand();
    m_worksEmptyLabel->setText(QStringLiteral("本地作品会显示在这里"));
    if (m_querying) {
        m_tracksEmptyLabel->setText(m_selectedWorkId > 0
            ? QStringLiteral("正在读取音轨…") : QStringLiteral("正在读取作品库…"));
    } else if (welcome) {
        m_tracksEmptyLabel->clear();
    } else if (m_selectedWorkId == 0) {
        m_tracksEmptyLabel->setText(QStringLiteral("从左侧选择一部作品，查看音轨。"));
    } else {
        m_tracksEmptyLabel->setText(QStringLiteral("这部作品暂时没有音轨记录。"));
    }
    updateWelcomeGeometry();
    auto *sidebar = m_importMenuButton->parentWidget();
    const bool compact = sidebar->property("compactLibrarySidebar").toBool();
    sidebar->findChild<QLabel *>(QStringLiteral("applicationTitle"))->setVisible(m_applicationTitleVisible);
    m_workCountLabel->setVisible(!compact);
    const QString cancelName = m_workRemovalActive ? QStringLiteral("取消删除")
        : m_batchImportMode ? QStringLiteral("停止批次") : QStringLiteral("取消扫描");
    m_cancelButton->setAccessibleName(cancelName);
    m_cancelButton->setText(compact ? QStringLiteral("停止") : cancelName);
    QStringList footerDetails;
    if (!m_libraryErrorText.isEmpty()) footerDetails.append(m_libraryErrorText);
    if (!m_taskErrorText.isEmpty()) footerDetails.append(m_taskErrorText);
    if (m_querying) footerDetails.append(m_queryLabel->text());
    m_errorLabel->setToolTip(safeToolTip(compact ? footerDetails.join(QLatin1Char('\n')) : m_errorLabel->text()));
    if (compact) {
        // Only real errors and an active library query use the footer. Task
        // summaries and diagnostics have no persistent sidebar presentation.
        const bool hasError = !m_errorLabel->text().isEmpty();
        m_errorLabel->setVisible(hasError);
        m_queryLabel->setVisible(!hasError && m_querying);
    }
}

} // namespace asmr
