#include "ui/LibraryTrackTreeModel.h"
#include "library/NaturalSort.h"
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QStringList>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>
namespace asmr {
struct LibraryTrackTreeModel::Node {
    Node(QString title, QString relative, Node *owner, bool folder)
        : name(std::move(title)), path(std::move(relative)), key(name),
          parent(owner), directory(folder) {}
    QString name;
    QString path;
    NaturalSortKey key;
    Node *parent;
    bool directory;
    int row = 0;
    LibraryFile file;
    std::vector<std::unique_ptr<Node>> children;
};
LibraryTrackTreeModel::LibraryTrackTreeModel(QObject *parent)
    : QAbstractItemModel(parent), m_root(std::make_unique<Node>(QString(), QString(), nullptr, true)) {}
LibraryTrackTreeModel::~LibraryTrackTreeModel() = default;
LibraryTrackTreeModel::Node *LibraryTrackTreeModel::node(const QModelIndex &index) const
{
    return index.isValid() && index.model() == this
        ? static_cast<Node *>(index.internalPointer()) : nullptr;
}
QModelIndex LibraryTrackTreeModel::nodeIndex(const Node *entry, int column) const
{
    return entry && entry != m_root.get()
        ? createIndex(entry->row, column, const_cast<Node *>(entry)) : QModelIndex();
}
QModelIndex LibraryTrackTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= 3 || (parent.isValid() && parent.column() != 0)) return {};
    const auto *owner = parent.isValid() ? node(parent) : m_root.get();
    if (!owner || row >= static_cast<int>(owner->children.size())) return {};
    return createIndex(row, column, owner->children[static_cast<size_t>(row)].get());
}
QModelIndex LibraryTrackTreeModel::parent(const QModelIndex &child) const
{
    const auto *entry = node(child);
    return entry ? nodeIndex(entry->parent) : QModelIndex();
}
int LibraryTrackTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0) return 0;
    const auto *owner = parent.isValid() ? node(parent) : m_root.get();
    return owner ? static_cast<int>(owner->children.size()) : 0;
}
int LibraryTrackTreeModel::columnCount(const QModelIndex &) const { return 3; }
Qt::ItemFlags LibraryTrackTreeModel::flags(const QModelIndex &index) const
{
    const auto *entry = node(index);
    if (!entry) return Qt::NoItemFlags;
    auto result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (m_batchSelectionEnabled && index.column() == 0 && !entry->directory
        && !entry->file.missing && entry->file.id > 0)
        result |= Qt::ItemIsUserCheckable;
    return result;
}
QVariant LibraryTrackTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) return {};
    if (role == Qt::TextAlignmentRole && section == 1)
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return QStringLiteral("名称");
    case 1: return QStringLiteral("时长");
    case 2: return QStringLiteral("相对路径");
    default: return {};
    }
}
QVariant LibraryTrackTreeModel::data(const QModelIndex &index, int role) const
{
    const auto *entry = node(index);
    if (!entry) return {};
    const bool playing = !entry->directory && m_workId == m_playingWorkId
        && entry->file.id > 0 && entry->file.id == m_playingFileId;
    const bool subtitleAvailable = !entry->directory && !entry->file.missing
        && m_subtitleTracks.contains(entry->file.id);
    const QString subtitleDescription = subtitleAvailable
        ? QStringLiteral("已有关联字幕，是否启用由字幕设置决定。") : QString();
    switch (role) {
    case Qt::CheckStateRole:
        return m_batchSelectionEnabled && index.column() == 0 && !entry->directory
            && !entry->file.missing && entry->file.id > 0
            ? QVariant(m_checkedTracks.contains(entry->file.id) ? Qt::Checked : Qt::Unchecked) : QVariant();
    case WorkIdRole: return m_workId;
    case FileIdRole: return entry->directory ? qint64(0) : entry->file.id;
    case RelativePathRole: return entry->path;
    case MissingRole: return !entry->directory && entry->file.missing;
    case PlayingRole: return playing;
    case DirectoryRole: return entry->directory;
    case SubtitleAvailableRole: return subtitleAvailable;
    case DurationRole:
        return !entry->directory && !entry->file.missing && m_durations.contains(entry->file.id)
            ? QVariant(m_durations.value(entry->file.id)) : QVariant();
    case Qt::TextAlignmentRole:
        return index.column() == 1 ? QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    case Qt::ForegroundRole:
        if (!entry->directory && entry->file.missing)
            return QBrush(QColor(QStringLiteral("#7E8794")));
        return playing ? QVariant(QBrush(QColor(QStringLiteral("#7FC5B4")))) : QVariant();
    case Qt::BackgroundRole:
        return playing ? QVariant(QBrush(QColor(QStringLiteral("#263E3D")))) : QVariant();
    case Qt::FontRole:
        if (playing) { QFont font; font.setWeight(QFont::DemiBold); return font; }
        return {};
    case Qt::ToolTipRole: {
        const QString status = entry->directory ? QStringLiteral("作品内目录")
            : entry->file.missing ? QStringLiteral("最近一次完整扫描标记为缺失。")
                : QStringLiteral("最近一次完整扫描时存在；未实时检测磁盘。");
        return QStringLiteral("<qt>%1</qt>").arg(
            (entry->name + QLatin1Char('\n') + entry->path + QLatin1Char('\n') + status
                + (subtitleAvailable ? QLatin1Char('\n') + subtitleDescription : QString()))
                .toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
    }
    default: break;
    }
    if (role == Qt::AccessibleDescriptionRole && !entry->directory) {
        const QString status = entry->file.missing ? QStringLiteral("最近一次完整扫描标记为缺失。")
            : QStringLiteral("最近一次完整扫描时存在；未实时检测磁盘。");
        return status + (subtitleAvailable ? QLatin1Char('\n') + subtitleDescription : QString());
    }
    if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole) return {};
    switch (index.column()) {
    case 0: {
        QString name = playing ? QStringLiteral("▶  ") + entry->name : entry->name;
        if (role == Qt::AccessibleTextRole && !entry->directory && entry->file.missing)
            name += QStringLiteral("，缺失");
        if (role == Qt::AccessibleTextRole && subtitleAvailable)
            name += QStringLiteral("，已有关联字幕");
        return name;
    }
    case 1: {
        if (entry->directory) return QString();
        const auto found = m_durations.constFind(entry->file.id);
        if (entry->file.missing || found == m_durations.cend()) return QStringLiteral("--:--");
        const qint64 seconds = found.value() / 1000;
        const QString tail = QStringLiteral("%1:%2").arg(seconds / 60 % 60)
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        return seconds >= 3600 ? QStringLiteral("%1:%2").arg(seconds / 3600)
            .arg(tail, 5, QLatin1Char('0')) : tail;
    }
    case 2: return entry->path;
    default: return {};
    }
}
bool LibraryTrackTreeModel::setFiles(qint64 workId, const QVector<LibraryFile> &files)
{
    QVector<LibraryFile> audio;
    for (auto file : files) {
        if (workId <= 0 || file.workId != workId || file.type != LibraryFileType::Audio) continue;
        file.relativePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const auto parts = file.relativePath.split(QLatin1Char('/'));
        if (std::any_of(parts.cbegin(), parts.cend(), [](const auto &part) {
                return part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..");
            })) continue;
        audio.push_back(std::move(file));
    }
    std::sort(audio.begin(), audio.end(), [](const auto &a, const auto &b) {
        if (a.id != b.id) return a.id < b.id;
        return a.relativePath < b.relativePath;
    });
    if (m_workId == workId && m_files == audio) return false;
    // Preserve a projection only while both the work and the complete file
    // identity are unchanged. Scans explicitly invalidate metadata externally.
    QHash<qint64, qint64> durations;
    QSet<qint64> subtitleTracks;
    if (m_workId == workId) {
        for (const auto &file : audio) {
            const auto *previous = m_byId.value(file.id);
            if (previous && previous->file == file && !file.missing && m_durations.contains(file.id))
                durations.insert(file.id, m_durations.value(file.id));
            if (previous && previous->file == file && !file.missing && m_subtitleTracks.contains(file.id))
                subtitleTracks.insert(file.id);
        }
    }
    // Prepare before reset, so an allocation failure cannot leave a reset open.
    auto root = std::make_unique<Node>(QString(), QString(), nullptr, true);
    QHash<QString, Node *> directories;
    QHash<qint64, Node *> byId;
    directories.insert(QString(), root.get());
    for (const auto &file : audio) {
        const auto parts = file.relativePath.split(QLatin1Char('/'));
        Node *owner = root.get();
        QString path;
        for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
            path = path.isEmpty() ? parts[i] : path + QLatin1Char('/') + parts[i];
            auto *directory = directories.value(path);
            if (!directory) {
                auto child = std::make_unique<Node>(parts[i], path, owner, true);
                directory = child.get();
                owner->children.push_back(std::move(child));
                directories.insert(path, directory);
            }
            owner = directory;
        }
        auto leaf = std::make_unique<Node>(file.fileName, file.relativePath, owner, false);
        leaf->file = file;
        if (file.id > 0) byId.insert(file.id, leaf.get());
        owner->children.push_back(std::move(leaf));
    }
    std::function<void(Node *)> sort = [&](Node *owner) {
        std::sort(owner->children.begin(), owner->children.end(), [](const auto &a, const auto &b) {
            if (a->directory != b->directory) return a->directory;
            if (const int name = a->key.compare(b->key)) return name < 0;
            if (a->path != b->path) return a->path < b->path;
            return a->file.id < b->file.id;
        });
        for (size_t row = 0; row < owner->children.size(); ++row) {
            auto *child = owner->children[row].get();
            child->row = static_cast<int>(row);
            if (child->directory) sort(child);
        }
    };
    sort(root.get());
    directories.remove(QString());
    beginResetModel();
    m_checkedTracks.clear();
    m_batchSelectionEnabled = false;
    m_root = std::move(root);
    m_directories = std::move(directories);
    m_byId = std::move(byId);
    m_files = std::move(audio);
    m_durations = std::move(durations);
    m_subtitleTracks = std::move(subtitleTracks);
    m_workId = workId;
    endResetModel();
    return true;
}
QModelIndex LibraryTrackTreeModel::indexForFileId(qint64 id) const
{
    return nodeIndex(m_byId.value(id));
}
QModelIndex LibraryTrackTreeModel::indexForDirectory(const QString &path) const
{
    return nodeIndex(m_directories.value(path));
}
QModelIndexList LibraryTrackTreeModel::directoryIndexes() const
{
    QModelIndexList result;
    result.reserve(m_directories.size());
    for (const auto *entry : m_directories) result.append(nodeIndex(entry));
    return result;
}
bool LibraryTrackTreeModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    const auto *entry = node(index);
    if (role != Qt::CheckStateRole || !entry || !m_batchSelectionEnabled
        || index.column() != 0 || entry->directory || entry->file.missing || entry->file.id <= 0
        || (value.toInt() != Qt::Checked && value.toInt() != Qt::Unchecked)) return false;
    const bool checked = value.toInt() == Qt::Checked;
    if (m_checkedTracks.contains(entry->file.id) == checked) return true;
    if (checked) m_checkedTracks.insert(entry->file.id);
    else m_checkedTracks.remove(entry->file.id);
    emit dataChanged(index, index, {Qt::CheckStateRole});
    return true;
}
void LibraryTrackTreeModel::setBatchSelectionEnabled(bool enabled)
{
    if (m_batchSelectionEnabled == enabled) return;
    m_batchSelectionEnabled = enabled;
    m_checkedTracks.clear();
    for (const auto *entry : m_byId) {
        if (entry->file.missing || entry->file.id <= 0) continue;
        const auto index = nodeIndex(entry);
        emit dataChanged(index, index, {Qt::CheckStateRole});
    }
}
void LibraryTrackTreeModel::setPlayingTrack(qint64 workId, qint64 fileId)
{
    if (m_playingWorkId == workId && m_playingFileId == fileId) return;
    const auto previous = m_playingWorkId == m_workId ? indexForFileId(m_playingFileId) : QModelIndex();
    m_playingWorkId = workId;
    m_playingFileId = fileId;
    const auto current = workId == m_workId ? indexForFileId(fileId) : QModelIndex();
    for (const auto &index : {previous, current}) {
        if (index.isValid())
            emit dataChanged(index, index.siblingAtColumn(2),
                             {Qt::DisplayRole, Qt::AccessibleTextRole, Qt::ForegroundRole,
                              Qt::BackgroundRole, Qt::FontRole, PlayingRole});
    }
}
void LibraryTrackTreeModel::setTrackDuration(qint64 workId, qint64 fileId,
                                            std::optional<qint64> milliseconds)
{
    const auto index = indexForFileId(fileId);
    if (workId != m_workId || !index.isValid() || index.data(MissingRole).toBool()) return;
    if (milliseconds && *milliseconds < 0) milliseconds.reset();
    const auto previous = m_durations.constFind(fileId);
    if (milliseconds) {
        if (previous != m_durations.cend() && previous.value() == *milliseconds) return;
        m_durations.insert(fileId, *milliseconds);
    } else if (!m_durations.remove(fileId)) return;
    const auto duration = index.siblingAtColumn(1);
    emit dataChanged(duration, duration, {Qt::DisplayRole, Qt::AccessibleTextRole, DurationRole});
}
void LibraryTrackTreeModel::clearWorkDurations(qint64 workId)
{
    if (workId != m_workId) return;
    const auto ids = m_durations.keys();
    m_durations.clear();
    for (const qint64 id : ids) {
        const auto index = indexForFileId(id).siblingAtColumn(1);
        if (index.isValid())
            emit dataChanged(index, index, {Qt::DisplayRole, Qt::AccessibleTextRole, DurationRole});
    }
}
void LibraryTrackTreeModel::setSubtitleTracks(qint64 workId, const QSet<qint64> &audioFileIds)
{
    if (workId != m_workId) return;
    QSet<qint64> present;
    for (const qint64 id : audioFileIds) {
        const auto *entry = m_byId.value(id);
        if (id > 0 && entry && !entry->directory && !entry->file.missing)
            present.insert(id);
    }
    if (present == m_subtitleTracks) return;
    const auto changed = (present - m_subtitleTracks) | (m_subtitleTracks - present);
    m_subtitleTracks = std::move(present);
    for (const qint64 id : changed) {
        const auto index = indexForFileId(id);
        if (index.isValid())
            emit dataChanged(index, index, {SubtitleAvailableRole, Qt::ToolTipRole,
                Qt::AccessibleTextRole, Qt::AccessibleDescriptionRole});
    }
}
} // namespace asmr
