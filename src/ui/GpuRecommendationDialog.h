#pragma once

#include "ai/GpuCompatibility.h"

#include <QDialog>
#include <QPointer>

#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QFileDialog;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSettings;
class QShowEvent;
class QTimer;

namespace asmr {

class ChickenRiceInstallService;
class DialogFrame;

// Owns UI drafts and an asynchronous, value-only GPU probe. A normal close
// leaves installation running; shutdown cancels and drains both operations.
// The caller owns settings and any injected installer.
class GpuRecommendationDialog final : public QDialog
{
    Q_OBJECT
public:
    using Probe = std::function<GpuDetectionResult(const std::atomic_bool &)>;

    explicit GpuRecommendationDialog(QSettings *settings, QWidget *parent = nullptr,
                                     Probe probe = {},
                                     ChickenRiceInstallService *installer = nullptr,
                                     bool embedded = false);
    ~GpuRecommendationDialog() override;

    void startDetection();
    void shutdown();
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool installationBusy() const;
    [[nodiscard]] bool hasOpenPicker() const { return !m_installFolderDialog.isNull(); }

public slots:
    void done(int result) override;

signals:
    void shutdownFinished();
    void engineReady(QString engineDirectory);
    void activityChanged();

protected:
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void restoreSelection();
    bool persistSelection(bool requireComplete = true);
    void flushPendingSelection();
    void cancelDetection();
    void applyDetection(const GpuDetectionResult &result);
    void updatePresentation();
    void updateInstallPresentation();
    [[nodiscard]] std::optional<ChickenRiceVariant> selectedInstallVariant() const;
    void browseInstallRoot();
    void startInstallation();
    void cancelInstallation();
    void finishShutdownIfReady();

    QPointer<QSettings> m_settings;
    Probe m_probe;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QList<GpuDevice> m_devices;
    quint64 m_requestGeneration = 0;
    bool m_busy = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    QPointer<ChickenRiceInstallService> m_installer;
    QPointer<QFileDialog> m_installFolderDialog;
    bool m_ownsInstaller = false;
    bool m_embedded = false;
    bool m_installCancelRequested = false;
    QString m_installFeedback;
    QString m_selectionFeedback;
    QTimer *m_autoSaveTimer = nullptr;
    std::optional<ChickenRiceVariant> m_startedInstallVariant;
    QString m_startedInstallRoot;
    QComboBox *m_deviceCombo = nullptr;
    QComboBox *m_selectionMode = nullptr;
    QComboBox *m_manualVariant = nullptr;
    QLabel *m_manualVariantLabel = nullptr;
    QLabel *m_detectionStatus = nullptr;
    QLabel *m_deviceInformation = nullptr;
    QLabel *m_recommendation = nullptr;
    QLabel *m_explanation = nullptr;
    QLineEdit *m_installRoot = nullptr;
    QPushButton *m_browseInstallRootButton = nullptr;
    QLabel *m_packageSize = nullptr;
    QLabel *m_installStatus = nullptr;
    QLabel *m_installBytes = nullptr;
    QLabel *m_installBackgroundHint = nullptr;
    QProgressBar *m_installProgress = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_cancelInstallButton = nullptr;
    QPushButton *m_detectButton = nullptr;
    DialogFrame *m_dialogFrame = nullptr;
};

} // namespace asmr
