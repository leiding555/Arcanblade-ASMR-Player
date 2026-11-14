#pragma once

#include "artwork/ManagedCoverFile.h"
#include "artwork/CoverArtLoader.h"
#include "library/LibraryRepository.h"

#include <QDialog>
#include <QFuture>
#include <QPointer>
#include <optional>

class QFileDialog;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPushButton;

namespace asmr {
class CoverArtWidget;
class TitleBarWidget;
class WindowFrameController;

// Preview-only editor. The owner commits the name and immutable cover draft
// together; opening, choosing and cancelling do not change stored artwork.
class CategoryEditDialog final : public QDialog {
    Q_OBJECT
public:
    enum class CoverChange { Keep, Replace, Automatic };
    CategoryEditDialog(const WorkCategory &category, const ManualCoverSettings &settings,
        const QVector<LibraryWork> &orderedMembers, QWidget *parent = nullptr);
    ~CategoryEditDialog() override;
    [[nodiscard]] QString name() const;
    [[nodiscard]] CoverChange coverChange() const { return m_coverChange; }
    [[nodiscard]] const ManagedCoverDraft &draft() const { return m_draft; }
    [[nodiscard]] bool isIdle() const { return !m_active; }
    void selectLocalFile(const QString &path);
    void setSaving(bool saving);
    void setFeedback(const QString &message);
    void shutdown();

signals:
    void saveRequested();
    void shutdownFinished();

protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;
    void closeEvent(QCloseEvent *event) override;
    void done(int result) override;

private:
    struct PreviewResult { CoverArtResult cover; ManagedCoverDraft draft; };
    enum class PreviewKind { Initial, Automatic, External };
    void chooseLocalFile();
    void startPreview(PreviewKind kind, const QString &path = {});
    void updateControls();
    void finishShutdownIfReady();
    WorkCategory m_category;
    ManualCoverSettings m_settings;
    QVector<LibraryWork> m_members;
    CoverChange m_coverChange = CoverChange::Keep;
    ManagedCoverDraft m_draft;
    bool m_active = false;
    bool m_saving = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    std::optional<int> m_requestedResult;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFuture<PreviewResult> m_lifetime;
    QPointer<QFileDialog> m_fileDialog;
    TitleBarWidget *m_titleBar = nullptr;
    WindowFrameController *m_frame = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_feedback = nullptr;
    CoverArtWidget *m_preview = nullptr;
    QPushButton *m_choose = nullptr;
    QPushButton *m_automatic = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_cancel = nullptr;
};
} // namespace asmr
