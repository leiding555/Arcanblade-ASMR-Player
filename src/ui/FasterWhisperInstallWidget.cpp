#include "ui/FasterWhisperInstallWidget.h"

#include "ai/FasterWhisperArchive.h"
#include "ai/FasterWhisperPackage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPixmap>
#include <QPromise>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <exception>
#include <utility>

namespace asmr {
namespace {
using State = FasterWhisperInstallState;
const QString Prefix = QStringLiteral("Ai/FasterWhisper/");

QString absolutePath(const QString &input)
{
    const QString path = QDir::fromNativeSeparators(input.trimmed());
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)
        || path.startsWith(QStringLiteral("//?/")) || path.startsWith(QStringLiteral("//./"))) return {};
    return QDir::cleanPath(path);
}
QString savedPath(QSettings *settings, const QString &key)
{
    if (!settings) return {};
    const QVariant value = settings->value(Prefix + key);
    return value.metaType().id() == QMetaType::QString ? absolutePath(value.toString()) : QString();
}
QString resolveModel(const QString &input)
{
    const QString root = absolutePath(input);
    if (root.isEmpty()) return {};
    for (const auto &candidate : {root, QDir(root).filePath(QStringLiteral("faster-whisper-large-v2")),
                                 QDir(root).filePath(QStringLiteral("_models/faster-whisper-large-v2"))}) {
        if (QFileInfo(QDir(candidate).filePath(QStringLiteral("model.bin"))).isFile()) return candidate;
    }
    return root;
}
QLabel *label(const char *name, QWidget *parent)
{
    auto *value = new QLabel(parent);
    value->setObjectName(QString::fromLatin1(name));
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    value->setMinimumWidth(0);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return value;
}
QLabel *icon(const char *name, QWidget *parent)
{
    auto *value = new QLabel(parent);
    value->setObjectName(QString::fromLatin1(name));
    value->setWordWrap(false);
    value->setAlignment(Qt::AlignCenter);
    value->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    value->setFixedSize(18, 18);
    value->hide();
    return value;
}
void setStatus(QLabel *text, QLabel *mark, const QString &state, const QString &message)
{
    text->setText(message);
    text->setToolTip(message);
    text->setProperty("validationState", state);
    if (mark->property("validationState").toString() != state) {
        mark->setProperty("validationState", state);
        const bool passed = state == QStringLiteral("passed");
        const bool failed = state == QStringLiteral("failed");
        if (passed || failed) {
            const auto ratio = mark->devicePixelRatioF();
            QPixmap image(QSize(qRound(18 * ratio), qRound(18 * ratio)));
            image.setDevicePixelRatio(ratio);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(passed ? QColor("#48bb92") : QColor("#ed7f86"),
                                2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (passed) {
                QPainterPath path;
                path.moveTo(3, 9); path.lineTo(7, 13); path.lineTo(15, 5);
                painter.drawPath(path);
            } else {
                painter.drawLine(QPointF(4, 4), QPointF(14, 14));
                painter.drawLine(QPointF(14, 4), QPointF(4, 14));
            }
            painter.end();
            mark->setPixmap(image);
        } else mark->clear();
    }
    mark->setVisible(state == QStringLiteral("passed") || state == QStringLiteral("failed"));
    mark->setAccessibleName(state == QStringLiteral("passed") ? QStringLiteral("检查通过")
        : state == QStringLiteral("failed") ? QStringLiteral("检查失败") : QString());
}
QString bytesText(qint64 bytes)
{ return QString::number(double(bytes) / (1024 * 1024 * 1024), 'f', 2) + QStringLiteral(" GiB"); }
// Keep form status rows compact while allowing long errors to wrap.
class StatusRow final : public QWidget {
public:
    explicit StatusRow(QWidget *parent) : QWidget(parent) {}
    void refreshHeight()
    {
        if (!layout() || width() <= 0) return;
        const int natural = layout()->hasHeightForWidth() ? layout()->heightForWidth(width()) : layout()->sizeHint().height();
        const int height = qMax(layout()->minimumSize().height(), natural);
        if (height >= 0 && (minimumHeight() != height || maximumHeight() != height)) {
            setFixedHeight(height); updateGeometry();
        }
    }
protected:
    void resizeEvent(QResizeEvent *event) override { QWidget::resizeEvent(event); refreshHeight(); }
    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            refreshHeight();
        return handled;
    }
};
QString modelMappingError(const QString &path)
{
    const QString leaf = QFileInfo(path).fileName();
    const QString prefix = QStringLiteral("faster-whisper-");
    if (!leaf.startsWith(prefix) || leaf.size() <= prefix.size()
        || !QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]+$")).match(leaf).hasMatch())
        return QStringLiteral("模型目录名需为 faster-whisper-<名称>（英文字母、数字、下划线、点或连字符）。请重新选择含标准 _models/faster-whisper-large-v2 的组件，或下载安装组件；已有文件不会移动。");
    return {};
}
QString modelIdentity(const QString &path)
{
    QString identity = absolutePath(path);
    for (const auto &file : fasterWhisperPackage().modelFiles) {
        const QFileInfo info(QDir(path).filePath(file.name));
        if (!info.isFile() || info.isSymLink() || info.size() <= 0) return {};
        identity += QLatin1Char('|') + file.name + QLatin1Char(':') + QString::number(info.size())
            + QLatin1Char(':') + QString::number(info.lastModified().toMSecsSinceEpoch())
            + QLatin1Char(':') + QString::number(info.birthTime().toMSecsSinceEpoch());
    }
    return identity;
}
struct CheckResult {
    QString engineInput, modelInput, engine, model, engineError, modelError, modelIdentity;
};
}

FasterWhisperInstallWidget::FasterWhisperInstallWidget(QSettings *settings, QWidget *parent,
    FasterWhisperInstallService *installer, Validator engineValidator, Validator modelValidator)
    : QWidget(parent), m_settings(settings), m_installer(installer),
      m_engineValidator(std::move(engineValidator)), m_modelValidator(std::move(modelValidator))
{
    setObjectName(QStringLiteral("fasterWhisperInstallWidget"));
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    if (!m_engineValidator) m_engineValidator = [](const QString &path, const std::atomic_bool &cancelled) {
        QString error;
        if (!cancelled.load()) (void)validateFasterWhisperEngine(path, &error);
        return error;
    };
    if (!m_modelValidator) m_modelValidator = [](const QString &path, const std::atomic_bool &cancelled) {
        QString error;
        (void)validateFasterWhisperModel(path, fasterWhisperPackage().modelFiles, cancelled, &error);
        return error;
    };
    if (!m_installer) {
        m_installer = new FasterWhisperInstallService(this);
        m_ownsInstaller = true;
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->setAlignment(Qt::AlignTop);
    auto *form = new QFormLayout;
    form->setAlignment(Qt::AlignTop);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_engine = new QLineEdit(this);
    m_engine->setObjectName(QStringLiteral("fasterWhisperEngineDirectory"));
    m_engine->setMinimumWidth(0);
    m_engine->setPlaceholderText(QStringLiteral("选择包含 faster-whisper-xxl.exe 的组件目录"));
    m_browseEngine = new QPushButton(QStringLiteral("选择已有组件…"), this);
    m_browseEngine->setObjectName(QStringLiteral("fasterWhisperBrowseEngine"));
    auto *engineRow = new QHBoxLayout;
    engineRow->addWidget(m_engine, 1); engineRow->addWidget(m_browseEngine);
    form->addRow(QStringLiteral("当前组件目录"), engineRow);
    auto *host = new StatusRow(this);
    host->setObjectName(QStringLiteral("fasterWhisperComponentStatusRow"));
    host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0); row->setSpacing(6);
    m_engineIcon = icon("fasterWhisperEngineIcon", host);
    m_engineStatus = label("fasterWhisperEngineStatus", host);
    row->addWidget(m_engineIcon, 0, Qt::AlignTop);
    row->addWidget(m_engineStatus, 1, Qt::AlignTop);
    form->addRow(QString(), host);
    m_compute = new QComboBox(this);
    m_compute->setObjectName(QStringLiteral("fasterWhisperComputeMode"));
    m_compute->addItem(QStringLiteral("GPU（低显存）"), aiSubtitleComputeModeKey(AiSubtitleComputeMode::GpuLowMemory));
    m_compute->addItem(QStringLiteral("GPU（高显存）"), aiSubtitleComputeModeKey(AiSubtitleComputeMode::GpuHighMemory));
    m_compute->addItem(QStringLiteral("CPU"), aiSubtitleComputeModeKey(AiSubtitleComputeMode::Cpu));
    m_compute->setItemData(0, QStringLiteral("使用 int8_float16，优先节省显存。"), Qt::ToolTipRole);
    m_compute->setItemData(1, QStringLiteral("使用 float16，需要更多显存。"), Qt::ToolTipRole);
    m_compute->setItemData(2, QStringLiteral("使用 int8，在 CPU 上运行。"), Qt::ToolTipRole);
    form->addRow(QStringLiteral("运行方式"), m_compute);
    layout->addLayout(form);
    auto *parameters = new QWidget(this);
    parameters->setObjectName(QStringLiteral("fasterWhisperGenerationParameters"));
    // Each visible parameter block keeps its natural height. Extra viewport
    // height belongs to the final stretch, not the form or heading label.
    parameters->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *parameterLayout = new QVBoxLayout(parameters);
    parameterLayout->setContentsMargins(0, 0, 0, 0); parameterLayout->setSpacing(8);
    parameterLayout->setAlignment(Qt::AlignTop);
    auto *heading = label("fasterWhisperParametersHeading", parameters);
    heading->setWordWrap(false);
    heading->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    heading->setText(QStringLiteral("生成参数"));
    parameterLayout->addWidget(heading);
    auto *presetRow = new QHBoxLayout;
    m_preset = new QComboBox(parameters);
    m_preset->setObjectName(QStringLiteral("fasterWhisperGenerationPreset"));
    for (const auto &item : {std::pair{QStringLiteral("标准（组件默认）"), FasterWhisperGenerationPreset::ComponentDefault},
                            std::pair{QStringLiteral("轻声优先"), FasterWhisperGenerationPreset::QuietSpeech},
                            std::pair{QStringLiteral("减少误识别"), FasterWhisperGenerationPreset::Conservative},
                            std::pair{QStringLiteral("自定义"), FasterWhisperGenerationPreset::Custom}})
        m_preset->addItem(item.first, fasterWhisperGenerationPresetKey(item.second));
    m_reset = new QPushButton(QStringLiteral("恢复默认"), parameters);
    m_reset->setObjectName(QStringLiteral("fasterWhisperRestoreDefaults"));
    presetRow->addWidget(m_preset, 1); presetRow->addWidget(m_reset);
    parameterLayout->addLayout(presetRow);
    m_presetHint = label("fasterWhisperGenerationPresetHint", parameters);
    m_presetHint->setProperty("secondaryText", true);
    parameterLayout->addWidget(m_presetHint);
    m_customParameters = new QWidget(parameters);
    m_customParameters->setObjectName(QStringLiteral("fasterWhisperCustomParameters"));
    m_customParameters->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *custom = new QFormLayout(m_customParameters);
    custom->setContentsMargins(0, 0, 0, 0);
    custom->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    custom->setRowWrapPolicy(QFormLayout::WrapLongRows);
    custom->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_vad = new QCheckBox(QStringLiteral("过滤无语音片段"), m_customParameters);
    m_vad->setObjectName(QStringLiteral("fasterWhisperVadFilter"));
    m_vad->setToolTip(QStringLiteral("关闭后跳过 VAD 过滤，阈值暂停生效。"));
    custom->addRow(QStringLiteral("语音检测"), m_vad);
    m_threshold = new QDoubleSpinBox(m_customParameters);
    m_threshold->setObjectName(QStringLiteral("fasterWhisperVadThreshold"));
    m_threshold->setDecimals(2); m_threshold->setRange(0.3, 0.7); m_threshold->setSingleStep(0.05);
    m_threshold->setToolTip(QStringLiteral("较低更容易保留轻声，也可能保留更多噪声。"));
    custom->addRow(QStringLiteral("VAD 阈值"), m_threshold);
    m_beam = new QSpinBox(m_customParameters);
    m_beam->setObjectName(QStringLiteral("fasterWhisperBeamSize"));
    m_beam->setRange(1, 10);
    custom->addRow(QStringLiteral("搜索宽度"), m_beam);
    m_previousText = new QCheckBox(QStringLiteral("引用前文"), m_customParameters);
    m_previousText->setObjectName(QStringLiteral("fasterWhisperPreviousText"));
    m_previousText->setToolTip(QStringLiteral("可提高上下文一致性；反复重复时可关闭后重试。"));
    custom->addRow(QString(), m_previousText);
    m_subtitleFormatting = new QCheckBox(QStringLiteral("字幕断句与排版"), m_customParameters);
    m_subtitleFormatting->setObjectName(QStringLiteral("fasterWhisperSubtitleFormatting"));
    m_subtitleFormatting->setToolTip(QStringLiteral("使用引擎的亚洲语言排版预设拆分字幕和换行，字幕分段和时间边界可能变化；保留原始 JSON。默认关闭。"));
    custom->addRow(QString(), m_subtitleFormatting);
    parameterLayout->addWidget(m_customParameters);
    layout->addWidget(parameters);
    m_installToggle = new QPushButton(QStringLiteral("安装组件"), this);
    m_installToggle->setObjectName(QStringLiteral("fasterWhisperInstallToggle"));
    m_installToggle->setCheckable(true);
    layout->addWidget(m_installToggle, 0, Qt::AlignLeft);
    m_installSection = new QWidget(this);
    m_installSection->setObjectName(QStringLiteral("fasterWhisperInstallSection"));
    m_installSection->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *install = new QVBoxLayout(m_installSection);
    install->setContentsMargins(0, 0, 0, 0); install->setSpacing(8);
    install->setAlignment(Qt::AlignTop);
    auto *source = label("fasterWhisperSources", m_installSection);
    const auto package = fasterWhisperPackage();
    qint64 modelBytes = 0;
    for (const auto &file : package.modelFiles) modelBytes += file.size;
    source->setText(QStringLiteral("引擎：%1；模型：%2").arg(bytesText(package.engineArchive.size), bytesText(modelBytes)));
    source->setToolTip(QStringLiteral("Purfview r245.4：https://github.com/Purfview/whisper-standalone-win/releases/tag/Faster-Whisper-XXL\nlarge-v2：https://huggingface.co/Systran/faster-whisper-large-v2"));
    install->addWidget(source);
    auto *rootForm = new QFormLayout;
    rootForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    rootForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_root = new QLineEdit(m_installSection);
    m_root->setObjectName(QStringLiteral("fasterWhisperInstallRoot")); m_root->setMinimumWidth(0);
    m_browseRoot = new QPushButton(QStringLiteral("浏览…"), m_installSection);
    m_browseRoot->setObjectName(QStringLiteral("fasterWhisperBrowseRoot"));
    auto *rootRow = new QHBoxLayout;
    rootRow->addWidget(m_root, 1); rootRow->addWidget(m_browseRoot);
    rootForm->addRow(QStringLiteral("安装位置"), rootRow);
    install->addLayout(rootForm);
    m_saveStatus = label("fasterWhisperSaveStatus", this); layout->addWidget(m_saveStatus);
    m_status = label("fasterWhisperInstallStatus", m_installSection); install->addWidget(m_status);
    m_progress = new QProgressBar(m_installSection);
    m_progress->setObjectName(QStringLiteral("fasterWhisperInstallProgress"));
    m_progress->setTextVisible(false); m_progress->setMaximumHeight(14); install->addWidget(m_progress);
    m_bytes = label("fasterWhisperInstallBytes", m_installSection); install->addWidget(m_bytes);
    auto *buttons = new QHBoxLayout;
    m_check = new QPushButton(QStringLiteral("重新检查"), m_installSection);
    m_check->setObjectName(QStringLiteral("fasterWhisperCheckButton"));
    m_prepare = new QPushButton(QStringLiteral("下载并准备"), m_installSection);
    m_prepare->setObjectName(QStringLiteral("fasterWhisperPrepareButton"));
    m_prepare->setProperty("primaryAction", true);
    m_cancel = new QPushButton(QStringLiteral("取消"), m_installSection);
    m_cancel->setObjectName(QStringLiteral("fasterWhisperCancelButton"));
    for (auto *button : {m_browseEngine, m_browseRoot, m_reset, m_installToggle, m_check, m_prepare, m_cancel})
        button->setAutoDefault(false);
    buttons->addWidget(m_check); buttons->addStretch(); buttons->addWidget(m_cancel); buttons->addWidget(m_prepare);
    install->addLayout(buttons);
    layout->addWidget(m_installSection);
    layout->addStretch(1);
    m_installSection->hide();
    connect(m_installToggle, &QPushButton::toggled, this, [this](bool expanded) {
        m_installSection->setVisible(expanded); updateGeometry(); emit activityChanged();
    });
    connect(m_compute, &QComboBox::currentIndexChanged, this, [this] { persistGenerationOptions(); });
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this] { persistGenerationOptions(); updatePresentation(); });
    connect(m_vad, &QCheckBox::toggled, this, [this] { persistGenerationOptions(); updatePresentation(); });
    connect(m_threshold, &QDoubleSpinBox::valueChanged, this, [this] { persistGenerationOptions(); });
    connect(m_beam, &QSpinBox::valueChanged, this, [this] { persistGenerationOptions(); });
    connect(m_previousText, &QCheckBox::toggled, this, [this] { persistGenerationOptions(); });
    connect(m_subtitleFormatting, &QCheckBox::toggled, this, [this] { persistGenerationOptions(); });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        setGenerationOptions({}); persistGenerationOptions(); updatePresentation();
    });
    m_checkTimer = new QTimer(this);
    m_checkTimer->setSingleShot(true); m_checkTimer->setInterval(180);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true); m_saveTimer->setInterval(250);
    connect(m_checkTimer, &QTimer::timeout, this, &FasterWhisperInstallWidget::startCheck);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { persistRoot(); updatePresentation(); });
    connect(m_engine, &QLineEdit::textChanged, this, [this] {
        if (!m_loading) { m_engineDirty = true; scheduleCheck(); }
    });
    connect(m_root, &QLineEdit::textChanged, this, [this] {
        if (!m_loading) { m_saveTimer->start(); updatePresentation(); }
    });
    connect(m_root, &QLineEdit::editingFinished, this, [this] { m_saveTimer->stop(); persistRoot(); updatePresentation(); });
    connect(m_browseEngine, &QPushButton::clicked, this, [this] { browse(m_engine, QStringLiteral("选择 Faster-Whisper-XXL 程序目录")); });
    connect(m_browseRoot, &QPushButton::clicked, this, [this] { browse(m_root, QStringLiteral("选择新组件下载安装位置")); });
    connect(m_check, &QPushButton::clicked, this, &FasterWhisperInstallWidget::recheck);
    connect(m_prepare, &QPushButton::clicked, this, &FasterWhisperInstallWidget::startInstallation);
    connect(m_cancel, &QPushButton::clicked, this, &FasterWhisperInstallWidget::cancel);
    connect(m_installer, &FasterWhisperInstallService::progressChanged, this, [this] {
        updatePresentation(); emit activityChanged();
    });
    connect(m_installer, &FasterWhisperInstallService::finished, this, &FasterWhisperInstallWidget::applyResult);
    connect(m_installer, &FasterWhisperInstallService::shutdownFinished, this, &FasterWhisperInstallWidget::finishShutdownIfReady);
    connect(m_installer, &QObject::destroyed, this, [this] {
        m_installer = nullptr;
        updatePresentation();
        const QPointer<FasterWhisperInstallWidget> guard(this);
        emit activityChanged();
        if (guard) finishShutdownIfReady();
    });
    m_engine->setText(savedPath(settings, QStringLiteral("engineDirectory")));
    m_modelDirectory = savedPath(settings, QStringLiteral("modelDirectory"));
    FasterWhisperGenerationOptions options;
    if (settings) {
        options.preset = fasterWhisperGenerationPresetFromKey(settings->value(Prefix + QStringLiteral("generationPreset")).toString())
            .value_or(FasterWhisperGenerationPreset::ComponentDefault);
        options.vadFilter = settings->value(Prefix + QStringLiteral("vadFilter"), true).toBool();
        options.vadThreshold = settings->value(Prefix + QStringLiteral("vadThreshold"), 0.45).toDouble();
        options.subtitleFormatting = settings->value(Prefix + QStringLiteral("subtitleFormatting"), false).toBool();
        options.beamSize = settings->value(Prefix + QStringLiteral("beamSize"), 5).toInt();
        options.conditionOnPreviousText = settings->value(Prefix + QStringLiteral("conditionOnPreviousText"), true).toBool();
        if (!validateFasterWhisperGenerationOptions(options).isEmpty()) options = {};
        const QString savedMode = settings->value(Prefix + QStringLiteral("computeMode")).toString();
        auto mode = aiSubtitleComputeModeFromKey(savedMode).value_or(AiSubtitleComputeMode::GpuLowMemory);
        if (savedMode == QStringLiteral("gpu")) {
            // The old GPU + auto precision used the model's standard GPU
            // precision. Preserve the old custom memory-saving choice instead.
            const bool memorySaving = options.preset == FasterWhisperGenerationPreset::Custom
                && settings->value(Prefix + QStringLiteral("computeType")).toString() == QStringLiteral("memory-saving");
            mode = memorySaving ? AiSubtitleComputeMode::GpuLowMemory : AiSubtitleComputeMode::GpuHighMemory;
        }
        m_compute->setCurrentIndex(qMax(0, m_compute->findData(aiSubtitleComputeModeKey(mode))));
    }
    setGenerationOptions(options);
    const auto savedRoot = savedPath(settings, QStringLiteral("installRoot"));
    m_root->setText(savedRoot.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Ai/FasterWhisper")) : savedRoot);
    m_loading = false;
    updatePresentation();
}

FasterWhisperInstallWidget::~FasterWhisperInstallWidget()
{
    if (m_saveTimer->isActive()) persistRoot();
    if (m_checkCancelled) m_checkCancelled->store(true);
    if (m_installer) {
        disconnect(m_installer, nullptr, this, nullptr);
        if (m_ownsInstaller) m_installer->shutdown();
        else if (m_installer->isBusy()) m_installer->cancel();
    }
}

bool FasterWhisperInstallWidget::installationBusy() const
{ return m_installer && m_installer->isBusy(); }
bool FasterWhisperInstallWidget::isBusy() const
{ return installationBusy() || m_checking || m_checkTimer->isActive() || m_validationWorkers > 0; }

bool FasterWhisperInstallWidget::isReady() const
{ return !m_shuttingDown && !isBusy() && !m_checkedEngine.isEmpty() && !m_checkedModel.isEmpty(); }
QString FasterWhisperInstallWidget::checkedEngineDirectory() const { return m_checkedEngine; }
QString FasterWhisperInstallWidget::checkedModelDirectory() const { return m_checkedModel; }
QString FasterWhisperInstallWidget::statusText() const { return m_engineStatus->text(); }
QString FasterWhisperInstallWidget::validationState() const
{ return m_engineStatus->property("validationState").toString(); }
AiSubtitleComputeMode FasterWhisperInstallWidget::computeMode() const
{ return aiSubtitleComputeModeFromKey(m_compute->currentData().toString()).value_or(AiSubtitleComputeMode::GpuLowMemory); }
FasterWhisperGenerationOptions FasterWhisperInstallWidget::generationOptions() const
{
    FasterWhisperGenerationOptions options;
    options.preset = fasterWhisperGenerationPresetFromKey(m_preset->currentData().toString())
        .value_or(FasterWhisperGenerationPreset::ComponentDefault);
    options.vadFilter = m_vad->isChecked(); options.vadThreshold = m_threshold->value();
    options.subtitleFormatting = m_subtitleFormatting->isChecked();
    options.beamSize = m_beam->value(); options.conditionOnPreviousText = m_previousText->isChecked();
    return options;
}
void FasterWhisperInstallWidget::setGenerationOptions(const FasterWhisperGenerationOptions &options)
{
    const bool loading = m_loading; m_loading = true;
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(fasterWhisperGenerationPresetKey(options.preset))));
    m_vad->setChecked(options.vadFilter); m_threshold->setValue(options.vadThreshold);
    m_subtitleFormatting->setChecked(options.subtitleFormatting);
    m_beam->setValue(options.beamSize); m_previousText->setChecked(options.conditionOnPreviousText);
    m_loading = loading;
}
void FasterWhisperInstallWidget::persistGenerationOptions()
{
    if (m_loading || m_shuttingDown) return;
    const auto options = generationOptions();
    if (!validateFasterWhisperGenerationOptions(options).isEmpty()) return;
    if (m_settings) {
        m_settings->setValue(Prefix + QStringLiteral("computeMode"), aiSubtitleComputeModeKey(computeMode()));
        m_settings->setValue(Prefix + QStringLiteral("generationPreset"), fasterWhisperGenerationPresetKey(options.preset));
        m_settings->setValue(Prefix + QStringLiteral("vadFilter"), options.vadFilter);
        m_settings->setValue(Prefix + QStringLiteral("vadThreshold"), options.vadThreshold);
        m_settings->remove(Prefix + QStringLiteral("computeType"));
        m_settings->setValue(Prefix + QStringLiteral("subtitleFormatting"), options.subtitleFormatting);
        m_settings->setValue(Prefix + QStringLiteral("beamSize"), options.beamSize);
        m_settings->setValue(Prefix + QStringLiteral("conditionOnPreviousText"), options.conditionOnPreviousText);
        m_settings->sync();
        m_saveError = m_settings->status() == QSettings::NoError ? QString() : QStringLiteral("设置保存失败，请检查设置存储位置是否可写。");
    } else m_saveError = QStringLiteral("设置不可用，无法保存生成参数。");
    updatePresentation();
    emit activityChanged();
}

void FasterWhisperInstallWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_checkStarted) recheck();
}
void FasterWhisperInstallWidget::recheck()
{
    if (!m_shuttingDown && !installationBusy()) scheduleCheck();
}
void FasterWhisperInstallWidget::scheduleCheck()
{
    if (m_loading || m_shuttingDown || installationBusy()) return;
    m_cancelRequested = false;
    m_checkStarted = true;
    if (m_checkCancelled) m_checkCancelled->store(true);
    ++m_generation;
    m_checkedEngine.clear(); m_checkedModel.clear();
    m_engineError.clear(); m_modelError.clear();
    m_checking = true;
    m_checkTimer->start();
    updatePresentation();
    emit activityChanged();
}
void FasterWhisperInstallWidget::startCheck()
{
    if (m_shuttingDown || installationBusy()) return;
    const quint64 generation = m_generation;
    auto token = std::make_shared<std::atomic_bool>(false);
    m_checkCancelled = token;
    const QString engineInput = m_engine->text().trimmed();
    const QString modelInput = m_modelDirectory;
    const QString cachedModel = m_cachedModel;
    const QString cachedIdentity = m_cachedModelIdentity;
    const auto engineValidator = m_engineValidator;
    const auto modelValidator = m_modelValidator;
    auto promise = std::make_shared<QPromise<CheckResult>>();
    auto *watcher = new QFutureWatcher<CheckResult>(this);
    ++m_validationWorkers;
    connect(watcher, &QFutureWatcher<CheckResult>::finished, this, [this, watcher, generation, token] {
        --m_validationWorkers;
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!m_shuttingDown && generation == m_generation && !token->load()) {
            m_checking = false;
            m_checkedEngine = result.engineError.isEmpty() ? result.engine : QString();
            m_checkedModel = result.modelError.isEmpty() ? result.model : QString();
            m_engineError = result.engineError; m_modelError = result.modelError;
            if (!m_checkedModel.isEmpty()) {
                m_modelDirectory = m_checkedModel;
                m_cachedModel = m_checkedModel; m_cachedModelIdentity = result.modelIdentity;
                m_modelDirty = true;
            }
            persistCheckedPaths();
        }
        updatePresentation();
        const QPointer<FasterWhisperInstallWidget> guard(this);
        emit activityChanged();
        if (guard) finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, engineInput, modelInput, token, engineValidator, modelValidator, cachedModel, cachedIdentity] {
        promise->start();
        CheckResult result;
        result.engineInput = engineInput; result.modelInput = modelInput;
        try {
            if (!engineInput.isEmpty()) {
                result.engine = absolutePath(engineInput);
                result.engineError = result.engine.isEmpty() ? QStringLiteral("请选择完整的本地目录路径。")
                    : engineValidator(result.engine, *token);
            }
            if (!token->load() && result.engineError.isEmpty() && !result.engine.isEmpty()) {
                const auto bundled = QDir(result.engine).filePath(QStringLiteral("_models/faster-whisper-large-v2"));
                result.model = QFileInfo(QDir(bundled).filePath(QStringLiteral("model.bin"))).isFile() || modelInput.isEmpty()
                    ? bundled : resolveModel(modelInput);
                result.modelIdentity = modelIdentity(result.model);
                result.modelError = modelMappingError(result.model);
                if (result.modelError.isEmpty()
                    && (result.model != cachedModel || result.modelIdentity.isEmpty() || result.modelIdentity != cachedIdentity))
                    result.modelError = modelValidator(result.model, *token);
                if (result.modelError.isEmpty() && result.modelIdentity != modelIdentity(result.model))
                    result.modelError = QStringLiteral("模型在检查期间发生变化，请重新检查。");
            }
        } catch (const std::exception &exception) {
            result.engineError = result.modelError = QString::fromUtf8(exception.what());
        } catch (...) { result.engineError = result.modelError = QStringLiteral("组件检查出现异常。"); }
        promise->addResult(result); promise->finish();
    });
}
void FasterWhisperInstallWidget::cancelCheck()
{
    m_checkTimer->stop();
    ++m_generation;
    if (m_checkCancelled) m_checkCancelled->store(true);
    m_checking = false;
}
bool FasterWhisperInstallWidget::persistRoot()
{
    const auto root = absolutePath(m_root->text());
    if (root.isEmpty()) { m_saveError = QStringLiteral("安装位置未保存：请选择完整的目录路径。"); return false; }
    if (!m_settings) { m_saveError = QStringLiteral("设置不可用，无法保存安装位置。"); return false; }
    m_settings->setValue(Prefix + QStringLiteral("installRoot"), root);
    m_settings->sync();
    m_saveError = m_settings->status() == QSettings::NoError ? QString()
        : QStringLiteral("设置保存失败，请检查设置存储位置是否可写。");
    return m_saveError.isEmpty();
}
void FasterWhisperInstallWidget::persistCheckedPaths()
{
    if (!m_settings) { m_saveError = QStringLiteral("设置不可用，无法保存组件选择。"); return; }
    if (m_engineDirty && (m_engine->text().trimmed().isEmpty() || !m_checkedEngine.isEmpty())) {
        m_settings->setValue(Prefix + QStringLiteral("engineDirectory"), m_checkedEngine);
        if (m_engine->text().trimmed().isEmpty()) {
            m_modelDirectory.clear(); m_settings->setValue(Prefix + QStringLiteral("modelDirectory"), QString());
        }
        m_engineDirty = false;
    }
    if (m_modelDirty && !m_checkedModel.isEmpty()) {
        m_settings->setValue(Prefix + QStringLiteral("modelDirectory"), m_checkedModel);
        m_modelDirty = false;
    }
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError)
        m_saveError = QStringLiteral("设置保存失败，请检查设置存储位置是否可写。");
    else m_saveError.clear();
}
void FasterWhisperInstallWidget::browse(QLineEdit *target, const QString &title)
{
    if (m_shuttingDown || installationBusy() || m_picker) return;
    auto *picker = new QFileDialog(this, title, target->text());
    picker->setObjectName(QStringLiteral("fasterWhisperDirectoryPicker"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setWindowModality(Qt::WindowModal);
    picker->setFileMode(QFileDialog::Directory);
    picker->setOption(QFileDialog::ShowDirsOnly);
    m_picker = picker;
    connect(picker, &QDialog::finished, this, [this, picker, target](int result) {
        if (!m_shuttingDown && !installationBusy() && result == QDialog::Accepted && !picker->selectedFiles().isEmpty())
            target->setText(picker->selectedFiles().first());
        m_picker = nullptr;
        updatePresentation();
        emit activityChanged();
    });
    picker->open();
    updatePresentation();
    emit activityChanged();
}
void FasterWhisperInstallWidget::closePicker()
{ if (m_picker) m_picker->reject(); }
void FasterWhisperInstallWidget::startInstallation()
{
    if (m_shuttingDown || isBusy() || m_picker || !m_installer) return;
    if (!m_engine->text().trimmed().isEmpty() && m_checkedEngine.isEmpty()) return;
    m_saveTimer->stop();
    if (!persistRoot()) { updatePresentation(); return; }
    FasterWhisperInstallRequest request;
    request.package = fasterWhisperPackage();
    request.installRoot = absolutePath(m_root->text());
    request.existingEngineDirectory = m_checkedEngine;
    request.existingModelDirectory = m_checkedModel;
    m_cancelRequested = false;
    m_feedback.clear();
    const QPointer<FasterWhisperInstallWidget> guard(this);
    m_installer->start(request);
    if (guard) updatePresentation();
}
void FasterWhisperInstallWidget::cancel()
{
    m_cancelRequested = true;
    cancelCheck();
    if (m_installer) m_installer->cancel();
    if (!installationBusy()) m_feedback = QStringLiteral("检查已取消，可重新检查或清空目录后下载。");
    updatePresentation();
    emit activityChanged();
}
void FasterWhisperInstallWidget::applyResult(const FasterWhisperInstallResult &result)
{
    m_cancelRequested = false;
    if (!m_shuttingDown) {
        if (!result.engineDirectory.isEmpty()) {
            const QSignalBlocker blocker(m_engine);
            m_engine->setText(result.engineDirectory);
            m_checkedEngine = result.engineDirectory; m_engineError.clear();
            m_engineDirty = true;
        }
        if (!result.modelDirectory.isEmpty()) {
            m_modelDirectory = result.modelDirectory;
            m_modelError = modelMappingError(result.modelDirectory);
            m_checkedModel = m_modelError.isEmpty() ? result.modelDirectory : QString();
            m_modelDirty = true;
        }
        persistCheckedPaths();
    }
    m_feedback = result.state == State::Ready ? QStringLiteral("程序和 large-v2 模型已准备好。")
        : result.state == State::Cancelled ? QStringLiteral("已取消，下载进度已保留；点击重试可继续。")
        : QStringLiteral("准备失败：%1").arg(result.error);
    if (!result.warnings.isEmpty()) m_feedback += QLatin1Char('\n') + result.warnings.join(QLatin1Char('\n'));
    updatePresentation();
    const QPointer<FasterWhisperInstallWidget> guard(this);
    emit activityChanged();
    if (guard) finishShutdownIfReady();
}
void FasterWhisperInstallWidget::updatePresentation()
{
    const bool installing = installationBusy();
    const bool pending = m_checking || m_checkTimer->isActive();
    const bool editable = !m_shuttingDown && !installing && !m_picker;
    for (auto *edit : {m_engine, m_root}) edit->setEnabled(editable);
    for (auto *button : {m_browseEngine, m_browseRoot}) button->setEnabled(editable);
    if (pending) setStatus(m_engineStatus, m_engineIcon, QStringLiteral("pending"), QStringLiteral("正在检查组件和 large-v2 模型…"));
    else if (m_cancelRequested) setStatus(m_engineStatus, m_engineIcon, QStringLiteral("cancelled"), QStringLiteral("组件检查已取消，可重新检查。"));
    else if (m_engine->text().trimmed().isEmpty())
        setStatus(m_engineStatus, m_engineIcon, QStringLiteral("unselected"), QStringLiteral("未选择组件，可展开“安装组件”下载安装。"));
    else if (!m_checkedEngine.isEmpty() && !m_checkedModel.isEmpty()) {
        setStatus(m_engineStatus, m_engineIcon, QStringLiteral("passed"), QStringLiteral("组件检查通过"));
        m_engineStatus->setToolTip(QStringLiteral("large-v2：%1").arg(m_checkedModel));
    } else setStatus(m_engineStatus, m_engineIcon, QStringLiteral("failed"),
        !m_engineError.isEmpty() ? QStringLiteral("程序检查未通过：%1").arg(m_engineError)
            : QStringLiteral("large-v2 模型未就绪：%1\n可展开“安装组件”准备缺失模型。").arg(m_modelError.isEmpty() ? QStringLiteral("请重新检查") : m_modelError));
    static_cast<StatusRow *>(m_engineStatus->parentWidget())->refreshHeight();
    const bool validSelection = m_engine->text().trimmed().isEmpty() || !m_checkedEngine.isEmpty();
    const auto preset = generationOptions().preset;
    m_presetHint->setText(preset == FasterWhisperGenerationPreset::QuietSpeech
        ? QStringLiteral("检测阈值 0.35：更容易保留轻声，也可能保留更多噪声。")
        : preset == FasterWhisperGenerationPreset::Conservative
        ? QStringLiteral("检测阈值 0.55：更严格地筛选语音，也可能遗漏轻声。")
        : preset == FasterWhisperGenerationPreset::Custom
        ? QStringLiteral("按下方设置生成字幕；关闭语音检测后，VAD 阈值不生效。")
        : QStringLiteral("使用组件默认生成参数，计算精度由运行方式决定。"));
    const bool customVisible = preset == FasterWhisperGenerationPreset::Custom;
    const bool customChanged = customVisible != !m_customParameters->isHidden();
    m_customParameters->setVisible(customVisible);
    if (customChanged) {
        auto *parameters = m_customParameters->parentWidget();
        parameters->layout()->invalidate();
        parameters->layout()->activate();
        parameters->updateGeometry();
    }
    m_threshold->setEnabled(m_vad->isChecked());
    m_threshold->setToolTip(m_vad->isChecked()
        ? QStringLiteral("较低更容易保留轻声，也可能保留更多噪声。")
        : QStringLiteral("语音检测已关闭，此阈值不生效；重新开启后恢复当前数值。"));
    m_compute->setEnabled(editable); m_preset->setEnabled(editable); m_reset->setEnabled(editable);
    m_customParameters->setEnabled(editable);
    const auto state = m_installer ? m_installer->result().state : State::Idle;
    const bool bothReady = !m_checkedEngine.isEmpty() && !m_checkedModel.isEmpty();
    m_prepare->setText(bothReady ? QStringLiteral("组件已就绪")
        : state == State::Failed || state == State::Cancelled ? QStringLiteral("重试准备") : QStringLiteral("下载并准备"));
    m_prepare->setEnabled(editable && !isBusy() && validSelection && !bothReady && !absolutePath(m_root->text()).isEmpty() && m_installer);
    m_check->setEnabled(editable && !isBusy());
    m_cancel->setVisible(isBusy());
    if (installing && !m_installToggle->isChecked()) m_installToggle->setChecked(true);
    m_cancel->setEnabled(!m_shuttingDown && isBusy() && !m_cancelRequested);
    m_saveStatus->setText(m_saveError); m_saveStatus->setVisible(!m_saveError.isEmpty());
    const auto progress = m_installer ? m_installer->progress() : FasterWhisperInstallProgress();
    const bool extracting = progress.state == State::Extracting;
    m_status->setText(installing ? (m_cancelRequested ? QStringLiteral("正在取消…")
        : extracting ? QStringLiteral("正在解压并检查程序…") : progress.message) : m_feedback);
    m_status->setVisible(!m_status->text().isEmpty());
    m_progress->setVisible(installing);
    m_bytes->setVisible(installing && !extracting && progress.totalBytes > 0);
    if (!extracting && progress.totalBytes > 0) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(qBound(0, int(1000.0 * double(progress.completedBytes) / double(progress.totalBytes)), 1000));
        m_bytes->setText((progress.component == QStringLiteral("model") ? QStringLiteral("模型：") : QStringLiteral("程序："))
            + bytesText(progress.completedBytes) + QStringLiteral(" / ") + bytesText(progress.totalBytes));
    } else m_progress->setRange(0, 0);
    updateGeometry();
}
void FasterWhisperInstallWidget::shutdown()
{
    if (!m_shuttingDown) {
        m_shuttingDown = true;
        m_shutdownStarting = true;
        if (m_saveTimer->isActive()) { m_saveTimer->stop(); persistRoot(); }
        cancelCheck();
        const QPointer<FasterWhisperInstallWidget> guard(this);
        closePicker();
        if (!guard) return;
        if (m_installer) m_installer->shutdown();
        if (!guard) return;
        m_shutdownStarting = false;
    }
    updatePresentation();
    finishShutdownIfReady();
}
void FasterWhisperInstallWidget::finishShutdownIfReady()
{
    if (m_shuttingDown && !m_shutdownStarting && !isBusy() && !m_shutdownEmitted) {
        m_shutdownEmitted = true;
        emit shutdownFinished();
    }
}
} // namespace asmr
