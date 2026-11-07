#pragma once
#include <QDialog>
#include <memory>
class QSettings;
class QCloseEvent;
namespace asmr {
class ApiKeyStore;
class DialogFrame;
class TranslationApiSettingsWidget;
// Compatibility host for existing standalone safety tests. Production embeds
// TranslationApiSettingsWidget in the AI subtitle window.
class TranslationApiSettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TranslationApiSettingsDialog(QSettings *settings, QWidget *parent = nullptr,
        std::shared_ptr<ApiKeyStore> keyStore = {});
    [[nodiscard]] bool isBusy() const;
    void shutdown();
signals:
    void shutdownFinished();
protected:
    void closeEvent(QCloseEvent *event) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;
private:
    TranslationApiSettingsWidget *m_settingsWidget = nullptr;
    DialogFrame *m_frame = nullptr;
};
} // namespace asmr
