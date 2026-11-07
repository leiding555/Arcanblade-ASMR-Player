#pragma once
#include "library/LibraryRepository.h"
#include <QAbstractItemModel>
#include <QHash>
#include <memory>
#include <optional>
namespace asmr {
// In-memory projection of an already queried work snapshot. No filesystem/SQL.
class LibraryTrackTreeModel final : public QAbstractItemModel {
public:
    enum DataRole {
        WorkIdRole = Qt::UserRole + 1, FileIdRole, RootPathRole, RelativePathRole,
        MissingRole, PlayingRole, DirectoryRole, DurationRole, SubtitleAvailableRole,
    };
    explicit LibraryTrackTreeModel(QObject *parent = nullptr);
    ~LibraryTrackTreeModel() override;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    // Returns true only when reset was necessary. Input order is immaterial.
    bool setFiles(qint64 workId, const QVector<LibraryFile> &files);
    // Checkboxes are a transient current-page interaction, never persisted.
    void setBatchSelectionEnabled(bool enabled);
    [[nodiscard]] bool batchSelectionEnabled() const { return m_batchSelectionEnabled; }
    void setPlayingTrack(qint64 workId, qint64 fileId);
    void setTrackDuration(qint64 workId, qint64 fileId, std::optional<qint64> milliseconds);
    void clearWorkDurations(qint64 workId);
    void setSubtitleTracks(qint64 workId, const QSet<qint64> &audioFileIds);
    [[nodiscard]] qsizetype trackCount() const { return m_files.size(); }
    [[nodiscard]] QModelIndex indexForFileId(qint64 id) const;
    [[nodiscard]] QModelIndex indexForDirectory(const QString &path) const;
    [[nodiscard]] QModelIndexList directoryIndexes() const;
private:
    struct Node;
    Node *node(const QModelIndex &index) const;
    QModelIndex nodeIndex(const Node *node, int column = 0) const;
    std::unique_ptr<Node> m_root;
    QHash<qint64, Node *> m_byId;
    QHash<QString, Node *> m_directories;
    QVector<LibraryFile> m_files;
    QHash<qint64, qint64> m_durations;
    QSet<qint64> m_subtitleTracks;
    QSet<qint64> m_checkedTracks;
    bool m_batchSelectionEnabled = false;
    qint64 m_workId = 0;
    qint64 m_playingWorkId = 0;
    qint64 m_playingFileId = 0;
};
} // namespace asmr
