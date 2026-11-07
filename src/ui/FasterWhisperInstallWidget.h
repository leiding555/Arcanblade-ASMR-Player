#pragma once

#include "ai/FasterWhisperInstallService.h"
#include "ai/FasterWhisperGenerationOptions.h"
#include <QPointer>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QFileDialog;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSettings;
class QShowEvent;
class QTimer;

namespace asmr {

// Shared XXL settings and component preparation. Existing directories stay read-only.
// Hiding keeps preparation running; shutdown cancels and drains its workers.
class FasterWhisperInstallWidget final : public QWidget {
    Q_OBJECT
public:
    using Validator = std::function<QString(const QString &, const std::atomic_bool &)>;
    explicit FasterWhisperInstallWidget(QSettings *settings, QWidget *parent = nullptr,
        FasterWhisperInstallService *installer = nullptr,
        Validator engineValidator = {}, Validator modelValidator = {});
    ~FasterWhisperInstallWidget() override;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool installationBusy() const;
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QString checkedEngineDirectory() const;
    [[nodiscard]] QString checkedModelDirectory() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString validationState() const;
    [[nodiscard]] bool hasOpenPicker() const { return !m_picker.isNull(); }
    [[nodiscard]] AiSubtitleComputeMode computeMode() const;
    [[nodiscard]] FasterWhisperGenerationOptions generationOptions() const;
    void recheck();
    void shutdown();
    void closePicker();
signals:
    void activityChanged();
    void shutdownFinished();
protected:
    void showEvent(QShowEvent *event) override;
private:
    void scheduleCheck();
    void startCheck();
    void cancelCheck();
    void browse(QLineEdit *target, const QString &title);
    bool persistRoot();
    void persistCheckedPaths();
    void startInstallation();
    void persistGenerationOptions();
    void setGenerationOptions(const FasterWhisperGenerationOptions &options);
    void cancel();
    void applyResult(const FasterWhisperInstallResult &result);
    void updatePresentation();
    void finishShutdownIfReady();
    QPointer<QSettings> m_settings;
    QPointer<FasterWhisperInstallService> m_installer;
    QPointer<QFileDialog> m_picker;
    Validator m_engineValidator;
    Validator m_modelValidator;
    bool m_ownsInstaller = false;
    bool m_loading = true;
    bool m_engineDirty = false;
    bool m_modelDirty = false;
    bool m_shuttingDown = false;
    bool m_shutdownStarting = false;
    bool m_shutdownEmitted = false;
    bool m_cancelRequested = false;
    bool m_checking = false;
    bool m_checkStarted = false;
    quint64 m_generation = 0;
    int m_validationWorkers = 0;
    std::shared_ptr<std::atomic_bool> m_checkCancelled;
    QString m_checkedEngine;
    QString m_checkedModel;
    QString m_engineError;
    QString m_modelError;
    QString m_feedback;
    QString m_saveError;
    QLineEdit *m_engine = nullptr;
    QString m_modelDirectory;
    QString m_cachedModel;
    QString m_cachedModelIdentity;
    QLineEdit *m_root = nullptr;
    QLabel *m_engineStatus = nullptr;
    QLabel *m_engineIcon = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_bytes = nullptr;
    QLabel *m_saveStatus = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_browseEngine = nullptr;
    QPushButton *m_browseRoot = nullptr;
    QPushButton *m_check = nullptr;
    QPushButton *m_prepare = nullptr;
    QPushButton *m_cancel = nullptr;
    QWidget *m_installSection = nullptr;
    QWidget *m_customParameters = nullptr;
    QPushButton *m_installToggle = nullptr;
    QComboBox *m_compute = nullptr;
    QComboBox *m_preset = nullptr;
    QLabel *m_presetHint = nullptr;
    QCheckBox *m_subtitleFormatting = nullptr;
    QCheckBox *m_vad = nullptr;
    QDoubleSpinBox *m_threshold = nullptr;
    QSpinBox *m_beam = nullptr;
    QCheckBox *m_previousText = nullptr;
    QPushButton *m_reset = nullptr;
    QTimer *m_checkTimer = nullptr;
    QTimer *m_saveTimer = nullptr;
};
} // namespace asmr
