#include "ui/GpuRecommendationDialog.h"

#include "ai/GpuDetector.h"
#include "ai/ChickenRiceInstallService.h"
#include "ai/ChickenRicePackage.h"
#include "ui/DialogFrame.h"
#include "ui/PlayerTheme.h"

#include <QComboBox>
#include <QFormLayout>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QProgressBar>
#include <QStandardPaths>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPromise>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

namespace asmr {
namespace {
const QString SettingsPrefix = QStringLiteral("Ai/GpuRecommendation/");
const QString InstallRootKey = QStringLiteral("Ai/ChickenRice/installRoot");

QString normalizedInstallRoot(const QString &path)
{
    const QString value = QDir::fromNativeSeparators(path.trimmed());
    if (value.isEmpty() || value.contains(QChar::Null) || !QDir::isAbsolutePath(value)
        || value.startsWith(QStringLiteral("//?/")) || value.startsWith(QStringLiteral("//./")))
        return {};
    const QString clean = QDir::cleanPath(value);
    return QDir(clean).isRoot() ? QString() : clean;
}

QString defaultInstallRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Ai/ChickenRice"));
}

QString byteDescription(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QStringLiteral("%1 GiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    if (bytes >= 1024LL * 1024)
        return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 字节").arg(std::max<qint64>(0, bytes));
}

QLabel *plainLabel(const QString &objectName, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QString memoryDescription(quint64 bytes)
{
    if (bytes == 0)
        return QStringLiteral("独立显存未报告");
    return QStringLiteral("独立显存 %1 GiB").arg(
        static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}
} // namespace

GpuRecommendationDialog::GpuRecommendationDialog(
    QSettings *settings, QWidget *parent, Probe probe, ChickenRiceInstallService *installer, bool embedded)
    : QDialog(parent), m_settings(settings), m_probe(std::move(probe)), m_installer(installer),
      m_embedded(embedded)
{
    if (!m_probe)
        m_probe = [](const std::atomic_bool &cancelled) { return GpuDetector::detect(cancelled); };
    setObjectName(QStringLiteral("gpuRecommendationDialog"));
    if (m_embedded) {
        setWindowFlags(Qt::Widget);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    }
    if (!m_installer) {
        m_installer = new ChickenRiceInstallService(this);
        m_ownsInstaller = true;
    }
    setWindowTitle(QStringLiteral("鸡米饭 v1.10 下载准备"));
    setWindowModality(Qt::NonModal);
    setStyleSheet(playerStyleSheet());
    if (!m_embedded)
        resize(600, 620);

    m_detectionStatus = plainLabel(QStringLiteral("gpuDetectionStatus"), this);
    m_detectionStatus->setText(QStringLiteral("尚未检测 GPU。"));
    m_detectionStatus->setVisible(!m_embedded);
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setObjectName(QStringLiteral("gpuDeviceCombo"));
    m_deviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_deviceCombo->setMinimumContentsLength(20);
    m_deviceInformation = plainLabel(QStringLiteral("gpuDeviceInformation"), this);
    m_selectionMode = new QComboBox(this);
    m_selectionMode->setObjectName(QStringLiteral("gpuSelectionMode"));
    m_selectionMode->addItem(QStringLiteral("自动推荐"), QStringLiteral("auto"));
    m_selectionMode->addItem(QStringLiteral("手动选择"), QStringLiteral("manual"));
    m_manualVariant = new QComboBox(this);
    m_manualVariant->setObjectName(QStringLiteral("gpuManualVariant"));
    m_manualVariant->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_manualVariant->setMinimumContentsLength(20);
    for (const auto variant : allChickenRiceVariants()) {
        m_manualVariant->addItem(chickenRiceVariantDisplayName(variant) + QStringLiteral("（未验证）"),
                                 chickenRiceVariantKey(variant));
        m_manualVariant->setItemData(m_manualVariant->count() - 1,
            chickenRiceArchiveName(variant), Qt::ToolTipRole);
    }
    m_recommendation = plainLabel(QStringLiteral("gpuRecommendation"), this);
    m_recommendation->setProperty("sectionTitle", true);
    m_explanation = plainLabel(QStringLiteral("gpuExplanation"), this);
    m_explanation->setProperty("secondaryText", true);

    m_installRoot = new QLineEdit(this);
    m_installRoot->setObjectName(QStringLiteral("gpuInstallRoot"));
    m_installRoot->setMinimumWidth(0);
    m_browseInstallRootButton = new QPushButton(QStringLiteral("浏览…"), this);
    m_browseInstallRootButton->setObjectName(QStringLiteral("gpuBrowseInstallRootButton"));
    auto *rootRow = new QHBoxLayout;
    rootRow->addWidget(m_installRoot, 1);
    rootRow->addWidget(m_browseInstallRootButton);
    m_packageSize = plainLabel(QStringLiteral("gpuPackageSize"), this);
    m_installStatus = plainLabel(QStringLiteral("gpuInstallStatus"), this);
    m_installBytes = plainLabel(QStringLiteral("gpuInstallBytes"), this);
    m_installProgress = new QProgressBar(this);
    m_installProgress->setObjectName(QStringLiteral("gpuInstallProgress"));
    m_installProgress->setRange(0, 1000);
    m_installProgress->setValue(0);
    m_installBackgroundHint = plainLabel(QStringLiteral("gpuInstallBackgroundHint"), this);
    m_installBackgroundHint->setProperty("secondaryText", true);
    m_installBackgroundHint->setText(QStringLiteral("关闭窗口后后台继续，退出程序暂停下载。"));
    m_installButton = new QPushButton(QStringLiteral("下载并准备"), this);
    m_installButton->setObjectName(QStringLiteral("gpuInstallButton"));
    m_installButton->setProperty("primaryAction", true);
    m_cancelInstallButton = new QPushButton(QStringLiteral("取消下载"), this);
    m_cancelInstallButton->setObjectName(QStringLiteral("gpuCancelInstallButton"));
    auto *installForm = new QFormLayout;
    installForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    installForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    installForm->addRow(QStringLiteral("下载安装位置"), rootRow);
    if (!m_embedded) {
        auto *rootHint = plainLabel(QStringLiteral("gpuInstallRootHint"), this);
        rootHint->setProperty("secondaryText", true);
        rootHint->setText(QStringLiteral("仅用于新组件安装。"));
        installForm->addRow(QString(), rootHint);
    }

    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("显示适配器"), m_deviceCombo);
    form->addRow(QStringLiteral("选择方式"), m_selectionMode);
    m_manualVariantLabel = new QLabel(QStringLiteral("手动版本"), this);
    m_manualVariantLabel->setObjectName(QStringLiteral("gpuManualVariantLabel"));
    form->addRow(m_manualVariantLabel, m_manualVariant);

    m_detectButton = new QPushButton(QStringLiteral("重新检测"), this);
    m_detectButton->setObjectName(QStringLiteral("gpuDetectButton"));
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setObjectName(QStringLiteral("gpuCloseButton"));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_detectButton);
    buttons->addStretch();
    buttons->addWidget(m_cancelInstallButton);
    buttons->addWidget(m_installButton);

    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(m_embedded ? 0 : 18, m_embedded ? 0 : 16,
                               m_embedded ? 0 : 18, m_embedded ? 0 : 16);
    layout->setSpacing(12);
    layout->setAlignment(Qt::AlignTop);
    QWidget *installationArea = this;
    QVBoxLayout *installationLayout = layout;
    if (m_embedded) {
        installationArea = new QWidget(this);
        installationArea->setObjectName(QStringLiteral("gpuAdvancedSettings"));
        installationLayout = new QVBoxLayout(installationArea);
        installationLayout->setContentsMargins(0, 0, 0, 0);
        installationLayout->setSpacing(12);
        installationLayout->setAlignment(Qt::AlignTop);
        auto *toggle = new QPushButton(QStringLiteral("安装组件"), this);
        toggle->setObjectName(QStringLiteral("gpuAdvancedButton"));
        toggle->setCheckable(true);
        toggle->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        toggle->setToolTip(QStringLiteral("显示或收起组件下载与安装设置。"));
        installationArea->hide();
        connect(toggle, &QPushButton::toggled, this, [this, installationArea](bool expanded) {
            installationArea->setVisible(expanded);
            this->layout()->invalidate();
            updateGeometry();
            emit activityChanged();
        });
        layout->addWidget(toggle, 0, Qt::AlignLeft);
        layout->addWidget(installationArea);
        closeButton->hide();
    }
    installationLayout->addWidget(m_recommendation);
    installationLayout->addWidget(m_packageSize);
    installationLayout->addWidget(m_detectionStatus);
    installationLayout->addLayout(form);
    installationLayout->addWidget(m_deviceInformation);
    installationLayout->addWidget(m_explanation);
    installationLayout->addLayout(installForm);
    installationLayout->addWidget(m_installStatus);
    installationLayout->addWidget(m_installProgress);
    installationLayout->addWidget(m_installBytes);
    installationLayout->addWidget(m_installBackgroundHint);
    installationLayout->addLayout(buttons);
    if (m_embedded) {
        setLayout(layout);
    } else {
        layout->addStretch();
        auto *closeRow = new QHBoxLayout;
        closeRow->addStretch();
        closeRow->addWidget(closeButton);
        layout->addLayout(closeRow);
        m_dialogFrame = new DialogFrame(this, layout);
    }

    connect(m_deviceCombo, &QComboBox::currentIndexChanged,
            this, [this] { updatePresentation(); });
    connect(m_selectionMode, &QComboBox::currentIndexChanged,
            this, [this] { persistSelection(false); updatePresentation(); });
    connect(m_manualVariant, &QComboBox::currentIndexChanged,
            this, [this] { persistSelection(false); updatePresentation(); });
    connect(m_detectButton, &QPushButton::clicked, this, &GpuRecommendationDialog::startDetection);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_browseInstallRootButton, &QPushButton::clicked,
            this, &GpuRecommendationDialog::browseInstallRoot);
    connect(m_installButton, &QPushButton::clicked,
            this, &GpuRecommendationDialog::startInstallation);
    connect(m_cancelInstallButton, &QPushButton::clicked,
            this, &GpuRecommendationDialog::cancelInstallation);
    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setSingleShot(true);
    m_autoSaveTimer->setInterval(250);
    connect(m_autoSaveTimer, &QTimer::timeout, this, [this] {
        persistSelection(false);
        updatePresentation();
    });
    connect(m_installRoot, &QLineEdit::textChanged, this, [this] {
        m_installFeedback.clear();
        m_autoSaveTimer->start();
        updatePresentation();
    });
    connect(m_installRoot, &QLineEdit::editingFinished,
            this, &GpuRecommendationDialog::flushPendingSelection);
    connect(m_installer, &ChickenRiceInstallService::progressChanged, this,
            [this](const ChickenRiceInstallProgress &) {
        updatePresentation();
        emit activityChanged();
    });
    connect(m_installer, &ChickenRiceInstallService::finished, this,
            [this](const ChickenRiceInstallResult &result) {
        if (m_settings && result.state == ChickenRiceInstallState::Ready
            && !result.engineDirectory.isEmpty()) {
            m_settings->setValue(QStringLiteral("Ai/ChickenRice/engineDirectory"), result.engineDirectory);
            m_settings->sync();
        }
        m_installCancelRequested = false;
        updatePresentation();
        QPointer<GpuRecommendationDialog> guard(this);
        if (!m_shuttingDown && result.state == ChickenRiceInstallState::Ready
            && !result.engineDirectory.isEmpty()) {
            emit engineReady(result.engineDirectory);
            if (!guard)
                return;
        }
        emit activityChanged();
        if (guard)
            finishShutdownIfReady();
    });
    connect(m_installer, &ChickenRiceInstallService::shutdownFinished,
            this, &GpuRecommendationDialog::finishShutdownIfReady);
    connect(m_installer, &QObject::destroyed, this, [this] {
        m_installer = nullptr;
        updatePresentation();
        QPointer<GpuRecommendationDialog> guard(this);
        emit activityChanged();
        if (guard)
            finishShutdownIfReady();
    });
    restoreSelection();
}

GpuRecommendationDialog::~GpuRecommendationDialog()
{
    flushPendingSelection();
    // The worker retains values only. Destroying its watcher disconnects the
    // completion callback; cancellation never requires a GUI-thread join.
    if (m_cancelled)
        m_cancelled->store(true);
    if (m_installer) {
        disconnect(m_installer, nullptr, this, nullptr);
        if (m_ownsInstaller)
            m_installer->shutdown();
    }
}

bool GpuRecommendationDialog::isBusy() const
{
    return m_busy || (m_installer && m_installer->isBusy());
}

bool GpuRecommendationDialog::installationBusy() const
{
    return m_installer && m_installer->isBusy();
}

void GpuRecommendationDialog::showEvent(QShowEvent *event)
{
    if (m_embedded) {
        QDialog::showEvent(event);
        return;
    }
    if (!m_installer || !m_installer->isBusy()) {
        // A direct hide/show can reopen before the path debounce fires.
        flushPendingSelection();
        restoreSelection();
    } else {
        updatePresentation();
    }
    QDialog::showEvent(event);
}

bool GpuRecommendationDialog::nativeEvent(
    const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}

void GpuRecommendationDialog::restoreSelection()
{
    const QSignalBlocker modeBlocker(m_selectionMode);
    const QSignalBlocker variantBlocker(m_manualVariant);
    const QSignalBlocker rootBlocker(m_installRoot);
    // Restoring the last valid values clears draft validation feedback. Keep
    // an actual storage failure visible until settings can be saved again.
    if (!m_settings || m_settings->status() == QSettings::NoError)
        m_selectionFeedback.clear();
    QString mode = QStringLiteral("auto");
    std::optional<ChickenRiceVariant> variant;
    if (m_settings) {
        const auto savedMode = m_settings->value(SettingsPrefix + QStringLiteral("selectionMode"));
        const auto savedVariant = m_settings->value(SettingsPrefix + QStringLiteral("manualVariant"));
        if (savedVariant.metaType().id() == QMetaType::QString)
            variant = chickenRiceVariantFromKey(savedVariant.toString());
        if (savedMode.metaType().id() == QMetaType::QString
            && savedMode.toString() == QStringLiteral("manual") && variant)
            mode = QStringLiteral("manual");
    }
    m_selectionMode->setCurrentIndex(m_selectionMode->findData(mode));
    m_manualVariant->setCurrentIndex(variant
        ? m_manualVariant->findData(chickenRiceVariantKey(*variant)) : 0);
    QString installRoot;
    if (m_settings) {
        const auto savedRoot = m_settings->value(InstallRootKey);
        if (savedRoot.metaType().id() == QMetaType::QString)
            installRoot = normalizedInstallRoot(savedRoot.toString());
    }
    m_installRoot->setText(installRoot.isEmpty() ? defaultInstallRoot() : installRoot);
    updatePresentation();
}

void GpuRecommendationDialog::startDetection()
{
    if (isBusy() || m_shuttingDown)
        return;
    m_busy = true;
    const auto generation = ++m_requestGeneration;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    const auto probe = m_probe;
    auto promise = std::make_shared<QPromise<GpuDetectionResult>>();
    auto *watcher = new QFutureWatcher<GpuDetectionResult>(this);
    connect(watcher, &QFutureWatcher<GpuDetectionResult>::finished, this,
            [this, watcher, generation, cancelled] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_busy = false;
        m_cancelled.reset();
        if (!m_shuttingDown) {
            if (generation == m_requestGeneration && !cancelled->load() && !result.cancelled)
                applyDetection(result);
            else
                m_detectionStatus->setText(QStringLiteral("检测已取消，可重新检测。"));
            updatePresentation();
        }
        QPointer<GpuRecommendationDialog> guard(this);
        emit activityChanged();
        if (guard)
            finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    m_detectionStatus->show();
    m_detectionStatus->setText(m_devices.isEmpty() ? QStringLiteral("正在检测 GPU…")
        : QStringLiteral("正在重新检测 GPU；下方仍为上次检测结果。"));
    updatePresentation();
    QThreadPool::globalInstance()->start([promise, probe, cancelled] {
        promise->start();
        GpuDetectionResult result;
        try {
            if (cancelled->load())
                result.cancelled = true;
            else
                result = probe(*cancelled);
        } catch (const std::exception &exception) {
            result.diagnostics.append(QStringLiteral("GPU 检测失败：%1")
                .arg(QString::fromUtf8(exception.what())));
        } catch (...) {
            result.diagnostics.append(QStringLiteral("GPU 检测发生未知错误。"));
        }
        result.cancelled = result.cancelled || cancelled->load();
        promise->addResult(std::move(result));
        promise->finish();
    });
    emit activityChanged();
}

void GpuRecommendationDialog::applyDetection(const GpuDetectionResult &result)
{
    const QString previousDevice = m_deviceCombo->currentData().toString();
    const QSignalBlocker deviceBlocker(m_deviceCombo);
    m_devices = result.devices;
    m_deviceCombo->clear();
    for (const auto &device : m_devices) {
        m_deviceCombo->addItem(device.name
            + (device.software ? QStringLiteral("（软件适配器）") : QString()), device.id);
        m_deviceCombo->setItemData(m_deviceCombo->count() - 1,
            QStringLiteral("%1\n%2；驱动 %3")
                .arg(device.name, memoryDescription(device.dedicatedMemoryBytes),
                     device.driverVersion.isEmpty() ? QStringLiteral("未报告") : device.driverVersion),
            Qt::ToolTipRole);
    }
    int selected = m_deviceCombo->findData(previousDevice);
    if (selected < 0)
        selected = preferredGpuIndex(m_devices);
    if (selected < 0 && !m_devices.isEmpty())
        selected = 0;
    m_deviceCombo->setCurrentIndex(selected);
    QString status = m_devices.isEmpty()
        ? QStringLiteral("未取得可用的 GPU 信息，仍可手动选择版本。")
        : QStringLiteral("检测到 %1 个显示适配器；可切换查看各设备。").arg(m_devices.size());
    if (!result.diagnostics.isEmpty())
        status += QLatin1Char('\n') + result.diagnostics.join(QLatin1Char('\n'));
    m_detectionStatus->setText(status);
    m_detectionStatus->setVisible(!m_embedded || m_devices.isEmpty() || !result.diagnostics.isEmpty());
}

void GpuRecommendationDialog::updatePresentation()
{
    const bool manual = m_selectionMode->currentData().toString() == QStringLiteral("manual");
    const auto selectedVariant = chickenRiceVariantFromKey(m_manualVariant->currentData().toString());
    const bool installing = m_installer && m_installer->isBusy();
    m_selectionMode->setEnabled(!m_shuttingDown && !installing);
    m_manualVariantLabel->setVisible(manual);
    m_manualVariant->setVisible(manual);
    m_manualVariant->setEnabled(manual && !m_shuttingDown && !installing);
    m_deviceCombo->setEnabled(!isBusy() && !m_shuttingDown && !m_devices.isEmpty());
    m_detectButton->setEnabled(!isBusy() && !m_shuttingDown);
    updateInstallPresentation();
    const int index = m_deviceCombo->currentIndex();
    const bool hasDevice = index >= 0 && index < m_devices.size();
    if (hasDevice) {
        const auto &device = m_devices.at(index);
        m_deviceInformation->setText(QStringLiteral("%1\n%2；驱动 %3")
            .arg(device.name, memoryDescription(device.dedicatedMemoryBytes),
                 device.driverVersion.isEmpty() ? QStringLiteral("未报告") : device.driverVersion));
    } else {
        m_deviceInformation->setText(QStringLiteral("未选择显示适配器。"));
    }
    m_deviceInformation->setVisible(!m_embedded);
    m_deviceCombo->setToolTip(m_deviceInformation->text());
    if (manual) {
        m_recommendation->setText(selectedVariant
            ? QStringLiteral("手动选择：%1（未验证）").arg(chickenRiceVariantDisplayName(*selectedVariant))
            : QStringLiteral("请选择有效版本。"));
        m_explanation->setText(QStringLiteral(
            "手动选择仅记录偏好；尚未验证该版本与所选 GPU 的兼容性。"));
        m_explanation->setVisible(!m_embedded);
        m_recommendation->setToolTip((selectedVariant
            ? chickenRiceArchiveName(*selectedVariant) + QLatin1Char('\n') : QString())
            + m_explanation->text());
        return;
    }
    if (!hasDevice) {
        m_recommendation->setText(QStringLiteral("暂无自动推荐"));
        m_explanation->setText(QStringLiteral("可重新检测，或切换为手动选择。"));
        m_explanation->setVisible(!m_embedded);
        m_recommendation->setToolTip(m_explanation->text());
        return;
    }
    const auto recommendation = recommendChickenRice(m_devices.at(index));
    QString status;
    switch (recommendation.status) {
    case RecommendationStatus::Recommended: status = QStringLiteral("推荐"); break;
    case RecommendationStatus::NeedsVerification: status = QStringLiteral("待验证"); break;
    case RecommendationStatus::DriverUpdateRequired: status = QStringLiteral("需更新驱动"); break;
    case RecommendationStatus::NoAutomaticMatch: status = QStringLiteral("无自动匹配"); break;
    }
    m_recommendation->setText(recommendation.variant
        ? QStringLiteral("%1：%2").arg(status, chickenRiceVariantDisplayName(*recommendation.variant))
        : status);
    QString explanation = recommendation.explanation;
    if (recommendation.lowMemoryMode && recommendation.variant)
        explanation += QStringLiteral("\n建议优先使用低显存模式。");
    explanation += QStringLiteral("\n推荐基于设备信息，尚未进行实际推理验证。");
    // Keep normal recommendations compact in the embedded AI settings page.
    // Actionable compatibility failures remain visible; details stay available
    // on hover for both automatic and manual choices.
    m_explanation->setText(m_embedded ? recommendation.explanation : explanation);
    m_explanation->setVisible(!m_embedded
        || recommendation.status == RecommendationStatus::DriverUpdateRequired
        || recommendation.status == RecommendationStatus::NoAutomaticMatch);
    m_recommendation->setToolTip((recommendation.variant
        ? chickenRiceArchiveName(*recommendation.variant) + QLatin1Char('\n') : QString())
        + explanation);
}

bool GpuRecommendationDialog::persistSelection(bool requireComplete)
{
    if (m_shuttingDown || !m_settings)
        return false;
    const QString mode = m_selectionMode->currentData().toString();
    const auto variant = chickenRiceVariantFromKey(m_manualVariant->currentData().toString());
    const QString root = normalizedInstallRoot(m_installRoot->text());
    const bool validMode = mode == QStringLiteral("auto")
        || (mode == QStringLiteral("manual") && variant.has_value());
    if (requireComplete && (!validMode || root.isEmpty())) {
        m_selectionFeedback = QStringLiteral("选择或下载安装位置无效，未开始安装。");
        updateInstallPresentation();
        return false;
    }
    // Save independently: an unfinished path edit must not discard a valid
    // version preference, and invalid input never replaces the saved value.
    if (validMode) {
        m_settings->setValue(SettingsPrefix + QStringLiteral("selectionMode"), mode);
        if (mode == QStringLiteral("manual"))
            m_settings->setValue(SettingsPrefix + QStringLiteral("manualVariant"), chickenRiceVariantKey(*variant));
    }
    if (!root.isEmpty())
        m_settings->setValue(InstallRootKey, root);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        m_selectionFeedback = QStringLiteral("设置自动保存失败，请检查设置存储位置。");
        return false;
    }
    m_selectionFeedback = !validMode ? QStringLiteral("版本选择无效，已保留上次保存的选择。")
        : root.isEmpty() ? QStringLiteral("下载安装位置无效，已保留上次保存的位置。") : QString();
    return true;
}

void GpuRecommendationDialog::flushPendingSelection()
{
    if (!m_autoSaveTimer || !m_autoSaveTimer->isActive())
        return;
    m_autoSaveTimer->stop();
    persistSelection(false);
    updatePresentation();
}

std::optional<ChickenRiceVariant> GpuRecommendationDialog::selectedInstallVariant() const
{
    if (m_selectionMode->currentData().toString() == QStringLiteral("manual"))
        return chickenRiceVariantFromKey(m_manualVariant->currentData().toString());
    const int index = m_deviceCombo->currentIndex();
    if (index < 0 || index >= m_devices.size())
        return std::nullopt;
    const auto recommendation = recommendChickenRice(m_devices.at(index));
    if (recommendation.status == RecommendationStatus::DriverUpdateRequired
        || recommendation.status == RecommendationStatus::NoAutomaticMatch)
        return std::nullopt;
    return recommendation.variant;
}

void GpuRecommendationDialog::updateInstallPresentation()
{
    const bool installing = m_installer && m_installer->isBusy();
    const auto variant = selectedInstallVariant();
    const auto package = variant ? chickenRicePackage(*variant) : std::nullopt;
    const QString root = normalizedInstallRoot(m_installRoot->text());
    m_installRoot->setEnabled(!installing && !m_shuttingDown);
    m_browseInstallRootButton->setEnabled(!installing && !m_shuttingDown && !m_installFolderDialog);
    m_installButton->setEnabled(!isBusy() && !m_shuttingDown && m_settings && package
        && !root.isEmpty() && m_installer);
    m_cancelInstallButton->setVisible(installing);
    m_cancelInstallButton->setEnabled(installing && !m_installCancelRequested && !m_shuttingDown);
    m_installBackgroundHint->setVisible(installing);
    m_packageSize->setText(package
        ? QStringLiteral("下载大小：%1（%2 个分卷）；准备时需预留约 %3 空间")
            .arg(byteDescription(package->size)).arg(package->parts.size())
            .arg(byteDescription(package->size + package->unpackedSize + 64 * 1024 * 1024))
        : QStringLiteral("选择可下载的版本后显示包大小。"));
    m_installButton->setToolTip(package ? QString()
        : QStringLiteral("请完成 GPU 检测；驱动需更新时可更新驱动或手动选择版本。"));
    const auto progress = m_installer ? m_installer->progress() : ChickenRiceInstallProgress{};
    const auto result = m_installer ? m_installer->result() : ChickenRiceInstallResult{};
    const auto state = installing || result.state == ChickenRiceInstallState::Idle
        ? progress.state : result.state;
    m_installButton->setText(state == ChickenRiceInstallState::Failed
        || state == ChickenRiceInstallState::Cancelled ? QStringLiteral("重试") : QStringLiteral("下载并准备"));
    m_installProgress->setVisible(installing);
    m_installBytes->setVisible(installing && state == ChickenRiceInstallState::Downloading);
    if (installing && state == ChickenRiceInstallState::Downloading && progress.totalBytes > 0) {
        m_installProgress->setRange(0, 1000);
        const double ratio = std::clamp(static_cast<double>(progress.completedBytes)
            / static_cast<double>(progress.totalBytes), 0.0, 1.0);
        m_installProgress->setValue(static_cast<int>(ratio * 1000.0));
        m_installBytes->setText(QStringLiteral("%1 / %2")
            .arg(byteDescription(progress.completedBytes), byteDescription(progress.totalBytes)));
        m_installBytes->setToolTip(QStringLiteral("%1 / %2 字节")
            .arg(progress.completedBytes).arg(progress.totalBytes));
    } else if (installing) {
        m_installProgress->setRange(0, 0);
        if (state == ChickenRiceInstallState::Downloading)
            m_installBytes->setText(QStringLiteral("已下载 %1").arg(byteDescription(progress.completedBytes)));
    }
    QString status = m_installFeedback;
    if (status.isEmpty() && m_installCancelRequested && installing)
        status = QStringLiteral("正在暂停，请稍候…");
    if (status.isEmpty()) {
        switch (state) {
        case ChickenRiceInstallState::Idle:
            status = m_embedded ? QString() : QStringLiteral("尚未准备本地引擎。"); break;
        case ChickenRiceInstallState::Preparing:
            status = QStringLiteral("正在准备下载…"); break;
        case ChickenRiceInstallState::Downloading:
            status = QStringLiteral("正在下载…"); break;
        case ChickenRiceInstallState::Verifying:
            status = QStringLiteral("正在校验文件…"); break;
        case ChickenRiceInstallState::Extracting:
            status = QStringLiteral("正在解压…"); break;
        case ChickenRiceInstallState::Ready:
            if (m_startedInstallVariant) {
                const bool previousSelection = variant != m_startedInstallVariant
                    || root != m_startedInstallRoot;
                status = previousSelection
                    ? QStringLiteral("上次准备的 %1：%2\n当前选择已改变；以上为上次任务结果。")
                        .arg(chickenRiceVariantDisplayName(*m_startedInstallVariant), result.engineDirectory)
                    : QStringLiteral("已准备完成（%1）：%2")
                        .arg(chickenRiceVariantDisplayName(*m_startedInstallVariant), result.engineDirectory);
            } else {
                status = QStringLiteral("上次准备已完成：%1").arg(result.engineDirectory);
            }
            break;
        case ChickenRiceInstallState::Cancelled:
            status = QStringLiteral("已暂停，可点击重试。"); break;
        case ChickenRiceInstallState::Failed:
            status = QStringLiteral("准备失败：%1").arg(result.error); break;
        }
        if (installing && !progress.message.isEmpty())
            status += QLatin1Char('\n') + progress.message;
        if (!installing && !result.warnings.isEmpty())
            status += QLatin1Char('\n') + result.warnings.join(QLatin1Char('\n'));
    }
    if (!m_selectionFeedback.isEmpty())
        status = m_selectionFeedback + (status.isEmpty() ? QString() : QLatin1Char('\n') + status);
    m_installStatus->setText(status);
    m_installStatus->setVisible(!status.isEmpty());
}

void GpuRecommendationDialog::browseInstallRoot()
{
    if (m_shuttingDown || (m_installer && m_installer->isBusy()) || m_installFolderDialog)
        return;
    auto *dialog = new QFileDialog(this, QStringLiteral("选择下载安装位置"), m_installRoot->text());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setWindowModality(Qt::WindowModal);
    m_installFolderDialog = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (m_installFolderDialog != dialog)
            return;
        m_installFolderDialog = nullptr;
        if (result == QDialog::Accepted && !m_shuttingDown
            && (!m_installer || !m_installer->isBusy())) {
            const auto selected = dialog->selectedFiles();
            if (!selected.isEmpty())
                m_installRoot->setText(selected.first());
        }
        updateInstallPresentation();
        emit activityChanged();
    });
    dialog->open();
    updateInstallPresentation();
    emit activityChanged();
}

void GpuRecommendationDialog::startInstallation()
{
    if (isBusy() || m_shuttingDown || !m_installer)
        return;
    const auto variant = selectedInstallVariant();
    const auto package = variant ? chickenRicePackage(*variant) : std::nullopt;
    const QString root = normalizedInstallRoot(m_installRoot->text());
    if (!package || root.isEmpty())
        return;
    if (m_autoSaveTimer)
        m_autoSaveTimer->stop();
    if (!persistSelection()) {
        updateInstallPresentation();
        return;
    }
    if (m_installFolderDialog)
        m_installFolderDialog->reject();
    m_installFeedback.clear();
    m_installCancelRequested = false;
    ChickenRiceInstallRequest request;
    request.package = *package;
    request.installRoot = root;
    QPointer<GpuRecommendationDialog> guard(this);
    const bool started = m_installer->start(request);
    if (!guard)
        return;
    if (started) {
        m_startedInstallVariant = variant;
        m_startedInstallRoot = root;
    } else {
        m_installFeedback = QStringLiteral("当前无法开始准备，请稍后重试。");
    }
    updatePresentation();
    emit activityChanged();
}

void GpuRecommendationDialog::cancelInstallation()
{
    if (!m_installer || !m_installer->isBusy() || m_installCancelRequested)
        return;
    m_installCancelRequested = true;
    m_installer->cancel();
    updateInstallPresentation();
}

void GpuRecommendationDialog::cancelDetection()
{
    if (!m_busy)
        return;
    ++m_requestGeneration;
    if (m_cancelled)
        m_cancelled->store(true);
    m_detectionStatus->setText(QStringLiteral("正在取消检测…"));
}

void GpuRecommendationDialog::done(int result)
{
    flushPendingSelection();
    // Embedded panels are shown/hidden only by their host; Esc or accept must
    // not silently remove the component page or cancel its GPU probe.
    if (m_embedded)
        return;
    cancelDetection();
    if (m_installFolderDialog)
        m_installFolderDialog->reject();
    // A normal close keeps the installer alive in this reused dialog.
    QDialog::done(result);
}

void GpuRecommendationDialog::shutdown()
{
    if (!m_shuttingDown) {
        flushPendingSelection();
        m_shuttingDown = true;
        cancelDetection();
        if (m_installFolderDialog)
            m_installFolderDialog->reject();
        hide();
        updatePresentation();
        if (m_installer) {
            QPointer<GpuRecommendationDialog> guard(this);
            m_installer->shutdown();
            if (!guard)
                return;
        }
    }
    finishShutdownIfReady();
}

void GpuRecommendationDialog::finishShutdownIfReady()
{
    if (m_shuttingDown && !isBusy() && !m_shutdownNotified) {
        m_shutdownNotified = true;
        emit shutdownFinished();
    }
}

} // namespace asmr
