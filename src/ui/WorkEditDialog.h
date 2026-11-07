#pragma once

#include "artwork/CoverArtLoader.h"
#include "artwork/ManagedCoverFile.h"

#include <QDialog>
#include <QFuture>
#include <QPointer>

#include <functional>
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
class WorkEditDialogTests;

// Draft-only UI. The owner commits a validated title and cover snapshot together;
// browsing, previewing and rejecting this dialog never write user or app files.
class WorkEditDialog final : public QDialog {
    Q_OBJECT
public:
    enum class CoverChange { Keep, Replace, Automatic };

    explicit WorkEditDialog(const QString &title, const QString &rootPath,
                            QWidget *parent = nullptr);
    ~WorkEditDialog() override;

    void setCoverState(const CoverArtResult &result, bool hasManual);
    void setSaving(bool saving);
    void setFeedback(const QString &message);
    // Also used after filesSelected: all paths follow the same asynchronous
    // validation and immutable snapshot pipeline, including external files.
    void selectLocalFile(const QString &path);
    void shutdown();
    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] CoverChange coverChange() const { return m_coverChange; }
    [[nodiscard]] const ManagedCoverDraft &draft() const { return m_draft; }

signals:
    void saveRequested();
    void shutdownFinished();

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void closeEvent(QCloseEvent *event) override;
    void done(int result) override;

private:
    friend class WorkEditDialogTests;
    struct PreviewTask { quint64 id; QString path; bool automatic = false; };
    struct PreviewResult { ManagedCoverDraft draft; CoverArtResult automatic; };
    void chooseLocalFile();
    void restoreAutomatic();
    void requestPreview(PreviewTask task);
    void startPreview(PreviewTask task);
    void updateControls();
    void requestSave();
    void finishShutdownIfReady();
    [[nodiscard]] bool validTitle() const;

    QString m_originalTitle;
    QString m_defaultTitle;
    QString m_rootPath;
    bool m_initialized = false;
    bool m_hasManual = false;
    bool m_active = false;
    bool m_loading = false;
    bool m_saving = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    bool m_useNativeFileDialog = true;
    quint64 m_generation = 0;
    CoverChange m_coverChange = CoverChange::Keep;
    ManagedCoverDraft m_draft;
    std::optional<PreviewTask> m_pending;
    std::optional<int> m_requestedResult;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFuture<PreviewResult> m_lifetime;
    std::shared_ptr<const std::function<void()>> m_beforePreviewTestHook;
    QPointer<QFileDialog> m_fileDialog;
    TitleBarWidget *m_titleBar = nullptr;
    WindowFrameController *m_windowFrame = nullptr;
    QLineEdit *m_titleEdit = nullptr;
    CoverArtWidget *m_preview = nullptr;
    QLabel *m_feedbackLabel = nullptr;
    QPushButton *m_chooseButton = nullptr;
    QPushButton *m_autoButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
};
} // namespace asmr
