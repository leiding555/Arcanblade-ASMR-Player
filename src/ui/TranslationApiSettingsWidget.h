#pragma once
#include "ai/TranslationApiSettings.h"
#include <QPointer>
#include <QWidget>
#include <memory>
class QSettings;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;
class QShowEvent;
template<class T> class QFutureWatcher;
namespace asmr {
class ApiKeyStore;
class TranslationApiProbe;
// Settings content shared by the AI subtitle window and the legacy test host.
// Credential workers capture no widgets; hiding the page never starts a request.
class TranslationApiSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TranslationApiSettingsWidget(QSettings *settings, QWidget *parent = nullptr,
        std::shared_ptr<ApiKeyStore> keyStore = {});
    [[nodiscard]] bool isBusy() const;
    void deactivate();
    void shutdown();
signals:
    void settingsChanged();
    void busyChanged(bool busy);
    void shutdownFinished();
protected:
    void showEvent(QShowEvent *event) override;
private:
    enum class CredentialOperation { Read, Write, Remove, ReadForTest };
    struct CredentialResult { bool ok = false; bool found = false; QString key; };
    [[nodiscard]] TranslationApiSettings currentSettings() const;
    void loadProvider(TranslationApiProvider provider);
    void settingsEdited();
    bool saveSettings();
    void credentialOperation(CredentialOperation operation);
    void updateControls();
    void cancelConnectionTest();
    void finishShutdownIfReady();
    void queueSettingsChanged();
    QPointer<QSettings> m_settings;
    std::shared_ptr<ApiKeyStore> m_keyStore;
    TranslationApiProbe *m_probe = nullptr;
    QFutureWatcher<CredentialResult> *m_credentialWatcher = nullptr;
    QFormLayout *m_form = nullptr;
    QComboBox *m_provider = nullptr;
    QLineEdit *m_baseUrl = nullptr;
    QLineEdit *m_model = nullptr;
    QComboBox *m_deepseekModel = nullptr;
    QLabel *m_thinkingHint = nullptr;
    QSpinBox *m_timeout = nullptr;
    QLineEdit *m_key = nullptr;
    QLabel *m_keyStatus = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_saveKey = nullptr;
    QPushButton *m_removeKey = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_cancel = nullptr;
    bool m_loading = false;
    bool m_preparingTest = false;
    quint64 m_testGeneration = 0;
    bool m_hasKey = false;
    bool m_shuttingDown = false;
    bool m_shutdownEmitted = false;
    bool m_lastBusy = false;
    bool m_settingsNotificationQueued = false;
    TranslationApiProvider m_currentProvider = TranslationApiProvider::DeepSeek;
    QString m_loadedScope;
};
} // namespace asmr
