#include "ui/TranslationApiSettingsDialog.h"
#include "ui/TranslationApiSettingsWidget.h"
#include "ui/DialogFrame.h"
#include "ui/PlayerTheme.h"
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
namespace asmr {
TranslationApiSettingsDialog::TranslationApiSettingsDialog(QSettings *settings, QWidget *parent,
    std::shared_ptr<ApiKeyStore> keyStore) : QDialog(parent)
{
    setObjectName(QStringLiteral("translationApiSettingsDialog"));
    setWindowTitle(QStringLiteral("翻译服务设置"));
    setStyleSheet(playerStyleSheet());
    resize(640, 560);
    auto *body = new QVBoxLayout;
    body->setContentsMargins(20, 16, 20, 16);
    body->setSpacing(8);
    m_settingsWidget = new TranslationApiSettingsWidget(settings, this, std::move(keyStore));
    body->addWidget(m_settingsWidget);
    auto *actions = new QHBoxLayout;
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName(QStringLiteral("translationApiClose"));
    actions->addStretch(); actions->addWidget(close); body->addLayout(actions);
    m_frame = new DialogFrame(this, body);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_settingsWidget, &TranslationApiSettingsWidget::shutdownFinished,
            this, &TranslationApiSettingsDialog::shutdownFinished);
}
bool TranslationApiSettingsDialog::isBusy() const { return m_settingsWidget->isBusy(); }
void TranslationApiSettingsDialog::shutdown() { m_settingsWidget->shutdown(); }
void TranslationApiSettingsDialog::closeEvent(QCloseEvent *event) {
    QPointer<TranslationApiSettingsDialog> guard(this);
    m_settingsWidget->deactivate();
    if (guard) QDialog::closeEvent(event);
}
bool TranslationApiSettingsDialog::nativeEvent(const QByteArray &type, void *message, qintptr *result) {
    return m_frame && m_frame->handleNativeEvent(type, message, result)
        ? true : QDialog::nativeEvent(type, message, result);
}
} // namespace asmr
