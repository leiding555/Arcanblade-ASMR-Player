#include "ui/TranslationApiSettingsWidget.h"
#include "ai/ApiKeyStore.h"
#include "ai/TranslationApiProbe.h"
#include "ui/InputControlPolicy.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPromise>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
namespace asmr {
namespace {
QLabel *wrapped(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}
void clearSecret(QString &value) { value.fill(QChar::Null); value.clear(); }
}
TranslationApiSettingsWidget::TranslationApiSettingsWidget(QSettings *settings, QWidget *parent,
    std::shared_ptr<ApiKeyStore> keyStore)
    : QWidget(parent), m_settings(settings), m_keyStore(std::move(keyStore))
{
    if (!m_keyStore) m_keyStore = std::make_shared<WindowsApiKeyStore>(
        QStringLiteral("Arcanblade/TranslationApi/") + QCoreApplication::organizationName()
            + QLatin1Char('/') + QCoreApplication::applicationName());
    setObjectName(QStringLiteral("translationApiSettingsWidget"));
    InputControlPolicy::install();
    m_probe = new TranslationApiProbe(this);
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);
    auto *form = new QFormLayout;
    m_form = form;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_provider = new QComboBox(this);
    m_provider->setObjectName(QStringLiteral("translationApiProvider"));
    m_provider->addItem(QStringLiteral("DeepSeek"), int(TranslationApiProvider::DeepSeek));
    m_provider->addItem(QStringLiteral("自定义兼容服务"), int(TranslationApiProvider::OpenAiCompatible));
    form->addRow(QStringLiteral("服务"), m_provider);
    m_baseUrl = new QLineEdit(this);
    m_baseUrl->setObjectName(QStringLiteral("translationApiBaseUrl"));
    m_baseUrl->setPlaceholderText(QStringLiteral("https://服务地址/v1"));
    m_baseUrl->setToolTip(QStringLiteral("填写基础地址或完整 /chat/completions 地址。HTTPS；本机服务可用 HTTP。"));
    form->addRow(QStringLiteral("服务地址"), m_baseUrl);
    auto *modelRow = new QWidget(this);
    auto *modelLayout = new QHBoxLayout(modelRow);
    modelLayout->setContentsMargins(0, 0, 0, 0);
    m_deepseekModel = new QComboBox(modelRow);
    m_deepseekModel->setObjectName(QStringLiteral("translationApiDeepSeekModel"));
    for (const auto &id : {QStringLiteral("deepseek-flash"), QStringLiteral("deepseek-v4-pro")})
        m_deepseekModel->addItem(translationApiModelDisplayName(id), id);
    m_model = new QLineEdit(modelRow);
    m_model->setObjectName(QStringLiteral("translationApiModel"));
    m_model->setPlaceholderText(QStringLiteral("模型名称"));
    modelLayout->addWidget(m_deepseekModel);
    modelLayout->addWidget(m_model);
    form->addRow(QStringLiteral("模型"), modelRow);
    m_thinkingHint = wrapped({}, this);
    m_thinkingHint->setObjectName(QStringLiteral("translationApiThinkingHint"));
    form->addRow(QString(), m_thinkingHint);
    m_timeout = new QSpinBox(this);
    m_timeout->setObjectName(QStringLiteral("translationApiTimeout"));
    m_timeout->setRange(1, 600);
    m_timeout->setSuffix(QStringLiteral(" 秒"));
    form->addRow(QStringLiteral("请求超时"), m_timeout);
    m_key = new QLineEdit(this);
    m_key->setObjectName(QStringLiteral("translationApiKey"));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("API Key"), m_key);
    body->addLayout(form);
    m_keyStatus = wrapped({}, this);
    m_keyStatus->setObjectName(QStringLiteral("translationApiKeyStatus"));
    body->addWidget(m_keyStatus);
    auto *keyButtons = new QHBoxLayout;
    m_saveKey = new QPushButton(QStringLiteral("保存 Key"), this);
    m_saveKey->setObjectName(QStringLiteral("translationApiSaveKey"));
    m_removeKey = new QPushButton(QStringLiteral("删除 Key"), this);
    m_removeKey->setObjectName(QStringLiteral("translationApiRemoveKey"));
    keyButtons->addWidget(m_saveKey); keyButtons->addWidget(m_removeKey); keyButtons->addStretch();
    body->addLayout(keyButtons);
    body->addWidget(wrapped(QStringLiteral("有效参数自动保存。API Key 单独保存在 Windows 凭据中，按服务地址隔离。"), this));
    body->addWidget(wrapped(QStringLiteral("连接测试仅发送以下固定短文本，不读取作品、音频或字幕。"), this));
    auto *sample = new QPlainTextEdit(this);
    sample->setObjectName(QStringLiteral("translationApiSample"));
    sample->setReadOnly(true);
    sample->setPlainText(TranslationApiProbe::sampleText());
    sample->setFixedHeight(48);
    body->addWidget(sample);
    body->addWidget(wrapped(QStringLiteral("测试会调用模型，可能产生少量费用；取消后已发出的请求仍可能计费。"), this));
    m_status = wrapped({}, this);
    m_status->setObjectName(QStringLiteral("translationApiStatus"));
    body->addWidget(m_status);
    auto *activity = new QProgressBar(this);
    activity->setObjectName(QStringLiteral("translationApiActivity"));
    activity->setRange(0, 0);
    activity->hide();
    body->addWidget(activity);
    body->addStretch();
    auto *actions = new QHBoxLayout;
    m_test = new QPushButton(QStringLiteral("测试连接"), this);
    m_test->setObjectName(QStringLiteral("translationApiTest"));
    m_cancel = new QPushButton(QStringLiteral("取消测试"), this);
    m_cancel->setObjectName(QStringLiteral("translationApiCancel"));
    actions->addWidget(m_test); actions->addWidget(m_cancel); actions->addStretch();
    body->addLayout(actions);
    connect(m_cancel, &QPushButton::clicked, this, &TranslationApiSettingsWidget::cancelConnectionTest);
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        if (isBusy() || m_shuttingDown) {
            QSignalBlocker blocker(m_provider);
            m_provider->setCurrentIndex(m_provider->findData(int(m_currentProvider)));
            return;
        }
        loadProvider(TranslationApiProvider(m_provider->currentData().toInt()));
        if (m_settings) {
            QString error;
            if (!writeSelectedTranslationApiProvider(*m_settings, m_currentProvider, &error))
                m_status->setText(QStringLiteral("服务选择未能保存，请重试。"));
            else queueSettingsChanged();
        }
    });
    for (auto *edit : {m_baseUrl, m_model}) {
        connect(edit, &QLineEdit::editingFinished, this, &TranslationApiSettingsWidget::settingsEdited);
        connect(edit, &QLineEdit::textChanged, this, [this] {
            if (m_loading) return;
            if (m_preparingTest || m_probe->isBusy()) cancelConnectionTest();
            m_status->clear();
            const auto scope = translationCredentialScope(currentSettings());
            if (scope != m_loadedScope) { m_hasKey = false; m_loadedScope.clear(); m_key->clear(); }
            updateControls();
        });
    }
    connect(m_deepseekModel, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading || m_currentProvider != TranslationApiProvider::DeepSeek) return;
        m_model->setText(m_deepseekModel->currentData().toString());
        settingsEdited();
    });
    connect(m_timeout, &QSpinBox::valueChanged, this, [this] { if (!m_loading) settingsEdited(); });
    connect(m_key, &QLineEdit::textChanged, this, &TranslationApiSettingsWidget::updateControls);
    connect(m_saveKey, &QPushButton::clicked, this, [this] { credentialOperation(CredentialOperation::Write); });
    connect(m_removeKey, &QPushButton::clicked, this, [this] { credentialOperation(CredentialOperation::Remove); });
    connect(m_test, &QPushButton::clicked, this, [this] { credentialOperation(CredentialOperation::ReadForTest); });
    connect(m_probe, &TranslationApiProbe::busyChanged, this, [this, activity](bool busy) {
        activity->setVisible(busy);
        if (busy) m_status->setText(QStringLiteral("正在测试连接…"));
        updateControls();
        finishShutdownIfReady();
    });
    connect(m_probe, &TranslationApiProbe::finished, this, [this](const TranslationApiProbeResult &result) {
        if (!m_shuttingDown) {
            QString text = result.message;
            if (result.state == TranslationApiProbeState::Succeeded) {
                text += QStringLiteral(" 用时 %1 秒。").arg(result.elapsedMs / 1000.0, 0, 'f', 1);
                if (result.promptTokens >= 0 && result.completionTokens >= 0)
                    text += QStringLiteral(" 输入 %1 / 输出 %2 tokens。").arg(result.promptTokens).arg(result.completionTokens);
            }
            m_status->setText(text);
        }
        updateControls();
        finishShutdownIfReady();
    });
    loadProvider(m_settings ? readSelectedTranslationApiProvider(*m_settings) : TranslationApiProvider::DeepSeek);
}
bool TranslationApiSettingsWidget::isBusy() const { return m_credentialWatcher || m_probe->isBusy(); }
TranslationApiSettings TranslationApiSettingsWidget::currentSettings() const {
    const auto saved = m_settings ? readTranslationApiSettings(*m_settings, m_currentProvider)
                                  : defaultTranslationApiSettings(m_currentProvider);
    return {m_currentProvider, m_baseUrl->text(),
        m_currentProvider == TranslationApiProvider::DeepSeek ? m_deepseekModel->currentData().toString() : m_model->text(),
        m_timeout->value(), translationApiThinkingEnabled(saved)};
}
void TranslationApiSettingsWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    updateControls();
}
void TranslationApiSettingsWidget::loadProvider(TranslationApiProvider provider) {
    m_loading = true;
    m_currentProvider = provider;
    m_provider->setCurrentIndex(m_provider->findData(int(provider)));
    const auto value = m_settings ? readTranslationApiSettings(*m_settings, provider) : defaultTranslationApiSettings(provider);
    m_baseUrl->setText(value.baseUrl);
    m_baseUrl->setReadOnly(provider == TranslationApiProvider::DeepSeek);
    m_model->setText(value.model);
    int modelIndex = m_deepseekModel->findData(value.model);
    // Keep a previously saved official model visible instead of silently replacing it.
    if (provider == TranslationApiProvider::DeepSeek && modelIndex < 0) {
        m_deepseekModel->addItem(translationApiModelDisplayName(value.model), value.model);
        modelIndex = m_deepseekModel->count() - 1;
    }
    if (modelIndex >= 0) m_deepseekModel->setCurrentIndex(modelIndex);
    m_timeout->setValue(value.timeoutSeconds);
    m_key->clear(); m_hasKey = false; m_loadedScope.clear(); m_status->clear();
    m_loading = false;
    updateControls();
    if (validateTranslationApiSettings(currentSettings()).isEmpty()) credentialOperation(CredentialOperation::Read);
}
bool TranslationApiSettingsWidget::saveSettings() {
    const auto value = currentSettings();
    const auto error = validateTranslationApiSettings(value);
    if (!error.isEmpty()) { m_status->setText(error); return false; }
    const auto before = m_settings ? readTranslationApiSettings(*m_settings, value.provider) : value;
    const bool changed = before.baseUrl != value.baseUrl || before.model != value.model
        || before.timeoutSeconds != value.timeoutSeconds || before.thinkingEnabled != value.thinkingEnabled;
    QString saveError;
    if (m_settings && !writeTranslationApiSettings(*m_settings, value, &saveError)) {
        m_status->setText(QStringLiteral("设置未能保存，请重试。")); return false;
    }
    if (changed) queueSettingsChanged();
    return true;
}
void TranslationApiSettingsWidget::settingsEdited() {
    if (m_loading || isBusy() || m_shuttingDown) return;
    if (!saveSettings()) { updateControls(); return; }
    if (translationCredentialScope(currentSettings()) != m_loadedScope) credentialOperation(CredentialOperation::Read);
    else updateControls();
}
void TranslationApiSettingsWidget::credentialOperation(CredentialOperation operation) {
    if (isBusy() || m_shuttingDown || !saveSettings()) return;
    const auto value = currentSettings();
    const auto scope = translationCredentialScope(value);
    QString key = operation == CredentialOperation::Write ? m_key->text() : QString();
    if (operation == CredentialOperation::Write && !validateTranslationApiKey(key).isEmpty()) {
        m_status->setText(QStringLiteral("请输入有效的 API Key，再点击保存。")); return;
    }
    auto *watcher = new QFutureWatcher<CredentialResult>(this);
    m_credentialWatcher = watcher;
    auto promise = std::make_shared<QPromise<CredentialResult>>();
    const auto store = m_keyStore;
    const auto testGeneration = operation == CredentialOperation::ReadForTest ? ++m_testGeneration : m_testGeneration;
    m_preparingTest = operation == CredentialOperation::ReadForTest;
    if (m_preparingTest) m_status->setText(QStringLiteral("正在准备连接测试…"));
    connect(watcher, &QFutureWatcher<CredentialResult>::finished, this, [this, watcher, operation, value, scope, testGeneration] {
        auto result = watcher->result();
        m_credentialWatcher = nullptr;
        watcher->deleteLater();
        const bool current = translationCredentialScope(currentSettings()) == scope;
        const bool activeIntent = operation != CredentialOperation::ReadForTest
            || (testGeneration == m_testGeneration && m_preparingTest);
        if (operation == CredentialOperation::ReadForTest && testGeneration == m_testGeneration) m_preparingTest = false;
        if (!m_shuttingDown && current && activeIntent) {
            if (result.ok) {
                m_loadedScope = scope; m_hasKey = result.found;
                if (operation == CredentialOperation::Write) {
                    m_key->clear(); m_status->setText(QStringLiteral("API Key 已安全保存。"));
                } else if (operation == CredentialOperation::Remove) {
                    m_key->clear(); m_status->setText(QStringLiteral("当前服务的 API Key 已删除。"));
                }
                if (operation == CredentialOperation::ReadForTest) {
                    if (result.found) {
                        QPointer<TranslationApiSettingsWidget> guard(this);
                        m_probe->start(value, result.key);
                        if (!guard) { clearSecret(result.key); return; }
                    } else m_status->setText(QStringLiteral("当前服务尚未保存 API Key。"));
                }
            } else {
                if (operation == CredentialOperation::Read) m_hasKey = false;
                m_status->setText(QStringLiteral("Windows 安全凭据操作失败，请重试；未使用明文保存。"));
            }
        }
        clearSecret(result.key);
        updateControls(); finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, store, operation, scope, key = std::move(key)]() mutable {
        promise->start(); CredentialResult result;
        try {
            QString error;
            if (operation == CredentialOperation::Write) {
                result.ok = store->write(scope, key, &error); result.found = result.ok;
            } else if (operation == CredentialOperation::Remove) result.ok = store->remove(scope, &error);
            else {
                result.ok = store->read(scope, &result.key, &error);
                result.found = result.ok && !result.key.isEmpty();
                if (operation != CredentialOperation::ReadForTest) clearSecret(result.key);
            }
        } catch (...) { result = {}; }
        clearSecret(key);
        promise->addResult(std::move(result)); promise->finish();
    });
    updateControls();
}
void TranslationApiSettingsWidget::updateControls() {
    const bool busy = isBusy();
    const bool editable = !busy && !m_shuttingDown;
    for (auto *widget : {static_cast<QWidget *>(m_provider), static_cast<QWidget *>(m_baseUrl),
             static_cast<QWidget *>(m_model), static_cast<QWidget *>(m_deepseekModel),
             static_cast<QWidget *>(m_timeout), static_cast<QWidget *>(m_key)})
        widget->setEnabled(editable);
    const auto value = currentSettings();
    const bool official = value.provider == TranslationApiProvider::DeepSeek;
    m_form->setRowVisible(m_baseUrl, !official);
    m_model->setVisible(!official);
    m_deepseekModel->setVisible(official);
    const bool thinking = translationApiThinkingEnabled(value);
    m_thinkingHint->setText(QStringLiteral("思考模式请求超时至少 180 秒（当前 %1 秒）。")
        .arg(translationApiRequestTimeoutSeconds(value)));
    m_form->setRowVisible(m_thinkingHint, thinking);
    const bool valid = validateTranslationApiSettings(value).isEmpty();
    const bool saved = valid && translationCredentialScope(value) == m_loadedScope && m_hasKey;
    m_saveKey->setEnabled(editable && valid && validateTranslationApiKey(m_key->text()).isEmpty());
    m_removeKey->setEnabled(editable && saved);
    m_test->setEnabled(editable && saved && m_key->text().isEmpty());
    const bool testing = m_preparingTest || m_probe->isBusy();
    m_cancel->setVisible(testing);
    m_cancel->setEnabled(!m_shuttingDown && testing);
    findChild<QProgressBar *>(QStringLiteral("translationApiActivity"))->setVisible(testing);
    m_key->setPlaceholderText(saved ? QStringLiteral("已保存；输入新 Key 可替换") : QStringLiteral("输入后点击“保存 Key”"));
    m_keyStatus->setText(m_credentialWatcher ? QStringLiteral("正在处理安全凭据…")
        : saved ? QStringLiteral("当前服务已安全保存 API Key") : QStringLiteral("当前服务未保存 API Key"));
    m_status->setVisible(!m_status->text().isEmpty());
    if (busy != m_lastBusy) {
        m_lastBusy = busy;
        QTimer::singleShot(0, this, [this] { emit busyChanged(isBusy()); });
    }
}
void TranslationApiSettingsWidget::cancelConnectionTest() {
    ++m_testGeneration;
    const bool preparing = m_preparingTest;
    m_preparingTest = false;
    QPointer<TranslationApiSettingsWidget> guard(this);
    m_probe->cancel();
    if (!guard) return;
    if (preparing && !m_shuttingDown) m_status->setText(QStringLiteral("已取消连接测试。"));
    updateControls(); finishShutdownIfReady();
}
void TranslationApiSettingsWidget::deactivate() {
    QPointer<TranslationApiSettingsWidget> guard(this);
    cancelConnectionTest();
    if (guard) m_key->clear();
}
void TranslationApiSettingsWidget::shutdown() {
    m_shuttingDown = true; m_key->clear(); cancelConnectionTest();
}
void TranslationApiSettingsWidget::finishShutdownIfReady() {
    if (m_shuttingDown && !isBusy() && !m_shutdownEmitted) {
        m_shutdownEmitted = true; emit shutdownFinished();
    }
}
void TranslationApiSettingsWidget::queueSettingsChanged() {
    if (m_settingsNotificationQueued) return;
    m_settingsNotificationQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_settingsNotificationQueued = false;
        emit settingsChanged();
    });
}
} // namespace asmr
