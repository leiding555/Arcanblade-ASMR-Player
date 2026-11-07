#include "ui/ChickenRiceTranslateDialog.h"
#include "ui/FasterWhisperInstallWidget.h"
#include "ui/SubtitleTranslationWidget.h"
#include "ui/TranslationApiSettingsWidget.h"

#include "ui/DialogFrame.h"
#include "ai/ChickenRiceArchive.h"
#include "ui/PlayerTheme.h"
#include "ui/NavigationIcons.h"

#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPromise>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSettings>
#include <QScrollBar>
#include <QScreen>
#include <QTextOption>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <exception>
#include <cmath>
#include <limits>

namespace asmr {
namespace {
constexpr int MaximumQueueHistoryItems = 5000;
// A preparation page can be considerably taller than the task page. Only the
// visible page contributes to the scroll host's size, including after a switch.
class CurrentPageStack final : public QStackedWidget {
public:
    explicit CurrentPageStack(QWidget *parent = nullptr) : QStackedWidget(parent)
    { layout()->setSizeConstraint(QLayout::SetNoConstraint); }
    QSize sizeHint() const override
    { return currentWidget() ? currentWidget()->sizeHint() : QSize(); }
    QSize minimumSizeHint() const override
    { return currentWidget() ? currentWidget()->minimumSizeHint().expandedTo(currentWidget()->minimumSize()) : QSize(); }
    bool hasHeightForWidth() const override
    { return currentWidget() && currentWidget()->hasHeightForWidth(); }
    int heightForWidth(int width) const override
    {
        auto *page = currentWidget();
        if (!page) return -1;
        return page->hasHeightForWidth() ? page->heightForWidth(width) : page->sizeHint().height();
    }
};

// QScrollArea asks its content layout for height-for-width. QStackedLayout
// deliberately takes the tallest of every page, including hidden installation
// forms, so do not route that query through the stack's internal layout.
class CurrentPageContentLayout final : public QVBoxLayout {
public:
    explicit CurrentPageContentLayout(QStackedWidget *pages) : m_pages(pages) {}
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        const auto margins = contentsMargins();
        const auto *page = m_pages->currentWidget();
        const int pageWidth = std::max(0, width - margins.left() - margins.right());
        const int pageHeight = !page ? 0 : page->hasHeightForWidth()
            ? page->heightForWidth(pageWidth) : page->sizeHint().height();
        return std::max(0, pageHeight) + margins.top() + margins.bottom();
    }
private:
    QStackedWidget *m_pages;
};

// QFormLayout may stretch a QWidget field vertically even with Maximum
// policy. Give status fields an actual height bound, recomputed for wrapped
// text whenever their width or content changes.
class ComponentStatusRow final : public QWidget {
public:
    explicit ComponentStatusRow(QWidget *parent = nullptr) : QWidget(parent)
    { setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum); }

    void refreshHeight()
    {
        if (!layout() || width() <= 0) return;
        const int naturalHeight = layout()->hasHeightForWidth()
            ? layout()->heightForWidth(width()) : layout()->sizeHint().height();
        const int height = std::max(layout()->minimumSize().height(), naturalHeight);
        if (height >= 0 && (minimumHeight() != height || maximumHeight() != height)) {
            setFixedHeight(height);
            updateGeometry();
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        refreshHeight();
    }
    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::FontChange
            || event->type() == QEvent::StyleChange)
            refreshHeight();
        return handled;
    }
};

using State = AiSubtitleTaskState;
const QString Prefix = QStringLiteral("Ai/ChickenRice/");
const QString GenerationSettingsKey = Prefix + QStringLiteral("generationOptions");

// Draw the marks instead of depending on a font's coverage of check/cross glyphs.
QLabel *statusIcon(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setFixedSize(18, 18);
    label->setAlignment(Qt::AlignCenter);
    label->hide();
    return label;
}

void updateStatusIcon(QLabel *label, const QString &state)
{
    const bool passed = state == QStringLiteral("passed");
    const bool failed = state == QStringLiteral("failed");
    if (label->property("validationState").toString() != state) {
        label->setProperty("validationState", state);
        if (passed || failed) {
            const qreal ratio = label->devicePixelRatioF();
            QPixmap pixmap(QSize(qRound(18 * ratio), qRound(18 * ratio)));
            pixmap.setDevicePixelRatio(ratio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(passed ? QColor("#48bb92") : QColor("#ed7f86"),
                                2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (passed) {
                QPainterPath path;
                path.moveTo(3, 9);
                path.lineTo(7, 13);
                path.lineTo(15, 5);
                painter.drawPath(path);
            } else {
                painter.drawLine(QPointF(4, 4), QPointF(14, 14));
                painter.drawLine(QPointF(14, 4), QPointF(4, 14));
            }
            painter.end();
            label->setPixmap(pixmap);
        } else {
            label->clear();
        }
    }
    label->setAccessibleName(passed ? QStringLiteral("组件检查通过")
                            : failed ? QStringLiteral("组件检查失败") : QString());
    label->setVisible(passed || failed);
}

QString normalizedPath(const QString &input)
{
    const QString path = QDir::fromNativeSeparators(input.trimmed());
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)
        || path.startsWith(QStringLiteral("//?/")) || path.startsWith(QStringLiteral("//./")))
        return {};
    return QDir::cleanPath(path);
}

bool samePath(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty())
        return false;
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

bool validOutputComponent(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"));
    return value.isEmpty() || expression.match(value).hasMatch();
}

QString outputNameKey(const QString &audioPath)
{
    QString key = QDir(QFileInfo(audioPath).absolutePath()).filePath(QFileInfo(audioPath).completeBaseName());
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

QString defaultRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Ai/ChickenRice"));
}

QString stringSetting(QSettings *settings, const QString &name)
{
    if (!settings)
        return {};
    const QVariant value = settings->value(Prefix + name);
    return value.metaType().id() == QMetaType::QString ? value.toString() : QString();
}

QLabel *plainLabel(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

// Keep potentially unbounded engine diagnostics out of QMessageBox's fixed
// text grid. The full plain text stays selectable and scrollable at any width.
class TaskErrorDialog final : public QDialog {
public:
    TaskErrorDialog(const QString &error, const QString &audioPath,
                    const QString &logPath, const QStringList &warnings, QWidget *parent)
        : QDialog(parent)
    {
        setObjectName(QStringLiteral("translateErrorDialog"));
        setWindowTitle(QStringLiteral("字幕生成失败"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowModality(Qt::WindowModal);
        auto *body = new QVBoxLayout;
        body->setContentsMargins(20, 16, 20, 16);
        body->setSpacing(12);
        auto *summary = plainLabel(QStringLiteral("translateErrorSummary"), this);
        const QString fullError = error.isEmpty()
            ? QStringLiteral("生成未完成，请查看任务日志。") : error;
        QString firstLine = fullError.section(QLatin1Char('\n'), 0, 0).trimmed();
        if (firstLine.isEmpty()) firstLine = QStringLiteral("字幕生成未完成。");
        summary->setText(firstLine.size() > 180 ? firstLine.left(180) + QStringLiteral("…") : firstLine);
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        body->addWidget(summary);
        auto *hint = plainLabel(QStringLiteral("translateErrorHint"), this);
        hint->setText(QStringLiteral("可查看完整详情和日志位置，调整组件或生成参数后重试。"));
        body->addWidget(hint);
        m_details = new QPlainTextEdit(this);
        m_details->setObjectName(QStringLiteral("translateErrorDetails"));
        m_details->setReadOnly(true);
        m_details->setWordWrapMode(QTextOption::WrapAnywhere);
        m_details->setMinimumSize(0, 160);
        QString details = QStringLiteral("错误：\n") + fullError
            + QStringLiteral("\n\n音轨：\n") + audioPath;
        if (!logPath.isEmpty()) details += QStringLiteral("\n\n日志：\n") + logPath;
        if (!warnings.isEmpty()) details += QStringLiteral("\n\n警告：\n") + warnings.join(QLatin1Char('\n'));
        m_details->setPlainText(details);
        m_details->hide();
        body->addWidget(m_details, 1);
        auto *buttons = new QHBoxLayout;
        auto *toggle = new QPushButton(QStringLiteral("查看详情"), this);
        toggle->setObjectName(QStringLiteral("translateErrorDetailsButton"));
        auto *close = new QPushButton(QStringLiteral("关闭"), this);
        close->setObjectName(QStringLiteral("translateErrorCloseButton"));
        close->setDefault(true);
        buttons->addWidget(toggle);
        buttons->addStretch();
        buttons->addWidget(close);
        // Keep collapsed labels together; extra height belongs above the buttons.
        body->addStretch(0);
        body->addLayout(buttons);
        setMinimumWidth(320);
        resize(640, 220);
        m_frame = new DialogFrame(this, body);
        connect(toggle, &QPushButton::clicked, this, [this, toggle] {
            const bool expanded = m_details->isHidden();
            m_details->setVisible(expanded);
            toggle->setText(expanded ? QStringLiteral("收起详情") : QStringLiteral("查看详情"));
            auto *content = m_frame->contentWidget();
            content->layout()->invalidate();
            content->setMinimumHeight(content->layout()->totalMinimumSize().height());
            const QSize limit = screen() ? (screen()->availableGeometry().size() - QSize(32, 32))
                .expandedTo(QSize(1, 1)) : QSize(800, 600);
            resize(QSize(width(), expanded ? 520 : 260).boundedTo(limit).expandedTo(minimumSize()));
            if (screen()) {
                const QRect bounds = screen()->availableGeometry().adjusted(16, 16, -16, -16);
                move(std::clamp(x(), bounds.left(), std::max(bounds.left(), bounds.right() - width() + 1)),
                     std::clamp(y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - height() + 1)));
            }
            if (expanded) m_details->setFocus(Qt::OtherFocusReason);
        });
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
    }
protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        return m_frame && m_frame->handleNativeEvent(type, message, result)
            ? true : QDialog::nativeEvent(type, message, result);
    }
private:
    DialogFrame *m_frame = nullptr;
    QPlainTextEdit *m_details = nullptr;
};

QString stateText(State state)
{
    switch (state) {
    case State::Idle: return QStringLiteral("待执行");
    case State::Preparing: return QStringLiteral("准备任务");
    case State::CheckingModel: return QStringLiteral("校验模型");
    case State::Running: return QStringLiteral("正在生成");
    case State::Validating: return QStringLiteral("正在检查字幕");
    case State::Succeeded: return QStringLiteral("已完成");
    case State::Cancelled: return QStringLiteral("已取消");
    case State::Failed: return QStringLiteral("失败");
    }
    return {};
}
} // namespace

struct QueueHistoryLoadResult {
    AiTaskSnapshot snapshot;
    QString error;
    std::shared_ptr<QLockFile> lease;
};

// Workers own their database connection. Pending row snapshots coalesce in
// one mailbox, preventing an older writer from overwriting a newer snapshot.
struct ChickenRiceTranslateDialog::QueueHistoryWriter {
    QMutex mutex;
    std::optional<AiTaskSnapshot> pending;
    QString lastError;
    bool running = false;
};

ChickenRiceTranslateDialog::ChickenRiceTranslateDialog(
    QSettings *settings, QWidget *parent, ChickenRiceTranslateService *service,
    GpuRecommendationDialog *componentPanel, GpuRecommendationDialog::Probe probe, EngineValidator validator,
    FasterWhisperTranscribeService *transcribeService, FasterWhisperInstallWidget *fasterWhisperPanel,
    TranslationApiSettingsWidget *translationSettingsPanel, QString databasePath)
    : QDialog(parent), m_settings(settings), m_gpuProbe(std::move(probe)),
      m_validator(std::move(validator)), m_service(service), m_transcribeService(transcribeService)
{
    m_taskDatabasePath = aiTaskDatabasePath(settings,databasePath);
    m_queueHistoryWriter = std::make_shared<QueueHistoryWriter>();
    m_queueHistoryTimer = new QTimer(this);
    m_queueHistoryTimer->setSingleShot(true);
    m_queueHistoryTimer->setInterval(50);
    connect(m_queueHistoryTimer, &QTimer::timeout, this, &ChickenRiceTranslateDialog::persistQueueHistory);
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setObjectName(QStringLiteral("translateElapsedTimer"));
    m_elapsedTimer->setInterval(1000);
    connect(m_elapsedTimer, &QTimer::timeout, this, &ChickenRiceTranslateDialog::updateProgressFeedback);
    if (!m_service) {
        m_service = new ChickenRiceTranslateService(this);
        m_ownsService = true;
    }
    setObjectName(QStringLiteral("chickenRiceTranslateDialog"));
    setWindowTitle(QStringLiteral("AI 字幕"));
    setWindowModality(Qt::NonModal);
    setStyleSheet(playerStyleSheet());
    resize(680, 560);

    m_pages = new CurrentPageStack(this);
    m_pages->setObjectName(QStringLiteral("translatePages"));
    m_generationPage = new QWidget(m_pages);
    m_generationPage->setObjectName(QStringLiteral("translateGenerationPage"));
    m_settingsPage = new QWidget(m_pages);
    m_settingsPage->setObjectName(QStringLiteral("translateSettingsPage"));
    m_pages->addWidget(m_generationPage);
    m_pages->addWidget(m_settingsPage);

    m_modeCombo = new QComboBox(m_generationPage);
    m_modeCombo->setObjectName(QStringLiteral("translateMode"));
    m_modeCombo->addItem(QStringLiteral("离线直译"), QStringLiteral("offline-direct"));
    m_modeCombo->addItem(QStringLiteral("日语识别"), QStringLiteral("offline-asr-ja"));
    m_modeCombo->addItem(QStringLiteral("字幕翻译"), QStringLiteral("cloud-translation"));
    m_componentSettingsButton = new QPushButton(QStringLiteral("组件与设置"), m_generationPage);
    m_componentSettingsButton->setObjectName(QStringLiteral("translateComponentSettingsButton"));
    auto *heading = new QHBoxLayout;
    heading->addWidget(new QLabel(QStringLiteral("生成方式"), m_generationPage));
    heading->addWidget(m_modeCombo);
    heading->addStretch();
    heading->addWidget(m_componentSettingsButton);
    m_modeConfiguration = new CurrentPageStack(m_generationPage);
    m_modeConfiguration->setObjectName(QStringLiteral("translateModeConfiguration"));
    auto *offline = new QWidget(m_modeConfiguration);
    offline->setProperty("modeId", QStringLiteral("offline-direct"));
    m_modeConfiguration->addWidget(offline);
    auto *offlineLayout = new QVBoxLayout(offline);
    offlineLayout->setContentsMargins(0, 0, 0, 0);
    m_introduction = plainLabel(QStringLiteral("translateIntroduction"), offline);
    m_introduction->setProperty("secondaryText", true);
    offlineLayout->addWidget(m_introduction);
    auto *generationStatusRow = new ComponentStatusRow(offline);
    generationStatusRow->setObjectName(QStringLiteral("translateComponentStatusRow"));
    generationStatusRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_componentStatus = plainLabel(QStringLiteral("translateComponentStatus"), generationStatusRow);
    m_componentStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto statusTextPolicy = m_componentStatus->sizePolicy();
    statusTextPolicy.setHeightForWidth(true);
    m_componentStatus->setSizePolicy(statusTextPolicy);
    m_componentStatusIcon = statusIcon(QStringLiteral("translateComponentStatusIcon"), generationStatusRow);
    auto *generationStatusLayout = new QHBoxLayout(generationStatusRow);
    generationStatusLayout->setContentsMargins(0, 0, 0, 0);
    generationStatusLayout->setSpacing(6);
    generationStatusLayout->setAlignment(Qt::AlignTop);
    generationStatusLayout->addWidget(m_componentStatusIcon, 0, Qt::AlignTop);
    generationStatusLayout->addWidget(m_componentStatus, 1, Qt::AlignTop);
    offlineLayout->addWidget(generationStatusRow);

    m_table = new QTableWidget(0, 2, m_generationPage);
    m_table->setObjectName(QStringLiteral("translateQueueTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("音轨"), QStringLiteral("状态")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setMinimumSize(0, 140);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *context = new QMenu(m_table);
    context->setObjectName(QStringLiteral("translateQueueContextMenu"));
    m_removeAction = context->addAction(QStringLiteral("从列表移除"));
    m_removeAction->setObjectName(QStringLiteral("translateRemoveAction"));
    m_openFolderAction = context->addAction(QStringLiteral("打开字幕所在文件夹"));
    m_openFolderAction->setObjectName(QStringLiteral("translateOpenFolderAction"));
    m_errorAction = context->addAction(QStringLiteral("查看错误"));
    m_errorAction->setObjectName(QStringLiteral("translateErrorAction"));
    m_translateSubtitleAction = context->addAction(QStringLiteral("翻译成中文…"));
    m_translateSubtitleAction->setObjectName(QStringLiteral("translateJapaneseResultAction"));
    m_summary = plainLabel(QStringLiteral("translateQueueSummary"), m_generationPage);
    m_status = plainLabel(QStringLiteral("translateStatus"), m_generationPage);
    m_queueHistoryStatus = plainLabel(QStringLiteral("translateQueueHistoryStatus"), m_generationPage);
    m_queueHistoryStatus->hide();
    m_activity = new QProgressBar(m_generationPage);
    m_activity->setObjectName(QStringLiteral("translateActivity"));
    m_activity->setTextVisible(false);
    m_activity->setMaximumHeight(14);

    m_addAudioButton = new QPushButton(QStringLiteral("添加音频…"), m_generationPage);
    m_addAudioButton->setObjectName(QStringLiteral("translateAddAudioButton"));
    m_clearButton = new QPushButton(QStringLiteral("清空列表"), m_generationPage);
    m_clearButton->setObjectName(QStringLiteral("translateClearButton"));
    m_clearAction = new QAction(QStringLiteral("清空列表"), this);
    m_clearAction->setObjectName(QStringLiteral("translateClearAction"));
    m_clearAction->setToolTip(QStringLiteral("仅清空当前生成方式的列表，已生成的字幕文件保留。"));
    m_clearButton->setToolTip(m_clearAction->toolTip());
    m_startButton = new QPushButton(QStringLiteral("开始生成"), m_generationPage);
    m_startButton->setObjectName(QStringLiteral("translateStartButton"));
    m_startButton->setProperty("primaryAction", true);
    m_retryButton = new QPushButton(QStringLiteral("重试未完成项"), m_generationPage);
    m_retryButton->setObjectName(QStringLiteral("translateRetryButton"));
    m_cancelButton = new QPushButton(QStringLiteral("取消任务"), m_generationPage);
    m_cancelButton->setObjectName(QStringLiteral("translateCancelButton"));
    m_translateSubtitleButton = new QPushButton(QStringLiteral("前往字幕翻译"), m_generationPage);
    m_translateSubtitleButton->setObjectName(QStringLiteral("translateJapaneseResultButton"));
    m_openResultFolderButton = new QPushButton(QStringLiteral("打开结果文件夹"), m_generationPage);
    m_openResultFolderButton->setObjectName(QStringLiteral("translateOpenResultFolderButton"));
    auto *queueButtons = new QHBoxLayout;
    m_queueButtons = queueButtons;
    queueButtons->addWidget(m_addAudioButton);
    queueButtons->addWidget(m_clearButton);
    queueButtons->addWidget(m_translateSubtitleButton);
    queueButtons->addStretch();
    queueButtons->addWidget(m_openResultFolderButton);
    queueButtons->addWidget(m_retryButton);
    queueButtons->addWidget(m_startButton);
    queueButtons->addWidget(m_cancelButton);

    auto *generationLayout = new QVBoxLayout(m_generationPage);
    generationLayout->setContentsMargins(0, 0, 0, 0);
    generationLayout->setSpacing(10);
    generationLayout->addLayout(heading);
    m_subtitleTranslation = new SubtitleTranslationWidget(m_settings, m_generationPage, nullptr, m_taskDatabasePath);
    generationLayout->addWidget(m_subtitleTranslation, 1);
    m_subtitleTranslation->hide();
    connect(m_subtitleTranslation, &SubtitleTranslationWidget::settingsRequested,
            this, &ChickenRiceTranslateDialog::showComponentSettings);
    connect(m_subtitleTranslation, &SubtitleTranslationWidget::batchCompleted,
            this, &ChickenRiceTranslateDialog::batchFinished);
    connect(m_subtitleTranslation, &SubtitleTranslationWidget::busyChanged, this, [this] {
        updatePresentation();
        finishShutdownIfReady();
    });
    connect(m_subtitleTranslation, &SubtitleTranslationWidget::shutdownFinished,
            this, &ChickenRiceTranslateDialog::finishShutdownIfReady);
    m_modeConfiguration->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    generationLayout->addWidget(m_modeConfiguration, 0, Qt::AlignTop);
    generationLayout->addWidget(m_table, 1);
    generationLayout->addWidget(m_summary);
    generationLayout->addWidget(m_status);
    generationLayout->addWidget(m_queueHistoryStatus);
    generationLayout->addWidget(m_activity);
    generationLayout->addLayout(queueButtons);

    auto *settingsLayout = new QVBoxLayout(m_settingsPage);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(10);
    settingsLayout->setAlignment(Qt::AlignTop);
    m_settingsHeading = plainLabel(QStringLiteral("translateSettingsHeading"), m_settingsPage);
    auto *backButton = new QPushButton(QStringLiteral("返回"), m_settingsPage);
    backButton->setObjectName(QStringLiteral("translateBackButton"));
    backButton->setIcon(navigationChevronIcon(Qt::LeftArrow));
    backButton->setIconSize(QSize(18, 18));
    auto *settingsHeadingRow = new QHBoxLayout;
    settingsHeadingRow->addWidget(backButton);
    settingsHeadingRow->addWidget(m_settingsHeading, 1);
    settingsLayout->addLayout(settingsHeadingRow);
    m_componentPages = new CurrentPageStack(m_settingsPage);
    m_componentPages->setObjectName(QStringLiteral("translateComponentPages"));
    auto *offlineSettings = new QWidget(m_componentPages);
    auto *offlineSettingsLayout = new QVBoxLayout(offlineSettings);
    offlineSettingsLayout->setContentsMargins(0, 0, 0, 0);
    offlineSettingsLayout->setSpacing(10);
    offlineSettingsLayout->setAlignment(Qt::AlignTop);
    offlineSettings->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_componentPages->addWidget(offlineSettings);
    m_fasterWhisperPage = new QWidget(m_componentPages);
    m_fasterWhisperPage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_fasterWhisperLayout = new QVBoxLayout(m_fasterWhisperPage);
    m_fasterWhisperLayout->setContentsMargins(0, 0, 0, 0);
    m_fasterWhisperLayout->setAlignment(Qt::AlignTop);
    m_componentPages->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_componentPages->addWidget(m_fasterWhisperPage);
    if (translationSettingsPanel) attachTranslationSettingsPanel(translationSettingsPanel);
    settingsLayout->addWidget(m_componentPages, 0, Qt::AlignTop);
    settingsLayout->addStretch(1);
    m_engineDirectory = new QLineEdit(m_settingsPage);
    m_engineDirectory->setObjectName(QStringLiteral("translateEngineDirectory"));
    m_engineDirectory->setMinimumWidth(0);
    m_engineDirectory->setPlaceholderText(QStringLiteral("选择包含 infer.exe 的已解压组件目录"));
    m_browseEngineButton = new QPushButton(QStringLiteral("选择已有组件…"), m_settingsPage);
    m_browseEngineButton->setObjectName(QStringLiteral("translateBrowseEngineButton"));
    auto *engineRow = new QHBoxLayout;
    engineRow->addWidget(m_engineDirectory, 1);
    engineRow->addWidget(m_browseEngineButton);
    m_computeMode = new QComboBox(m_settingsPage);
    m_computeMode->setObjectName(QStringLiteral("translateComputeMode"));
    m_computeMode->addItem(QStringLiteral("GPU（低显存）"), aiSubtitleComputeModeKey(AiSubtitleComputeMode::GpuLowMemory));
    m_computeMode->addItem(QStringLiteral("GPU（高显存）"), aiSubtitleComputeModeKey(AiSubtitleComputeMode::GpuHighMemory));
    m_computeMode->addItem(QStringLiteral("CPU"), aiSubtitleComputeModeKey(AiSubtitleComputeMode::Cpu));
    m_computeMode->setItemData(0, QStringLiteral("使用 int8_float16，保持低显存推理。"), Qt::ToolTipRole);
    m_computeMode->setItemData(1, QStringLiteral("使用官方高显存批处理模式，最大内部批量为 8。"), Qt::ToolTipRole);
    m_computeMode->setItemData(2, QStringLiteral("主翻译模型使用 CPU/int8；引擎的 VAD 仍可能使用可用 GPU。"), Qt::ToolTipRole);
    auto *form = new QFormLayout;
    form->setFormAlignment(Qt::AlignTop);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("当前组件目录"), engineRow);
    auto *settingsStatusRow = new ComponentStatusRow(m_settingsPage);
    settingsStatusRow->setObjectName(QStringLiteral("translateSettingsComponentStatusRow"));
    settingsStatusRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *settingsStatusLayout = new QHBoxLayout(settingsStatusRow);
    settingsStatusLayout->setContentsMargins(0, 0, 0, 0);
    settingsStatusLayout->setSpacing(6);
    settingsStatusLayout->setAlignment(Qt::AlignTop);
    m_settingsComponentStatus = plainLabel(QStringLiteral("translateSettingsComponentStatus"), settingsStatusRow);
    m_settingsComponentStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_settingsComponentStatus->setSizePolicy(statusTextPolicy);
    m_settingsComponentStatusIcon = statusIcon(QStringLiteral("translateSettingsComponentStatusIcon"), settingsStatusRow);
    settingsStatusLayout->addWidget(m_settingsComponentStatusIcon, 0, Qt::AlignTop);
    settingsStatusLayout->addWidget(m_settingsComponentStatus, 1, Qt::AlignTop);
    form->addRow(QString(), settingsStatusRow);
    form->addRow(QStringLiteral("运行方式"), m_computeMode);
    offlineSettingsLayout->addLayout(form);

    m_generationParameters = new QWidget(m_settingsPage);
    m_generationParameters->setObjectName(QStringLiteral("translateGenerationParameters"));
    m_generationParameters->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *parametersLayout = new QVBoxLayout(m_generationParameters);
    parametersLayout->setContentsMargins(0, 0, 0, 0);
    parametersLayout->setSpacing(8);
    auto *parametersHeading = plainLabel(QStringLiteral("translateGenerationHeading"), m_generationParameters);
    parametersHeading->setText(QStringLiteral("生成参数"));
    parametersHeading->setProperty("sectionTitle", true);
    parametersLayout->addWidget(parametersHeading);
    m_generationPreset = new QComboBox(m_generationParameters);
    m_generationPreset->setObjectName(QStringLiteral("translateGenerationPreset"));
    m_generationPreset->setMinimumWidth(0);
    m_generationPreset->addItem(QStringLiteral("标准（组件默认）"), QStringLiteral("component-default"));
    m_generationPreset->addItem(QStringLiteral("轻声优先"), QStringLiteral("quiet-speech"));
    m_generationPreset->addItem(QStringLiteral("减少误识别"), QStringLiteral("conservative"));
    m_generationPreset->addItem(QStringLiteral("自定义"), QStringLiteral("custom"));
    m_generationPreset->setToolTip(QStringLiteral("标准使用组件原有配置；其他预设仅调整语音检测阈值，自定义可分别调整。"));
    m_restoreGenerationButton = new QPushButton(QStringLiteral("恢复默认"), m_generationParameters);
    m_restoreGenerationButton->setObjectName(QStringLiteral("translateRestoreGenerationButton"));
    m_restoreGenerationButton->setToolTip(QStringLiteral("恢复生成参数为组件默认，不修改组件目录、运行方式或组件文件。"));
    auto *presetRow = new QHBoxLayout;
    presetRow->addWidget(m_generationPreset, 1);
    presetRow->addWidget(m_restoreGenerationButton);
    parametersLayout->addLayout(presetRow);
    m_generationPresetHint = plainLabel(QStringLiteral("translateGenerationPresetHint"), m_generationParameters);
    m_generationPresetHint->setProperty("secondaryText", true);
    parametersLayout->addWidget(m_generationPresetHint);
    m_generationSaveStatus = plainLabel(QStringLiteral("translateGenerationSaveStatus"), m_generationParameters);
    m_generationSaveStatus->hide();
    parametersLayout->addWidget(m_generationSaveStatus);

    m_customGenerationParameters = new QWidget(m_generationParameters);
    m_customGenerationParameters->setObjectName(QStringLiteral("translateCustomGenerationOptions"));
    auto *customForm = new QFormLayout(m_customGenerationParameters);
    customForm->setContentsMargins(0, 0, 0, 0);
    customForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    customForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_vadThreshold = new QDoubleSpinBox(m_customGenerationParameters);
    m_vadThreshold->setObjectName(QStringLiteral("translateVadThreshold"));
    m_vadThreshold->setRange(ChickenRiceVadThresholdMinimum, ChickenRiceVadThresholdMaximum);
    m_vadThreshold->setDecimals(2);
    m_vadThreshold->setSingleStep(0.05);
    m_vadThreshold->setToolTip(QStringLiteral("越低越容易捕捉轻声，也越容易误识别；当前官方默认值为 0.5。"));
    m_minSpeechDuration = new QSpinBox(m_customGenerationParameters);
    m_minSpeechDuration->setObjectName(QStringLiteral("translateMinSpeechDuration"));
    m_minSpeechDuration->setRange(ChickenRiceMinSpeechDurationMinimumMs, ChickenRiceMinSpeechDurationMaximumMs);
    m_minSpeechDuration->setSingleStep(50);
    m_minSpeechDuration->setSuffix(QStringLiteral(" ms"));
    m_minSpeechDuration->setToolTip(QStringLiteral("越短越容易保留短促应答，也可能保留更多杂音；当前官方默认值为 300 ms。"));
    m_minSilenceDuration = new QSpinBox(m_customGenerationParameters);
    m_minSilenceDuration->setObjectName(QStringLiteral("translateMinSilenceDuration"));
    m_minSilenceDuration->setRange(ChickenRiceMinSilenceDurationMinimumMs, ChickenRiceMinSilenceDurationMaximumMs);
    m_minSilenceDuration->setSingleStep(50);
    m_minSilenceDuration->setSuffix(QStringLiteral(" ms"));
    m_minSilenceDuration->setToolTip(QStringLiteral("根据多长的停顿区分语音片段；调大更容易连接相邻语音，当前官方默认值为 100 ms。"));
    m_speechPad = new QSpinBox(m_customGenerationParameters);
    m_speechPad->setObjectName(QStringLiteral("translateSpeechPad"));
    m_speechPad->setRange(ChickenRiceSpeechPadMinimumMs, ChickenRiceSpeechPadMaximumMs);
    m_speechPad->setSingleStep(50);
    m_speechPad->setSuffix(QStringLiteral(" ms"));
    m_speechPad->setToolTip(QStringLiteral("在语音前后保留音频，句头句尾被切掉时可适当增加；当前官方默认值为 200 ms。"));
    m_mergeSegments = new QCheckBox(QStringLiteral("合并相邻重复字幕"), m_customGenerationParameters);
    m_mergeSegments->setObjectName(QStringLiteral("translateMergeSegments"));
    m_mergeSegments->setToolTip(QStringLiteral("合并符合组件条件的相邻重复片段；不删除整部音频中相隔较远的重复台词。"));
    customForm->addRow(QStringLiteral("语音检测阈值"), m_vadThreshold);
    customForm->addRow(QStringLiteral("最短语音时长"), m_minSpeechDuration);
    customForm->addRow(QStringLiteral("分句静音间隔"), m_minSilenceDuration);
    customForm->addRow(QStringLiteral("语音首尾保留"), m_speechPad);
    customForm->addRow(QString(), m_mergeSegments);
    parametersLayout->addWidget(m_customGenerationParameters);
    offlineSettingsLayout->addWidget(m_generationParameters);
    m_componentContainer = new QWidget(m_settingsPage);
    m_componentContainer->setObjectName(QStringLiteral("translateComponentContainer"));
    m_componentLayout = new QVBoxLayout(m_componentContainer);
    m_componentLayout->setContentsMargins(0, 0, 0, 0);
    m_componentContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    offlineSettingsLayout->addWidget(m_componentContainer);
    offlineSettingsLayout->addStretch(1);
    for (auto *button : {m_browseEngineButton, m_addAudioButton, m_clearButton,
                        m_startButton, m_retryButton, m_cancelButton,
                        m_componentSettingsButton, backButton, m_restoreGenerationButton,
                        m_translateSubtitleButton, m_openResultFolderButton})
        button->setAutoDefault(false);
    auto *layout = new CurrentPageContentLayout(m_pages);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    layout->addWidget(m_pages);
    m_dialogFrame = new DialogFrame(this, layout);

    connect(m_componentSettingsButton, &QPushButton::clicked, this, &ChickenRiceTranslateDialog::showComponentSettings);
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, &ChickenRiceTranslateDialog::changeGenerationMode);
    connect(backButton, &QPushButton::clicked, this, &ChickenRiceTranslateDialog::showGenerationPage);
    connect(m_pages, &QStackedWidget::currentChanged, this, [this] { refreshContentMinimum(); });
    connect(m_browseEngineButton, &QPushButton::clicked, this, &ChickenRiceTranslateDialog::browseFolder);
    connect(m_clearButton, &QPushButton::clicked, m_clearAction, &QAction::trigger);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &ChickenRiceTranslateDialog::showQueueContextMenu);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &ChickenRiceTranslateDialog::updateContextActions);
    connect(m_removeAction, &QAction::triggered, this, [this] {
        const int row = m_table->currentRow();
        if (!taskEditingAllowed() || row < 0 || row >= m_items.size()) return;
        m_items.removeAt(row);
        m_table->removeRow(row);
        scheduleQueueHistorySave();
        updatePresentation();
    });
    connect(m_openFolderAction, &QAction::triggered, this, [this] {
        const int row = m_table->currentRow();
        if (m_shuttingDown || row < 0 || row >= m_items.size()) return;
        const auto &item = m_items.at(row);
        if (item.state == State::Succeeded && !normalizedPath(item.result.srtPath).isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(item.result.srtPath).absolutePath()));
    });
    connect(m_errorAction, &QAction::triggered, this, &ChickenRiceTranslateDialog::showSelectedError);
    connect(m_translateSubtitleAction, &QAction::triggered, this, &ChickenRiceTranslateDialog::transferSelectedJapaneseSubtitle);
    connect(m_translateSubtitleButton, &QPushButton::clicked, this, &ChickenRiceTranslateDialog::showSubtitleTranslation);
    connect(m_openResultFolderButton, &QPushButton::clicked, m_openFolderAction, &QAction::trigger);
    connect(m_addAudioButton, &QPushButton::clicked, this, &ChickenRiceTranslateDialog::browseAudioFiles);
    connect(m_clearAction, &QAction::triggered, this, [this] {
        if (!taskEditingAllowed())
            return;
        m_items.clear();
        m_table->setRowCount(0);
        m_activeRow = -1;
        m_feedback.clear();
        m_logTail.clear();
        scheduleQueueHistorySave();
        updatePresentation();
    });
    connect(m_startButton, &QPushButton::clicked, this, [this] { startQueue(false); });
    connect(m_retryButton, &QPushButton::clicked, this, [this] { startQueue(true); });
    connect(m_cancelButton, &QPushButton::clicked, this, &ChickenRiceTranslateDialog::cancelQueue);
    m_validationTimer = new QTimer(this);
    m_validationTimer->setSingleShot(true);
    m_validationTimer->setInterval(120);
    connect(m_validationTimer, &QTimer::timeout, this, &ChickenRiceTranslateDialog::startEngineCheck);
    connect(m_engineDirectory, &QLineEdit::textChanged, this, [this] {
        m_preferencesDirty |= !m_loadingSettings;
        if (!m_loadingSettings) scheduleEngineCheck();
    });
    connect(m_computeMode, &QComboBox::currentIndexChanged, this, [this] {
        m_preferencesDirty |= !m_loadingSettings;
    });
    const auto generationEdited = [this] {
        if (!m_loadingSettings) {
            persistGenerationOptions();
            updatePresentation();
        }
    };
    connect(m_generationPreset, &QComboBox::currentIndexChanged, this, generationEdited);
    connect(m_vadThreshold, &QDoubleSpinBox::valueChanged, this, generationEdited);
    connect(m_minSpeechDuration, &QSpinBox::valueChanged, this, generationEdited);
    connect(m_minSilenceDuration, &QSpinBox::valueChanged, this, generationEdited);
    connect(m_speechPad, &QSpinBox::valueChanged, this, generationEdited);
    connect(m_mergeSegments, &QCheckBox::toggled, this, generationEdited);
    connect(m_restoreGenerationButton, &QPushButton::clicked, this, [this] {
        if (!taskEditingAllowed()) return;
        applyGenerationOptions({});
        persistGenerationOptions();
        updatePresentation();
    });
    connect(m_service, &ChickenRiceTranslateService::progressChanged, this, [this](const auto &progress) {
        if (m_batchMode == QStringLiteral("offline-direct")) applyProgress(progress);
    });
    connect(m_service, &ChickenRiceTranslateService::finished, this, [this](const auto &result) {
        if (m_batchMode == QStringLiteral("offline-direct")) applyResult(result);
    });
    connect(m_service, &ChickenRiceTranslateService::shutdownFinished,
            this, &ChickenRiceTranslateDialog::finishShutdownIfReady);
    connect(m_service, &QObject::destroyed, this, [this] {
        m_service = nullptr;
        if (!m_queueRunning || m_batchMode != QStringLiteral("offline-direct")) {
            updatePresentation(); finishShutdownIfReady(); return;
        }
        if (m_activeRow >= 0) {
            m_items[m_activeRow].state = State::Failed;
            m_items[m_activeRow].result.error = QStringLiteral("生成服务已停止。");
            if (!m_items[m_activeRow].attempts.isEmpty()) {
                auto &attempt=m_items[m_activeRow].attempts.last();
                attempt.state=QStringLiteral("failed");
                attempt.finishedAt=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                attempt.result={{QStringLiteral("error"),m_items[m_activeRow].result.error}, {QStringLiteral("exitCode"),-1}};
            }
            scheduleQueueHistorySave();
            updateRow(m_activeRow);
        }
        m_launchAfterQueueSave=false;
        m_activeRow = -1;
        m_queueRunning = false;
        // cancelQueue may synchronously emit shutdownFinished and delete this.
        cancelQueue();
    });
    if (componentPanel)
        attachComponentPanel(componentPanel);
    if (fasterWhisperPanel) attachFasterWhisperPanel(fasterWhisperPanel);
    if (m_transcribeService) ensureTranscribeService();
    restoreSettings();
    restoreQueueHistory();
    m_loadingSettings = false;
    scheduleEngineCheck();
    updatePresentation();
}

ChickenRiceTranslateDialog::~ChickenRiceTranslateDialog()
{
    // The detached mailbox finishes a queued snapshot without joining an
    // inference worker, including when a caller destroys a hidden dialog.
    persistQueueHistory();
    if (m_discoveryCancelled)
        m_discoveryCancelled->store(true);
    if (m_validationCancelled)
        m_validationCancelled->store(true);
    if (m_translationSettingsPanel) {
        disconnect(m_translationSettingsPanel, nullptr, this, nullptr);
        m_translationSettingsPanel->shutdown();
    }
    if (m_componentPanel) {
        disconnect(m_componentPanel, nullptr, this, nullptr);
        m_componentPanel->shutdown();
    }
    if (m_fasterWhisperPanel) {
        disconnect(m_fasterWhisperPanel, nullptr, this, nullptr);
        m_fasterWhisperPanel->shutdown();
    }
    if (m_transcribeService) {
        disconnect(m_transcribeService, nullptr, this, nullptr);
        if (m_ownsTranscribeService) m_transcribeService->shutdown();
        else if (m_queueRunning && m_batchMode == QStringLiteral("offline-asr-ja")) m_transcribeService->cancel();
    }
    if (m_service) {
        disconnect(m_service, nullptr, this, nullptr);
        if (m_ownsService)
            m_service->shutdown();
        else if (m_queueRunning)
            m_service->cancel();
    }
}

bool ChickenRiceTranslateDialog::translationBusy() const
{
    return m_queueRunning || (m_service && m_service->isBusy())
        || (m_transcribeService && m_transcribeService->isBusy())
        || (m_subtitleTranslation && m_subtitleTranslation->isBusy());
}

bool ChickenRiceTranslateDialog::taskEditingAllowed() const
{
    return !m_shuttingDown && !m_queueHistoryLoading && m_queueHistoryWritable && !m_discovering && !translationBusy()
        && !(m_translationSettingsPanel && m_translationSettingsPanel->isBusy())
        && !(m_componentPanel && m_componentPanel->installationBusy())
        && !(m_fasterWhisperPanel && m_fasterWhisperPanel->installationBusy());
}

void ChickenRiceTranslateDialog::attachComponentPanel(GpuRecommendationDialog *panel)
{
    Q_ASSERT(panel && !m_componentPanel);
    m_componentPanel = panel;
    panel->setParent(m_componentContainer, Qt::Widget);
    m_componentLayout->addWidget(panel);
    connect(panel, &GpuRecommendationDialog::engineReady, this, [this](const QString &path) {
        const QString engine = normalizedPath(path);
        if (m_shuttingDown || engine.isEmpty())
            return;
        m_engineDirectory->setText(engine);
        scheduleEngineCheck();
        if (m_settings)
            m_settings->setValue(Prefix + QStringLiteral("engineDirectory"), engine);
        setModeFeedback(0, QStringLiteral("组件已准备好，返回生成页后点击“开始生成”。"));
        updatePresentation();
    });
    connect(panel, &GpuRecommendationDialog::activityChanged, this, [this] {
        updatePresentation();
        refreshContentMinimum();
        finishShutdownIfReady();
    });
    connect(panel, &GpuRecommendationDialog::shutdownFinished,
            this, &ChickenRiceTranslateDialog::finishShutdownIfReady);
    connect(panel, &QObject::destroyed, this, [this] {
        m_componentPanel = nullptr;
        updatePresentation();
        finishShutdownIfReady();
    });
    // The embedded installation section changes height without starting an
    // operation. Keep the shared scroll host in step with that expansion too.
    if (auto *advanced = panel->findChild<QPushButton *>(QStringLiteral("gpuAdvancedButton")))
        connect(advanced, &QPushButton::toggled, this, [this] {
            refreshContentMinimum();
            QTimer::singleShot(0, this, &ChickenRiceTranslateDialog::refreshContentMinimum);
        });
    panel->show();
}

void ChickenRiceTranslateDialog::ensureComponentPanel()
{
    if (!m_componentPanel)
        attachComponentPanel(new GpuRecommendationDialog(m_settings, m_componentContainer,
            m_gpuProbe, nullptr, true));
}

void ChickenRiceTranslateDialog::ensureFasterWhisperPanel()
{
    if (!m_fasterWhisperPanel)
        attachFasterWhisperPanel(new FasterWhisperInstallWidget(m_settings, m_fasterWhisperPage));
    if (!m_fasterWhisperCheckStarted) {
        m_fasterWhisperCheckStarted = true;
        // Validation belongs to the selected mode, not the visibility of its settings.
        m_fasterWhisperPanel->recheck();
    }
}

void ChickenRiceTranslateDialog::attachFasterWhisperPanel(FasterWhisperInstallWidget *panel)
{
    Q_ASSERT(panel && !m_fasterWhisperPanel);
    m_fasterWhisperPanel = panel;
    panel->setParent(m_fasterWhisperPage, Qt::Widget);
    // Keep both the field forms and the bounded settings widget at the top.
    // A single Maximum-height widget can otherwise be centred in the stack's
    // larger cell by the Windows layout style after a page switch.
    for (auto *form : panel->findChildren<QFormLayout *>())
        form->setFormAlignment(Qt::AlignTop);
    m_fasterWhisperLayout->addWidget(panel, 0, Qt::AlignTop);
    m_fasterWhisperLayout->addStretch(1);
    connect(panel, &FasterWhisperInstallWidget::activityChanged, this, [this] {
        updatePresentation();
        refreshContentMinimum();
        finishShutdownIfReady();
    });
    connect(panel, &FasterWhisperInstallWidget::shutdownFinished,
            this, &ChickenRiceTranslateDialog::finishShutdownIfReady);
    connect(panel, &QObject::destroyed, this, [this] {
        m_fasterWhisperPanel = nullptr;
        m_fasterWhisperCheckStarted = false;
        updatePresentation();
        finishShutdownIfReady();
    });
}

void ChickenRiceTranslateDialog::ensureTranscribeService()
{
    if (m_transcribeConnected) return;
    if (!m_transcribeService) {
        m_transcribeService = new FasterWhisperTranscribeService(this);
        m_ownsTranscribeService = true;
    }
    m_transcribeConnected = true;
    connect(m_transcribeService, &FasterWhisperTranscribeService::progressChanged, this, [this](const auto &progress) {
        if (m_batchMode == QStringLiteral("offline-asr-ja")) applyProgress(progress);
    });
    connect(m_transcribeService, &FasterWhisperTranscribeService::finished, this, [this](const auto &result) {
        if (m_batchMode == QStringLiteral("offline-asr-ja")) applyResult(result);
    });
    connect(m_transcribeService, &FasterWhisperTranscribeService::shutdownFinished,
            this, &ChickenRiceTranslateDialog::finishShutdownIfReady);
    connect(m_transcribeService, &QObject::destroyed, this, [this] {
        m_transcribeService = nullptr;
        m_transcribeConnected = false;
        if (!m_queueRunning || m_batchMode != QStringLiteral("offline-asr-ja")) {
            updatePresentation(); finishShutdownIfReady(); return;
        }
        if (m_activeRow >= 0) {
            m_items[m_activeRow].state = State::Failed;
            m_items[m_activeRow].result.error = QStringLiteral("识别服务已停止。");
            if (!m_items[m_activeRow].attempts.isEmpty()) {
                auto &attempt=m_items[m_activeRow].attempts.last();
                attempt.state=QStringLiteral("failed");
                attempt.finishedAt=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                attempt.result={{QStringLiteral("error"),m_items[m_activeRow].result.error}, {QStringLiteral("exitCode"),-1}};
            }
            scheduleQueueHistorySave();
            updateRow(m_activeRow);
        }
        m_launchAfterQueueSave=false;
        m_activeRow = -1;
        m_queueRunning = false;
        cancelQueue();
    });
}

bool ChickenRiceTranslateDialog::japaneseMode() const
{
    return m_modeCombo->currentData().toString() == QStringLiteral("offline-asr-ja");
}

bool ChickenRiceTranslateDialog::subtitleTranslationMode() const
{
    return m_modeCombo->currentData().toString() == QStringLiteral("cloud-translation");
}

void ChickenRiceTranslateDialog::ensureTranslationSettingsPanel()
{
    if (!m_translationSettingsPanel)
        attachTranslationSettingsPanel(new TranslationApiSettingsWidget(m_settings, m_componentPages));
}

void ChickenRiceTranslateDialog::attachTranslationSettingsPanel(TranslationApiSettingsWidget *panel)
{
    Q_ASSERT(panel && !m_translationSettingsPanel);
    m_translationSettingsPanel = panel;
    panel->setParent(m_componentPages, Qt::Widget);
    panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_componentPages->addWidget(panel);
    connect(panel, &TranslationApiSettingsWidget::settingsChanged, this, [this] {
        refreshTranslationSettings();
        updatePresentation();
    });
    connect(panel, &TranslationApiSettingsWidget::busyChanged, this, [this] {
        updatePresentation();
        finishShutdownIfReady();
    });
    connect(panel, &TranslationApiSettingsWidget::shutdownFinished,
            this, &ChickenRiceTranslateDialog::finishShutdownIfReady);
}

void ChickenRiceTranslateDialog::refreshTranslationSettings()
{
    if (!m_shuttingDown && m_subtitleTranslation) {
        m_subtitleTranslation->refreshSettings();
    }
}

bool ChickenRiceTranslateDialog::addSubtitleForTranslation(const QString &sourcePath, const QString &audioPath,
    const QString &libraryRoot, const QString &libraryRelativePath,
    qint64 workId, qint64 audioFileId, qint64 subtitleFileId)
{
    if (m_shuttingDown || !modeSwitchAllowed() || !m_subtitleTranslation) return false;
    m_modeCombo->setCurrentIndex(m_modeCombo->findData(QStringLiteral("cloud-translation")));
    if (!subtitleTranslationMode()) return false;
    if (!m_subtitleTranslation->addSubtitleFile(sourcePath, audioPath, libraryRoot, libraryRelativePath,
            workId, audioFileId, subtitleFileId)) return false;
    showGenerationPage();
    return true;
}

void ChickenRiceTranslateDialog::showSubtitleTranslation()
{
    if (!modeSwitchAllowed() || !m_subtitleTranslation) return;
    m_modeCombo->setCurrentIndex(m_modeCombo->findData(QStringLiteral("cloud-translation")));
    if (subtitleTranslationMode()) showGenerationPage();
}

void ChickenRiceTranslateDialog::transferSelectedJapaneseSubtitle()
{
    const int row = m_table->currentRow();
    if (!japaneseMode() || !modeSwitchAllowed() || row < 0 || row >= m_items.size()) return;
    const auto item = m_items.at(row);
    if (item.state != State::Succeeded || item.result.srtPath.isEmpty()) return;
    m_modeCombo->setCurrentIndex(m_modeCombo->findData(QStringLiteral("cloud-translation")));
    if (!subtitleTranslationMode()) return;
    m_subtitleTranslation->addSubtitleFile(item.result.srtPath, item.audioPath,
        item.libraryRoot, item.libraryRelativePath);
    showGenerationPage();
}

void ChickenRiceTranslateDialog::changeGenerationMode()
{
    const int mode = m_modeCombo->currentIndex();
    if (mode == m_sessionMode) return;
    if (mode < 0 || mode >= static_cast<int>(m_modeSessions.size()) || !modeSwitchAllowed()) {
        const QSignalBlocker blocker(m_modeCombo);
        m_modeCombo->setCurrentIndex(m_sessionMode);
        return;
    }
    auto &previous = m_modeSessions.at(m_sessionMode);
    previous.items = std::move(m_items);
    previous.feedback = std::move(m_feedback);
    previous.logTail = std::move(m_logTail);
    previous.selectedRow = m_table->currentRow();
    previous.scrollValue = m_table->verticalScrollBar()->value();
    m_sessionMode = mode;
    auto &current = m_modeSessions.at(mode);
    m_items = std::move(current.items);
    m_feedback = std::move(current.feedback);
    m_logTail = std::move(current.logTail);
    m_currentPercent = -1;
    {
        const QSignalBlocker blocker(m_table);
        m_table->setRowCount(0);
        m_table->setRowCount(static_cast<int>(m_items.size()));
        for (int row = 0; row < m_items.size(); ++row) {
            for (int column = 0; column < 2; ++column)
                m_table->setItem(row, column, new QTableWidgetItem);
            updateRow(row);
        }
        if (!m_items.isEmpty())
            m_table->selectRow(std::clamp(current.selectedRow, 0, static_cast<int>(m_items.size()) - 1));
        m_table->verticalScrollBar()->setValue(current.scrollValue);
    }
    if (japaneseMode()) {
        ensureFasterWhisperPanel();
        ensureTranscribeService();
    }
    if (subtitleTranslationMode()) {
        m_subtitleTranslation->refreshSettings();
        m_pages->setCurrentWidget(m_generationPage);
    }
    if (m_pages->currentWidget() == m_settingsPage) showComponentSettings();
    else updatePresentation();
}

bool ChickenRiceTranslateDialog::modeSwitchAllowed() const
{
    return !m_shuttingDown && !translationBusy()
        && !(m_translationSettingsPanel && m_translationSettingsPanel->isBusy())
        && !m_folderPicker && !m_audioPicker && !m_errorDialog
        && !(m_subtitleTranslation && m_subtitleTranslation->hasOpenDialog())
        && !(m_componentPanel && (m_componentPanel->installationBusy() || m_componentPanel->hasOpenPicker()))
        && !(m_fasterWhisperPanel && (m_fasterWhisperPanel->installationBusy() || m_fasterWhisperPanel->hasOpenPicker()));
}

void ChickenRiceTranslateDialog::setModeFeedback(int mode, const QString &text)
{
    if (mode == m_sessionMode) {
        if (!translationBusy()) m_feedback = text;
    } else m_modeSessions.at(mode).feedback = text;
}

void ChickenRiceTranslateDialog::showComponentSettings()
{
    if (m_shuttingDown)
        return;
    if (subtitleTranslationMode()) {
        if (translationBusy()) return;
        ensureTranslationSettingsPanel();
        m_componentPages->setCurrentWidget(m_translationSettingsPanel);
    } else if (japaneseMode()) {
        ensureFasterWhisperPanel();
        m_componentPages->setCurrentIndex(1);
    } else {
        ensureComponentPanel();
        m_componentPages->setCurrentIndex(0);
    }
    m_pages->setCurrentWidget(m_settingsPage);
    updatePresentation();
    refreshContentMinimum();
    if (!subtitleTranslationMode() && !japaneseMode() && !m_componentDetectionStarted) {
        m_componentDetectionStarted = true;
        QPointer<ChickenRiceTranslateDialog> guard(this);
        m_componentPanel->startDetection();
        if (!guard)
            return;
    }
}

void ChickenRiceTranslateDialog::showGenerationPage()
{
    if (m_shuttingDown) return;
    QPointer<ChickenRiceTranslateDialog> guard(this);
    if (m_translationSettingsPanel) m_translationSettingsPanel->deactivate();
    if (!guard) return;
    persistDraftSettings();
    m_pages->setCurrentWidget(m_generationPage);
    updatePresentation();
    // Recompute after status visibility changes, and once queued layout events
    // have invalidated the previously expanded installation page.
    refreshContentMinimum();
    QTimer::singleShot(0, this, &ChickenRiceTranslateDialog::refreshContentMinimum);
}

void ChickenRiceTranslateDialog::refreshContentMinimum()
{
    if (!m_dialogFrame)
        return;
    updateQueueButtonLayout();
    auto *content = m_dialogFrame->contentWidget();
    static_cast<ComponentStatusRow *>(m_componentStatus->parentWidget())->refreshHeight();
    static_cast<ComponentStatusRow *>(m_settingsComponentStatus->parentWidget())->refreshHeight();
    if (m_pages->currentWidget() && m_pages->currentWidget()->layout()) {
        m_pages->currentWidget()->layout()->invalidate();
        m_pages->currentWidget()->layout()->activate();
        m_pages->currentWidget()->updateGeometry();
    }
    m_pages->layout()->invalidate();
    content->layout()->invalidate();
    m_pages->updateGeometry();
    content->setMinimumSize(content->layout()->totalMinimumSize());
    // Expanded settings remain scrollable without changing the window minimum.
    setMinimumSize(320, 240);
}

void ChickenRiceTranslateDialog::updateQueueButtonLayout()
{
    if (!m_queueButtons) return;
    int rowWidth = 0, count = 0;
    for (auto *button : {m_addAudioButton, m_clearButton, m_translateSubtitleButton,
                        m_openResultFolderButton, m_retryButton, m_startButton, m_cancelButton}) {
        if (!button->isHidden()) { rowWidth += button->sizeHint().width(); ++count; }
    }
    rowWidth += std::max(0, count - 1) * m_queueButtons->spacing();
    // Keep the ordinary single row, but let a compact scroll host reach every
    // action without requiring horizontal scrolling. Include frame/body margins
    // and space for the vertical scrollbar in the available width.
    m_queueButtons->setDirection(width() - 54 >= rowWidth
        ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
}

void ChickenRiceTranslateDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    refreshContentMinimum();
}

void ChickenRiceTranslateDialog::restoreSettings()
{
    m_engineDirectory->setText(normalizedPath(stringSetting(m_settings, QStringLiteral("engineDirectory"))));
    const QString compute = stringSetting(m_settings, QStringLiteral("computeMode"));
    const auto mode = aiSubtitleComputeModeFromKey(compute).value_or(AiSubtitleComputeMode::GpuLowMemory);
    m_computeMode->setCurrentIndex(qMax(0, m_computeMode->findData(aiSubtitleComputeModeKey(mode))));
    restoreGenerationOptions();
    if (m_engineDirectory->text().isEmpty()) {
        const QString root = normalizedPath(stringSetting(m_settings, QStringLiteral("installRoot")));
        discoverInstalledEngine(root.isEmpty() ? defaultRoot() : root);
    }
}

void ChickenRiceTranslateDialog::restoreQueueHistory()
{
    m_queueHistoryLoading = true;
    ++m_queueHistoryWorkers;
    auto promise = std::make_shared<QPromise<QueueHistoryLoadResult>>();
    auto *watcher = new QFutureWatcher<QueueHistoryLoadResult>(this);
    connect(watcher,&QFutureWatcher<QueueHistoryLoadResult>::finished,this,[this,watcher] {
        auto loaded = watcher->future().takeResult();
        watcher->deleteLater();
        --m_queueHistoryWorkers;
        m_queueHistoryLoading = false;
        m_taskQueueLease=std::move(loaded.lease);
        bool valid = loaded.error.isEmpty();
        std::array<QVector<QueueItem>,2> restored;
        for (const auto &record : loaded.snapshot.tasks) {
            const int mode = record.mode == QStringLiteral("offline-asr-ja") ? 1 : 0;
            const auto &object = record.data;
            QueueItem item;
            item.taskId = record.id;
            item.createdAt = record.createdAt;
            item.audioPath = normalizedPath(object.value(QStringLiteral("audioPath")).toString());
            item.libraryRoot = object.value(QStringLiteral("libraryRoot")).toString();
            item.libraryRelativePath = object.value(QStringLiteral("libraryRelativePath")).toString();
            item.outputBatchId = object.value(QStringLiteral("outputBatchId")).toString();
            item.outputFileSuffix = object.value(QStringLiteral("outputFileSuffix")).toString();
            for (const auto &field : {QStringLiteral("outputBatchId"), QStringLiteral("outputFileSuffix")})
                if (object.contains(field) && !object.value(field).isString()) valid = false;
            if (!validOutputComponent(item.outputBatchId) || !validOutputComponent(item.outputFileSuffix)
                || (item.outputBatchId.isEmpty() && !item.outputFileSuffix.isEmpty())) valid = false;
            if (object.value(QStringLiteral("version")).toInt() != 1 || item.audioPath.isEmpty()
                || std::any_of(restored[mode].cbegin(),restored[mode].cend(),[&item](const auto &other) { return samePath(item.audioPath,other.audioPath); })) { valid=false; break; }
            if (!item.libraryRoot.isEmpty() || !item.libraryRelativePath.isEmpty()) {
                const auto relative = QDir::cleanPath(QDir::fromNativeSeparators(item.libraryRelativePath));
                if (normalizedPath(item.libraryRoot).isEmpty() || QDir::isAbsolutePath(relative)
                    || relative == QStringLiteral(".") || relative == QStringLiteral("..")
                    || relative.startsWith(QStringLiteral("../")) || relative.contains(QChar::Null)
                    || !samePath(item.audioPath,normalizedPath(QDir(item.libraryRoot).filePath(relative)))) { valid=false; break; }
            }
            item.state = record.state == QStringLiteral("pending") ? State::Idle
                : record.state == QStringLiteral("succeeded") ? State::Succeeded
                : record.state == QStringLiteral("failed") ? State::Failed : State::Cancelled;
            item.interrupted = record.state == QStringLiteral("interrupted");
            auto &result = item.result;
            result.audioPath=item.audioPath; result.state=item.state; result.mode=record.mode;
            result.language=object.value(QStringLiteral("language")).toString();
            result.srtPath=object.value(QStringLiteral("srtPath")).toString();
            result.taskDirectory=object.value(QStringLiteral("taskDirectory")).toString();
            result.logPath=object.value(QStringLiteral("logPath")).toString();
            result.error=object.value(QStringLiteral("error")).toString();
            result.exitCode=object.value(QStringLiteral("exitCode")).toInt(-1);
            result.subtitleCount=object.value(QStringLiteral("subtitleCount")).toInt();
            for (const auto &value : object.value(QStringLiteral("warnings")).toArray()) {
                if (!value.isString()) { valid=false; break; }
                result.warnings.append(value.toString());
            }
            for (const auto &path : {result.srtPath,result.taskDirectory,result.logPath})
                if (!path.isEmpty() && normalizedPath(path).isEmpty()) valid=false;
            if (item.state == State::Succeeded && result.srtPath.isEmpty()) valid=false;
            if (item.interrupted) result.error=QStringLiteral("上次运行已中断，请手动重试。");
            for (const auto &attempt : loaded.snapshot.attempts)
                if (attempt.taskId==item.taskId) item.attempts.append(attempt);
            restored[mode].append(std::move(item));
        }
        if (valid) {
            // Programmatic add actions may arrive before the worker finishes.
            // Merge them after restored rows and retain their explicit paths.
            for (int mode=0;mode<2;++mode) {
                auto &current = mode==m_sessionMode ? m_items : m_modeSessions[mode].items;
                for (auto &pending : current) {
                    auto found=std::find_if(restored[mode].begin(),restored[mode].end(),
                        [&pending](const auto &item) { return samePath(item.audioPath,pending.audioPath); });
                    if (found==restored[mode].end()) restored[mode].append(std::move(pending));
                    else if (!pending.libraryRoot.isEmpty()) {
                        found->libraryRoot=pending.libraryRoot; found->libraryRelativePath=pending.libraryRelativePath;
                    }
                }
                current=std::move(restored[mode]);
            }
            m_table->setRowCount(static_cast<int>(m_items.size()));
            for (int row=0;row<m_items.size();++row) {
                for (int column=0;column<2;++column) m_table->setItem(row,column,new QTableWidgetItem);
                updateRow(row);
            }
            if (!m_items.isEmpty()) m_table->selectRow(0);
        } else {
            m_queueHistoryWritable=false;
            m_queueHistoryStatus->setText(QStringLiteral("任务数据库读取失败，原记录已保留；本次列表暂不保存或执行。")
                +QLatin1Char('\n')+loaded.error);
            m_queueHistoryStatus->setToolTip(loaded.error);
        }
        if (m_queueHistoryDirty) persistQueueHistory();
        updatePresentation();
        finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    const auto path=m_taskDatabasePath;
    QThreadPool::globalInstance()->start([promise,path] {
        promise->start();
        QueueHistoryLoadResult loaded;
        loaded.lease=acquireAiTaskQueueLease(path,QStringLiteral("offline"),&loaded.error);
        if (loaded.lease) {
            AiTaskRepository repository(path);
            repository.load({QStringLiteral("offline-direct"),QStringLiteral("offline-asr-ja")},&loaded.snapshot,&loaded.error);
        }
        promise->addResult(std::move(loaded)); promise->finish();
    });
}

void ChickenRiceTranslateDialog::scheduleQueueHistorySave()
{
    if (m_loadingSettings || !m_queueHistoryWritable) return;
    m_queueHistoryDirty=true;
    if (!m_queueHistoryLoading) m_queueHistoryTimer->start();
}

void ChickenRiceTranslateDialog::persistQueueHistory()
{
    m_queueHistoryTimer->stop();
    if (!m_queueHistoryDirty || !m_queueHistoryWritable || m_queueHistoryLoading || !m_taskQueueLease) return;
    AiTaskSnapshot snapshot;
    for (int mode=0;mode<2;++mode) {
        auto &items = mode==m_sessionMode ? m_items : m_modeSessions[mode].items;
        for (int row=0;row<items.size();++row) {
            auto &item=items[row];
            if (item.taskId.isEmpty()) item.taskId=QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (item.createdAt.isEmpty()) item.createdAt=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            const auto &result=item.result;
            const auto state=item.state==State::Idle ? QStringLiteral("pending")
                : item.state==State::Succeeded ? QStringLiteral("succeeded")
                : item.state==State::Failed ? QStringLiteral("failed")
                : item.state==State::Cancelled ? item.interrupted ? QStringLiteral("interrupted") : QStringLiteral("cancelled")
                : QStringLiteral("running");
            QJsonObject recordData{{QStringLiteral("version"),1},{QStringLiteral("audioPath"),item.audioPath},
                {QStringLiteral("libraryRoot"),item.libraryRoot},{QStringLiteral("libraryRelativePath"),item.libraryRelativePath},
                {QStringLiteral("outputBatchId"),item.outputBatchId},{QStringLiteral("outputFileSuffix"),item.outputFileSuffix},
                {QStringLiteral("srtPath"),result.srtPath},{QStringLiteral("taskDirectory"),result.taskDirectory},
                {QStringLiteral("logPath"),result.logPath},{QStringLiteral("error"),result.error},
                {QStringLiteral("exitCode"),result.exitCode},{QStringLiteral("subtitleCount"),qint64(result.subtitleCount)},
                {QStringLiteral("warnings"),QJsonArray::fromStringList(result.warnings)},{QStringLiteral("language"),result.language}};
            snapshot.tasks.append({item.taskId,m_modeCombo->itemData(mode).toString(),row,state,recordData,item.createdAt,{}});
            snapshot.attempts+=item.attempts;
        }
    }
    if (snapshot.tasks.size()>MaximumQueueHistoryItems) {
        m_queueHistoryStatus->setText(QStringLiteral("任务记录过多，暂未保存；请移除不再需要的列表项。字幕文件会保留。"));
        m_queueHistoryStatus->show();
        return;
    }
    const auto writer=m_queueHistoryWriter;
    {
        QMutexLocker lock(&writer->mutex);
        writer->pending=std::move(snapshot);
        m_queueHistoryDirty=false;
        if (writer->running) return;
        writer->running=true;
    }
    const auto path=m_taskDatabasePath;
    ++m_queueHistoryWorkers;
    auto promise=std::make_shared<QPromise<QString>>();
    auto *watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,writer] {
        QString error;
        { QMutexLocker lock(&writer->mutex); error=writer->lastError; }
        watcher->deleteLater(); --m_queueHistoryWorkers;
        if (!m_queueHistoryDirty) {
            m_queueHistoryStatus->setText(error.isEmpty()?QString():QStringLiteral("任务列表保存失败，本次列表可能无法在下次启动时恢复。"));
            m_queueHistoryStatus->setToolTip(error);
        }
        QPointer<ChickenRiceTranslateDialog> guard(this);
        if (m_launchAfterQueueSave && !m_queueHistoryDirty && m_queueHistoryWorkers==0) {
            m_launchAfterQueueSave=false;
            if (m_activeRow<0) { updatePresentation(); finishShutdownIfReady(); return; }
            if (error.isEmpty()) launchActiveTask();
            else {
                AiSubtitleTaskResult result;
                result.audioPath=m_items.at(m_activeRow).audioPath; result.state=State::Failed;
                result.error=QStringLiteral("任务记录保存失败，未启动生成：%1").arg(error);
                applyResult(result);
            }
        }
        if (!guard) return;
        updatePresentation(); finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    auto lease=m_taskQueueLease;
    QThreadPool::globalInstance()->start([writer,path,promise,lease]() mutable {
        promise->start();
        QString error;
        while (true) {
            AiTaskSnapshot snapshot;
            {
                QMutexLocker lock(&writer->mutex);
                if (!writer->pending) { writer->running=false; break; }
                snapshot=std::move(*writer->pending); writer->pending.reset();
            }
            AiTaskRepository repository(path);
            repository.save({QStringLiteral("offline-direct"),QStringLiteral("offline-asr-ja")},snapshot,&error);
            { QMutexLocker lock(&writer->mutex); writer->lastError=error; }
        }
        lease.reset();
        promise->addResult(error); promise->finish();
    });
}

void ChickenRiceTranslateDialog::restoreGenerationOptions()
{
    ChickenRiceGenerationOptions options;
    if (m_settings) {
        const QVariant saved = m_settings->value(GenerationSettingsKey);
        if (saved.metaType().id() == QMetaType::QString && saved.toString().size() <= 4096) {
            const auto document = QJsonDocument::fromJson(saved.toString().toUtf8());
            const auto object = document.object();
            const auto presetValue = object.value(QStringLiteral("preset"));
            const auto preset = presetValue.isString()
                ? chickenRiceGenerationPresetFromKey(presetValue.toString()) : std::nullopt;
            const auto threshold = object.value(QStringLiteral("vadThreshold"));
            const auto speech = object.value(QStringLiteral("minSpeechDurationMs"));
            const auto silence = object.value(QStringLiteral("minSilenceDurationMs"));
            const auto padding = object.value(QStringLiteral("speechPadMs"));
            const auto merge = object.value(QStringLiteral("mergeSegments"));
            const auto integer = [](const QJsonValue &value, int minimum, int maximum) {
                const double number = value.toDouble(-1);
                return value.isDouble() && std::isfinite(number) && std::trunc(number) == number
                    && number >= minimum && number <= maximum;
            };
            if (document.isObject() && object.value(QStringLiteral("schema")).isDouble()
                && object.value(QStringLiteral("schema")).toDouble() == 1 && preset
                && threshold.isDouble() && std::isfinite(threshold.toDouble())
                && threshold.toDouble() >= ChickenRiceVadThresholdMinimum
                && threshold.toDouble() <= ChickenRiceVadThresholdMaximum
                && integer(speech, ChickenRiceMinSpeechDurationMinimumMs, ChickenRiceMinSpeechDurationMaximumMs)
                && integer(silence, ChickenRiceMinSilenceDurationMinimumMs, ChickenRiceMinSilenceDurationMaximumMs)
                && integer(padding, ChickenRiceSpeechPadMinimumMs, ChickenRiceSpeechPadMaximumMs) && merge.isBool()) {
                options.preset = *preset;
                options.vadThreshold = threshold.toDouble();
                options.minSpeechDurationMs = speech.toInt();
                options.minSilenceDurationMs = silence.toInt();
                options.speechPadMs = padding.toInt();
                options.mergeSegments = merge.toBool();
            }
        }
    }
    // A malformed preference is a UI fallback, never a write during restoration.
    applyGenerationOptions(options);
}

void ChickenRiceTranslateDialog::applyGenerationOptions(const ChickenRiceGenerationOptions &options)
{
    const bool wasLoading = std::exchange(m_loadingSettings, true);
    m_generationPreset->setCurrentIndex(m_generationPreset->findData(chickenRiceGenerationPresetKey(options.preset)));
    m_vadThreshold->setValue(options.vadThreshold);
    m_minSpeechDuration->setValue(options.minSpeechDurationMs);
    m_minSilenceDuration->setValue(options.minSilenceDurationMs);
    m_speechPad->setValue(options.speechPadMs);
    m_mergeSegments->setChecked(options.mergeSegments);
    m_loadingSettings = wasLoading;
}

ChickenRiceGenerationOptions ChickenRiceTranslateDialog::generationOptions() const
{
    ChickenRiceGenerationOptions options;
    options.preset = chickenRiceGenerationPresetFromKey(m_generationPreset->currentData().toString())
        .value_or(static_cast<ChickenRiceGenerationPreset>(-1));
    options.vadThreshold = m_vadThreshold->value();
    options.minSpeechDurationMs = m_minSpeechDuration->value();
    options.minSilenceDurationMs = m_minSilenceDuration->value();
    options.speechPadMs = m_speechPad->value();
    options.mergeSegments = m_mergeSegments->isChecked();
    return options;
}

void ChickenRiceTranslateDialog::persistGenerationOptions()
{
    if (m_loadingSettings || !m_settings || !taskEditingAllowed()) return;
    const auto options = generationOptions();
    auto custom = options;
    custom.preset = ChickenRiceGenerationPreset::Custom;
    if (!validateChickenRiceGenerationOptions(options).isEmpty()
        || !validateChickenRiceGenerationOptions(custom).isEmpty()) return;
    const QJsonObject object{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("preset"), chickenRiceGenerationPresetKey(options.preset)},
        {QStringLiteral("vadThreshold"), options.vadThreshold},
        {QStringLiteral("minSpeechDurationMs"), options.minSpeechDurationMs},
        {QStringLiteral("minSilenceDurationMs"), options.minSilenceDurationMs},
        {QStringLiteral("speechPadMs"), options.speechPadMs},
        {QStringLiteral("mergeSegments"), options.mergeSegments}};
    m_settings->setValue(GenerationSettingsKey, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    m_settings->sync();
    m_generationSaveStatus->setText(m_settings->status() == QSettings::NoError ? QString()
        : QStringLiteral("生成参数保存失败，请检查设置存储位置。"));
    m_generationSaveStatus->setVisible(!m_generationSaveStatus->text().isEmpty());
}

void ChickenRiceTranslateDialog::discoverInstalledEngine(const QString &root)
{
    m_discovering = true;
    m_discoveryCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_discoveryCancelled;
    auto promise = std::make_shared<QPromise<QString>>();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString candidate = watcher->result();
        watcher->deleteLater();
        m_discovering = false;
        if (!m_shuttingDown && m_engineDirectory->text().trimmed().isEmpty()) {
            m_loadingSettings = true;
            m_engineDirectory->setText(candidate);
            m_loadingSettings = false;
            scheduleEngineCheck();
            if (candidate.isEmpty())
                setModeFeedback(0, QStringLiteral("请浏览选择已安装的引擎目录；有多个安装版本时需手动选择。"));
        }
        updatePresentation();
        finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([root, cancelled, promise] {
        promise->start();
        QString candidate;
        const QRegularExpression fingerprint(QStringLiteral("^[0-9a-fA-F]{64}$"));
        const auto directories = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const auto &directory : directories) {
            if (cancelled->load())
                break;
            QFile marker(QDir(directory.absoluteFilePath()).filePath(QStringLiteral(".arcanblade-install.json")));
            if (QFileInfo(marker).isSymLink() || !marker.open(QIODevice::ReadOnly) || marker.size() > 16384)
                continue;
            const auto record = QJsonDocument::fromJson(marker.readAll()).object();
            if (record.value(QStringLiteral("owner")).toString() != QStringLiteral("Arcanblade.ChickenRice.Install.v1")
                || !fingerprint.match(record.value(QStringLiteral("fingerprint")).toString()).hasMatch())
                continue;
            if (!candidate.isEmpty()) {
                candidate.clear();
                break;
            }
            candidate = directory.absoluteFilePath();
        }
        promise->addResult(cancelled->load() ? QString() : candidate);
        promise->finish();
    });
}

void ChickenRiceTranslateDialog::persistSettings()
{
    if (!m_settings)
        return;
    m_settings->setValue(Prefix + QStringLiteral("engineDirectory"), m_frozenRequest.engineDirectory);
    m_settings->setValue(Prefix + QStringLiteral("computeMode"),
        aiSubtitleComputeModeKey(m_frozenRequest.computeMode));
}

void ChickenRiceTranslateDialog::persistDraftSettings()
{
    if (!m_preferencesDirty || !m_settings || !taskEditingAllowed())
        return;
    const QString engine = normalizedPath(m_engineDirectory->text());
    if (!engine.isEmpty())
        m_settings->setValue(Prefix + QStringLiteral("engineDirectory"), engine);
    m_settings->setValue(Prefix + QStringLiteral("computeMode"), m_computeMode->currentData().toString());
    m_preferencesDirty = false;
}

void ChickenRiceTranslateDialog::addAudioFiles(const QStringList &paths)
{
    if (m_shuttingDown)
        return;
    if (!paths.isEmpty() && subtitleTranslationMode()) {
        if (!modeSwitchAllowed()) return;
        m_modeCombo->setCurrentIndex(0);
    }
    // MainWindow also calls this for an empty list when reopening the entry.
    showGenerationPage();
    for (const auto &input : paths) {
        const QString path = normalizedPath(input);
        if (path.isEmpty() || std::any_of(m_items.cbegin(), m_items.cend(),
                [&path](const auto &item) { return samePath(item.audioPath, path); }))
            continue;
        const int row = static_cast<int>(m_items.size());
        m_items.append({path, State::Idle, {}, {}, {}, false,
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), {}});
        m_table->insertRow(row);
        for (int column = 0; column < 2; ++column)
            m_table->setItem(row, column, new QTableWidgetItem);
        updateRow(row);
    }
    if (m_table->currentRow() < 0 && !m_items.isEmpty())
        m_table->selectRow(0);
    if (!paths.isEmpty()) scheduleQueueHistorySave();
    updatePresentation();
}

void ChickenRiceTranslateDialog::addLibraryAudioFiles(const QString &workRoot, const QStringList &relativePaths)
{
    if (m_shuttingDown)
        return;
    const QString root = normalizedPath(workRoot);
    if (root.isEmpty())
        return;
    for (const auto &input : relativePaths) {
        const QString relative = QDir::fromNativeSeparators(input);
        const QString cleaned = QDir::cleanPath(relative);
        if (relative.isEmpty() || relative.contains(QChar::Null) || QDir::isAbsolutePath(relative)
            || cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../"))
            || cleaned == QStringLiteral("."))
            continue;
        const QString path = normalizedPath(QDir(root).filePath(relative));
        addAudioFiles({path});
        for (auto &item : m_items) {
            if (samePath(item.audioPath, path)) {
                // Upgrade a previously explicit file without losing its library
                // containment check when duplicate absolute paths are coalesced.
                item.libraryRoot = root;
                item.libraryRelativePath = relative;
                scheduleQueueHistorySave();
                break;
            }
        }
    }
}

bool ChickenRiceTranslateDialog::addLibraryAudioFilesForMode(
    const QString &workRoot, const QStringList &relativePaths, const QString &modeId)
{
    const int mode = m_modeCombo->findData(modeId);
    const auto root = normalizedPath(workRoot);
    if (mode < 0 || mode > 1 || relativePaths.isEmpty() || m_shuttingDown || root.isEmpty()
        || !QDir::isAbsolutePath(workRoot)) return false;
    // Validate all inputs before replacing a completed session display.
    for (const auto &input : relativePaths) {
        const auto relative = QDir::fromNativeSeparators(input);
        const auto parts = relative.split(QLatin1Char('/'));
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(QLatin1Char(':'))
            || relative.contains(QChar::Null) || parts.contains(QStringLiteral(".."))
            || parts.contains(QStringLiteral(".")) || parts.contains(QString())) return false;
    }
    // Accept frozen selections while read-only discovery/checks are running.
    // Start remains disabled until preparation and validation have completed.
    if (!modeSwitchAllowed()) {
        m_feedback = QStringLiteral("当前任务或组件准备尚未结束，请完成或取消后再添加本次选择。");
        updatePresentation();
        return false;
    }
    if (mode != m_modeCombo->currentIndex())
        m_modeCombo->setCurrentIndex(mode);
    m_feedback.clear();
    addLibraryAudioFiles(workRoot, relativePaths);
    return true;
}

void ChickenRiceTranslateDialog::setCurrentAudioPath(const QString &path)
{
    Q_UNUSED(path); // Kept for source compatibility; results are now associated by batch.
}

bool ChickenRiceTranslateDialog::isBusy() const
{
    return m_discovering || m_checkingEngine || m_validationWorkers > 0
        || m_queueHistoryWorkers > 0 || m_queueHistoryTimer->isActive()
        || translationBusy() || (m_componentPanel && m_componentPanel->isBusy())
        || (m_fasterWhisperPanel && m_fasterWhisperPanel->isBusy())
        || (m_translationSettingsPanel && m_translationSettingsPanel->isBusy());
}

void ChickenRiceTranslateDialog::browseFolder()
{
    if (!taskEditingAllowed() || m_folderPicker)
        return;
    auto *dialog = new QFileDialog(this, QStringLiteral("选择已安装的引擎目录"), m_engineDirectory->text());
    dialog->setObjectName(QStringLiteral("translateEnginePicker"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly);
    m_folderPicker = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (m_folderPicker != dialog)
            return;
        m_folderPicker = nullptr;
        if (taskEditingAllowed() && result == QDialog::Accepted && !dialog->selectedFiles().isEmpty()) {
            m_engineDirectory->setText(dialog->selectedFiles().first());
            scheduleEngineCheck();
            m_feedback.clear();
            persistDraftSettings();
        }
        updatePresentation();
    });
    dialog->open();
}

void ChickenRiceTranslateDialog::browseAudioFiles()
{
    if (!taskEditingAllowed() || m_audioPicker)
        return;
    auto *dialog = new QFileDialog(this, QStringLiteral("添加待生成字幕的音频"));
    dialog->setObjectName(QStringLiteral("translateAudioPicker"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setFileMode(QFileDialog::ExistingFiles);
    dialog->setNameFilters({QStringLiteral("音频文件 (*.mp3 *.wav)"),
        QStringLiteral("所有文件 (*)")});
    m_audioPicker = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (m_audioPicker != dialog)
            return;
        m_audioPicker = nullptr;
        if (taskEditingAllowed() && result == QDialog::Accepted)
            addAudioFiles(dialog->selectedFiles());
        updatePresentation();
    });
    dialog->open();
}

void ChickenRiceTranslateDialog::startQueue(bool retry)
{
    if (!taskEditingAllowed()) return;
    const bool japanese = japaneseMode();
    if (japanese) { ensureFasterWhisperPanel(); ensureTranscribeService(); }
    const QString engine = japanese ? m_fasterWhisperPanel->checkedEngineDirectory()
        : normalizedPath(m_engineDirectory->text());
    const bool ready = japanese ? m_fasterWhisperPanel->isReady()
        : !m_checkingEngine && samePath(engine, m_validatedEngineDirectory);
    if (!ready || (japanese ? !m_transcribeService : !m_service)) {
        m_feedback = QStringLiteral("请先选择并检查有效的组件及模型。");
        updatePresentation();
        return;
    }
    const auto options = generationOptions();
    const auto asrOptions = japanese ? m_fasterWhisperPanel->generationOptions() : FasterWhisperGenerationOptions{};
    const QString optionsError = japanese ? validateFasterWhisperGenerationOptions(asrOptions)
        : validateChickenRiceGenerationOptions(options);
    if (!optionsError.isEmpty()) {
        m_feedback = QStringLiteral("生成参数无效：%1").arg(optionsError);
        updatePresentation();
        return;
    }
    int pending = 0;
    for (int row = 0; row < m_items.size(); ++row) {
        auto &item = m_items[row];
        if (retry && (item.state == State::Failed || item.state == State::Cancelled)) {
            item.state = State::Idle;
            item.result = {};
            item.interrupted = false;
            updateRow(row);
        }
        pending += item.state == State::Idle;
    }
    if (!pending) return;
    // Assign once before persisting or launching any item. A retry retains its
    // original output group; audio added during a run receives a later group.
    const QString newBatchId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))
        + QLatin1Char('_') + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    QVector<int> assignedRows;
    for (int row = 0; row < m_items.size(); ++row) {
        auto &item = m_items[row];
        if (item.state != State::Idle || !item.outputBatchId.isEmpty()) continue;
        item.outputBatchId = newBatchId;
        assignedRows.append(row);
    }
    QHash<QString, int> names;
    for (const auto &item : m_items)
        if (item.outputBatchId == newBatchId) ++names[outputNameKey(item.audioPath)];
    for (const int row : assignedRows) {
        auto &item = m_items[row];
        if (names.value(outputNameKey(item.audioPath)) > 1)
            item.outputFileSuffix = QString::fromLatin1(QCryptographicHash::hash(
                item.taskId.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    }
    m_batchMode = m_modeCombo->currentData().toString();
    const QString jobs = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (japanese) {
        m_frozenTranscribeRequest = {};
        m_frozenTranscribeRequest.engineDirectory = engine;
        m_frozenTranscribeRequest.modelDirectory = m_fasterWhisperPanel->checkedModelDirectory();
        m_frozenTranscribeRequest.outputRoot = QDir(jobs).filePath(QStringLiteral("Ai/FasterWhisperJobs"));
        m_frozenTranscribeRequest.computeMode = m_fasterWhisperPanel->computeMode();
        m_frozenTranscribeRequest.generationOptions = asrOptions;
    } else {
        m_frozenRequest = {};
        m_frozenRequest.engineDirectory = engine;
        m_frozenRequest.outputRoot = QDir(jobs).filePath(QStringLiteral("Ai/ChickenRiceJobs"));
        m_frozenRequest.computeMode = aiSubtitleComputeModeFromKey(m_computeMode->currentData().toString())
            .value_or(ChickenRiceComputeMode::GpuLowMemory);
        m_frozenRequest.generationOptions = options;
        persistGenerationOptions();
        persistSettings();
    }
    m_queueRunning = true;
    m_batchActive = true;
    m_batchResults.clear();
    m_batchTotal = pending;
    m_batchRowLimit = static_cast<int>(m_items.size());
    m_batchStarted = 0;
    m_currentPercent = -1;
    m_cancelRequested = false;
    m_feedback.clear();
    m_logTail.clear();
    closePickers();
    startNext();
}

void ChickenRiceTranslateDialog::startNext()
{
    // A queued continuation from a cancelled batch may arrive after a retry
    // has already started. Never replace the identity of an active request.
    if (!m_queueRunning || m_activeRow >= 0) return;
    const bool japanese = m_batchMode == QStringLiteral("offline-asr-ja");
    if (m_cancelRequested || m_shuttingDown || (japanese ? !m_transcribeService : !m_service)) {
        settleBatch();
        return;
    }
    m_activeRow = -1;
    for (int row = 0; row < std::min(m_batchRowLimit, static_cast<int>(m_items.size())); ++row) {
        if (m_items[row].state == State::Idle) { m_activeRow = row; break; }
    }
    if (m_activeRow < 0) {
        m_feedback.clear();
        settleBatch();
        return;
    }
    auto &item = m_items[m_activeRow];
    item.state = State::Preparing;
    QJsonObject parameters{{QStringLiteral("version"),1},
        {QStringLiteral("mode"),m_batchMode},{QStringLiteral("audioPath"),item.audioPath},
        {QStringLiteral("libraryRoot"),item.libraryRoot},{QStringLiteral("libraryRelativePath"),item.libraryRelativePath},
        {QStringLiteral("outputBatchId"),item.outputBatchId},{QStringLiteral("outputFileSuffix"),item.outputFileSuffix}};
    if (japanese) {
        parameters.insert(QStringLiteral("engineDirectory"),m_frozenTranscribeRequest.engineDirectory);
        parameters.insert(QStringLiteral("modelDirectory"),m_frozenTranscribeRequest.modelDirectory);
        parameters.insert(QStringLiteral("outputRoot"),m_frozenTranscribeRequest.outputRoot);
        parameters.insert(QStringLiteral("computeMode"),aiSubtitleComputeModeKey(m_frozenTranscribeRequest.computeMode));
        parameters.insert(QStringLiteral("generation"),fasterWhisperGenerationRecord(
            m_frozenTranscribeRequest.generationOptions,m_frozenTranscribeRequest.computeMode));
    } else {
        parameters.insert(QStringLiteral("engineDirectory"),m_frozenRequest.engineDirectory);
        parameters.insert(QStringLiteral("outputRoot"),m_frozenRequest.outputRoot);
        parameters.insert(QStringLiteral("computeMode"),aiSubtitleComputeModeKey(m_frozenRequest.computeMode));
        parameters.insert(QStringLiteral("generation"),chickenRiceGenerationRecord(m_frozenRequest.generationOptions));
    }
    item.attempts.append({QUuid::createUuid().toString(QUuid::WithoutBraces),item.taskId,
        QStringLiteral("running"),parameters,{},QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),{}});
    m_currentPercent = -1;
    ++m_batchStarted;
    m_taskElapsed.start();
    m_feedback = QStringLiteral("第 %1/%2 项 · 准备任务").arg(m_batchStarted).arg(m_batchTotal);
    updateRow(m_activeRow);
    // Record the immutable attempt before the inference service can touch files.
    m_launchAfterQueueSave=true;
    scheduleQueueHistorySave();
    persistQueueHistory();
    updatePresentation();
}

void ChickenRiceTranslateDialog::launchActiveTask()
{
    if (m_activeRow < 0 || m_activeRow >= m_items.size()) return;
    const auto &item=m_items.at(m_activeRow);
    if (m_cancelRequested || m_shuttingDown) {
        AiSubtitleTaskResult result; result.audioPath=item.audioPath; result.state=State::Cancelled;
        applyResult(result); return;
    }
    const bool japanese=m_batchMode==QStringLiteral("offline-asr-ja");
    bool accepted = false;
    if (japanese) {
        auto request = m_frozenTranscribeRequest;
        request.audioPath = item.audioPath;
        request.libraryRoot = item.libraryRoot;
        request.libraryRelativePath = item.libraryRelativePath;
        request.outputBatchId = item.outputBatchId;
        request.outputFileSuffix = item.outputFileSuffix;
        accepted = m_transcribeService->start(request);
    } else {
        auto request = m_frozenRequest;
        request.audioPath = item.audioPath;
        request.libraryRoot = item.libraryRoot;
        request.libraryRelativePath = item.libraryRelativePath;
        request.outputBatchId = item.outputBatchId;
        request.outputFileSuffix = item.outputFileSuffix;
        accepted = m_service->start(request);
    }
    if (!accepted) {
        AiSubtitleTaskResult result;
        result.audioPath=item.audioPath; result.state=State::Failed;
        result.error=QStringLiteral("生成服务未接受此任务。");
        applyResult(result);
    }
    updatePresentation();
}

void ChickenRiceTranslateDialog::cancelQueue()
{
    m_cancelRequested = true;
    bool historyChanged = false;
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items[row].state == State::Idle) {
            m_items[row].state = State::Cancelled;
            historyChanged = true;
            updateRow(row);
        }
    }
    if (historyChanged) scheduleQueueHistorySave();
    if (m_service && m_service->isBusy())
        m_service->cancel();
    if (m_transcribeService && m_transcribeService->isBusy())
        m_transcribeService->cancel();
    m_feedback = m_activeRow >= 0 ? QStringLiteral("正在停止当前任务…") : QString();
    if (m_activeRow < 0) {
        settleBatch();
        return;
    }
    updatePresentation();
    finishShutdownIfReady();
}

void ChickenRiceTranslateDialog::applyProgress(const AiSubtitleTaskProgress &progress)
{
    if (m_activeRow < 0 || !m_queueRunning)
        return;
    if (progress.state == State::Preparing || progress.state == State::CheckingModel
        || progress.state == State::Running || progress.state == State::Validating)
        m_items[m_activeRow].state = progress.state;
    m_logTail = (progress.message + QLatin1Char('\n') + progress.logTail).right(12000);
    // A percentage describes only this phase, never the complete task/batch.
    m_currentPercent = progress.state == State::CheckingModel || progress.state == State::Running
        ? progress.percent : -1;
    QString phase = stateText(m_items.at(m_activeRow).state);
    if (progress.state == State::CheckingModel && progress.percent >= 0)
        phase = QStringLiteral("校验模型 %1%").arg(std::clamp(progress.percent, 0, 100));
    else if (progress.state == State::Running) {
        const bool japanese = m_batchMode == QStringLiteral("offline-asr-ja");
        phase = progress.percent >= 0
            ? (japanese ? QStringLiteral("日语识别 %1%") : QStringLiteral("离线直译 %1%"))
                .arg(std::clamp(progress.percent, 0, 100))
            : japanese ? QStringLiteral("正在识别日语") : QStringLiteral("正在离线直译");
    } else if (progress.state == State::Validating) {
        phase = QStringLiteral("校验并保存字幕");
    }
    m_feedback = m_cancelRequested ? QStringLiteral("正在停止当前任务…")
        : QStringLiteral("第 %1/%2 项 · %3").arg(m_batchStarted).arg(m_batchTotal).arg(phase);
    updateRow(m_activeRow);
    updatePresentation();
}

void ChickenRiceTranslateDialog::applyResult(const AiSubtitleTaskResult &result)
{
    if (m_activeRow < 0)
        return;
    auto &item = m_items[m_activeRow];
    item.result = result;
    if (!samePath(normalizedPath(result.audioPath), item.audioPath)) {
        item.result = {};
        item.result.error = QStringLiteral("结果与请求音轨不一致，未接受此结果。");
        item.state = State::Failed;
    } else if (result.state == State::Succeeded && normalizedPath(result.srtPath).isEmpty()) {
        item.result.srtPath.clear();
        item.result.error = QStringLiteral("生成结果未提供有效字幕路径。");
        item.state = State::Failed;
    } else {
        item.state = result.state == State::Succeeded || result.state == State::Cancelled || result.state == State::Failed
            ? result.state : State::Failed;
    }
    if (item.state == State::Succeeded)
        m_batchResults.append(item.result);
    if (!item.attempts.isEmpty()) {
        auto &attempt=item.attempts.last();
        attempt.state=item.state==State::Succeeded ? QStringLiteral("succeeded")
            : item.state==State::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
        attempt.finishedAt=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        attempt.result={{QStringLiteral("srtPath"),item.result.srtPath},
            {QStringLiteral("taskDirectory"),item.result.taskDirectory},{QStringLiteral("logPath"),item.result.logPath},
            {QStringLiteral("error"),item.result.error},{QStringLiteral("exitCode"),item.result.exitCode},
            {QStringLiteral("subtitleCount"),qint64(item.result.subtitleCount)},
            {QStringLiteral("warnings"),QJsonArray::fromStringList(item.result.warnings)},
            {QStringLiteral("language"),item.result.language}};
    }
    scheduleQueueHistorySave();
    updateRow(m_activeRow);
    m_activeRow = -1;
    m_currentPercent = -1;
    if (m_cancelRequested || m_shuttingDown) {
        m_feedback.clear();
        settleBatch();
    } else {
        updatePresentation();
        QTimer::singleShot(0, this, &ChickenRiceTranslateDialog::startNext);
    }
}

void ChickenRiceTranslateDialog::updateRow(int row)
{
    const auto &item = m_items.at(row);
    m_table->item(row, 0)->setText(QFileInfo(item.audioPath).fileName());
    m_table->item(row, 0)->setToolTip(item.audioPath);
    const bool completedWithWarnings = item.state == State::Succeeded && !item.result.warnings.isEmpty();
    m_table->item(row, 1)->setText(completedWithWarnings ? QStringLiteral("已完成（有警告）")
        : item.interrupted && item.state == State::Cancelled ? QStringLiteral("已中断") : stateText(item.state));
    m_table->item(row, 0)->setData(Qt::UserRole, item.state == State::Succeeded ? item.result.srtPath : QString());
    QString details = item.result.error;
    if (!item.result.warnings.isEmpty()) {
        if (!details.isEmpty()) details += QLatin1Char('\n');
        details += item.result.warnings.join(QLatin1Char('\n'));
        if (!item.result.logPath.isEmpty()) details += QLatin1Char('\n') + item.result.logPath;
    }
    m_table->item(row, 1)->setToolTip(details);
}

void ChickenRiceTranslateDialog::updateProgressFeedback()
{
    const bool active = m_queueRunning && m_activeRow >= 0 && m_taskElapsed.isValid();
    QString text = m_feedback;
    if (active) {
        if (!m_elapsedTimer->isActive()) m_elapsedTimer->start();
        if (!text.isEmpty()) {
            const qint64 seconds = m_taskElapsed.elapsed() / 1000;
            text += QStringLiteral(" · 已用 %1:%2").arg(seconds / 60)
                .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        }
    } else {
        m_elapsedTimer->stop();
        m_taskElapsed.invalidate();
    }
    m_status->setText(text);
    m_status->setVisible(!subtitleTranslationMode() && !text.isEmpty());
}

void ChickenRiceTranslateDialog::updatePresentation()
{
    const bool generating = translationBusy();
    const bool installing = m_componentPanel && m_componentPanel->installationBusy();
    const bool editable = taskEditingAllowed() && !m_folderPicker && !m_audioPicker;
    m_engineDirectory->setEnabled(editable);
    m_computeMode->setEnabled(editable);
    m_modeCombo->setEnabled(modeSwitchAllowed());
    m_generationParameters->setEnabled(editable);
    const auto preset = chickenRiceGenerationPresetFromKey(m_generationPreset->currentData().toString());
    m_customGenerationParameters->setVisible(preset == ChickenRiceGenerationPreset::Custom);
    m_generationPresetHint->setText(preset == ChickenRiceGenerationPreset::QuietSpeech
        ? QStringLiteral("检测阈值 0.4：尝试捕捉轻声，也可能增加误识别；效果需结合音频判断。")
        : preset == ChickenRiceGenerationPreset::Conservative
        ? QStringLiteral("检测阈值 0.6：更保守地判断语音，可能遗漏轻声；效果需结合音频判断。")
        : QString());
    m_generationPresetHint->setVisible(!m_generationPresetHint->text().isEmpty());
    m_browseEngineButton->setEnabled(editable);
    m_addAudioButton->setEnabled(editable);
    m_clearAction->setEnabled(editable && !m_items.isEmpty());
    m_clearButton->setEnabled(m_clearAction->isEnabled());
    m_componentSettingsButton->setEnabled(!m_shuttingDown && (!subtitleTranslationMode() || !generating));
    const bool preparingWhisper = m_fasterWhisperPanel && m_fasterWhisperPanel->installationBusy();
    m_settingsHeading->setText(subtitleTranslationMode() ? QStringLiteral("翻译设置")
        : japaneseMode() ? QStringLiteral("组件与设置 · 日语识别")
        : QStringLiteral("组件与设置 · 离线直译"));
    if (m_translationSettingsPanel) m_translationSettingsPanel->setEnabled(!m_shuttingDown && !generating);
    m_subtitleTranslation->setEnabled(!m_shuttingDown
        && !(m_translationSettingsPanel && m_translationSettingsPanel->isBusy()));
    if (m_componentPanel) m_componentPanel->setEnabled(!m_shuttingDown && !generating && !preparingWhisper);
    if (m_fasterWhisperPanel) m_fasterWhisperPanel->setEnabled(!m_shuttingDown && !generating && !installing);
    const QString engine = normalizedPath(m_engineDirectory->text());
    const bool checked = !m_checkingEngine && samePath(engine, m_validatedEngineDirectory);
    const QString offlineStatus = installing ? QStringLiteral("正在准备离线组件…")
        : m_discovering ? QStringLiteral("正在查找已有组件…")
        : m_checkingEngine ? QStringLiteral("正在检查离线组件…")
        : m_engineDirectory->text().trimmed().isEmpty() ? QStringLiteral("请选择组件目录")
        : checked ? QStringLiteral("组件检查通过")
        : QStringLiteral("组件检查失败：%1").arg(m_validationError);
    const QString offlineTooltip = engine + (m_validationError.isEmpty() ? QString() : QLatin1Char('\n') + m_validationError);
    const QString offlineCheckState = installing || m_discovering || m_checkingEngine
        ? QStringLiteral("pending") : m_engineDirectory->text().trimmed().isEmpty()
        ? QStringLiteral("unselected") : checked ? QStringLiteral("passed") : QStringLiteral("failed");
    m_settingsComponentStatus->setText(offlineStatus);
    m_settingsComponentStatus->setToolTip(offlineTooltip);
    m_settingsComponentStatus->setProperty("validationState", offlineCheckState);
    updateStatusIcon(m_settingsComponentStatusIcon, offlineCheckState);
    const bool japanese = japaneseMode();
    const bool generationReady = japanese ? m_fasterWhisperPanel && m_fasterWhisperPanel->isReady() : checked;
    const bool generationService = japanese ? bool(m_transcribeService) : bool(m_service);
    const QString checkState = japanese
        ? m_fasterWhisperPanel ? m_fasterWhisperPanel->validationState() : QStringLiteral("unselected")
        : offlineCheckState;
    m_componentStatus->setText(japanese ? m_fasterWhisperPanel ? m_fasterWhisperPanel->statusText()
        : QStringLiteral("请选择组件目录") : offlineStatus);
    m_componentStatus->setToolTip(japanese && m_fasterWhisperPanel
        ? m_fasterWhisperPanel->checkedEngineDirectory() + QLatin1Char('\n') + m_fasterWhisperPanel->checkedModelDirectory()
        : offlineTooltip);
    m_componentStatus->setProperty("validationState", checkState);
    updateStatusIcon(m_componentStatusIcon, checkState);
    m_introduction->setText(japanese ? QStringLiteral("日语音频 → 日语字幕 · 完全离线")
        : QStringLiteral("日语音频 → 中文字幕 · 完全离线"));
    m_modeConfiguration->currentWidget()->setProperty("modeId", m_modeCombo->currentData());
    int succeeded = 0, failed = 0, cancelled = 0, pending = 0;
    for (const auto &item : m_items) {
        succeeded += item.state == State::Succeeded;
        failed += item.state == State::Failed;
        cancelled += item.state == State::Cancelled;
        pending += item.state == State::Idle;
    }
    m_summary->setText(QStringLiteral("共 %1 项 · 完成 %2 · 失败 %3 · 已取消 %4")
        .arg(m_items.size()).arg(succeeded).arg(failed).arg(cancelled));
    m_startButton->setEnabled(editable && generationService && generationReady && pending > 0);
    m_startButton->setVisible(!generating && (pending > 0 || failed + cancelled == 0));
    m_retryButton->setEnabled(editable && generationService && generationReady && failed + cancelled > 0);
    m_retryButton->setVisible(!generating && failed + cancelled > 0);
    m_cancelButton->setEnabled(!m_shuttingDown && m_queueRunning && !m_cancelRequested);
    m_cancelButton->setVisible(generating);
    m_activity->setRange(0, m_currentPercent >= 0 ? 100 : 0);
    if (m_currentPercent >= 0) m_activity->setValue(std::clamp(m_currentPercent, 0, 100));
    const bool checkingModel = m_activeRow >= 0 && m_items.at(m_activeRow).state == State::CheckingModel;
    const QString progressPhase = checkingModel ? QStringLiteral("模型校验")
        : m_batchMode == QStringLiteral("offline-asr-ja") ? QStringLiteral("日语识别") : QStringLiteral("离线直译");
    m_activity->setFormat(progressPhase + QStringLiteral(" %p%"));
    m_activity->setToolTip(m_currentPercent >= 0
        ? QStringLiteral("%1阶段进度，不表示整项或整批完成比例。").arg(progressPhase)
        : QStringLiteral("当前阶段没有可靠百分比；活动条仅表示正在处理，已用时间不用于估算完成比例。"));
    m_activity->setVisible(generating);
    updateProgressFeedback();
    const bool cloud = subtitleTranslationMode();
    m_subtitleTranslation->setVisible(cloud);
    m_modeConfiguration->setVisible(!cloud);
    m_table->setVisible(!cloud);
    m_summary->setVisible(!cloud);
    m_queueHistoryStatus->setVisible(!cloud && !m_queueHistoryStatus->text().isEmpty());
    m_activity->setVisible(!cloud && generating);
    m_addAudioButton->setVisible(!cloud);
    m_clearButton->setVisible(!cloud);
    m_startButton->setVisible(!cloud && !generating && (pending > 0 || failed + cancelled == 0));
    m_retryButton->setVisible(!cloud && !generating && failed + cancelled > 0);
    m_cancelButton->setVisible(!cloud && generating);
    m_componentSettingsButton->setText(cloud ? QStringLiteral("翻译设置") : QStringLiteral("组件与设置"));
    updateContextActions();
    refreshContentMinimum();
}

void ChickenRiceTranslateDialog::updateContextActions()
{
    const int row = m_table->currentRow();
    const bool selected = row >= 0 && row < m_items.size();
    m_removeAction->setEnabled(selected && taskEditingAllowed());
    m_openFolderAction->setVisible(selected && m_items.at(row).state == State::Succeeded);
    m_openFolderAction->setEnabled(selected && !m_shuttingDown
        && m_items.at(row).state == State::Succeeded && !m_items.at(row).result.srtPath.isEmpty());
    m_errorAction->setVisible(selected && m_items.at(row).state == State::Failed);
    m_errorAction->setEnabled(selected && !m_shuttingDown && m_items.at(row).state == State::Failed);
    const bool translatable = japaneseMode() && selected && m_items.at(row).state == State::Succeeded
        && !m_items.at(row).result.srtPath.isEmpty();
    m_translateSubtitleAction->setVisible(translatable);
    m_translateSubtitleAction->setEnabled(translatable && modeSwitchAllowed());
    m_translateSubtitleButton->setVisible(japaneseMode());
    m_translateSubtitleButton->setEnabled(japaneseMode() && modeSwitchAllowed());
    m_openResultFolderButton->setVisible(!subtitleTranslationMode());
    m_openResultFolderButton->setEnabled(m_openFolderAction->isEnabled());
}

void ChickenRiceTranslateDialog::showQueueContextMenu(const QPoint &position)
{
    const auto index = m_table->indexAt(position);
    if (!index.isValid()) return;
    m_table->selectRow(index.row());
    updateContextActions();
    auto *menu = m_table->findChild<QMenu *>(QStringLiteral("translateQueueContextMenu"));
    menu->popup(m_table->viewport()->mapToGlobal(position));
}

void ChickenRiceTranslateDialog::showSelectedError()
{
    const int row = m_table->currentRow();
    if (m_shuttingDown || row < 0 || row >= m_items.size() || m_items.at(row).state != State::Failed) return;
    if (m_errorDialog) { m_errorDialog->raise(); return; }
    const auto &item = m_items.at(row);
    auto *dialog = new TaskErrorDialog(item.result.error, item.audioPath,
        item.result.logPath, item.result.warnings, this);
    m_errorDialog = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_errorDialog == dialog) m_errorDialog = nullptr;
        updatePresentation();
    });
    dialog->open();
    updatePresentation();
}

void ChickenRiceTranslateDialog::settleBatch()
{
    m_queueRunning = false;
    m_activeRow = -1;
    m_currentPercent = -1;
    const bool notify = std::exchange(m_batchActive, false);
    const auto results = std::exchange(m_batchResults, {});
    // Only this execution's successful ASR results are handed off. Reopening
    // saved history must not repopulate a deliberately cleared translation list.
    // Append before batchFinished: listeners may synchronously delete us.
    if (notify && !m_shuttingDown && m_batchMode == QStringLiteral("offline-asr-ja")) {
        int rejected = 0;
        for (const auto &result : results) {
            const auto item = std::find_if(m_items.cbegin(), m_items.cend(), [&result](const auto &value) {
                return value.state == State::Succeeded && samePath(value.audioPath, result.audioPath)
                    && samePath(value.result.srtPath, result.srtPath);
            });
            if (item == m_items.cend() || !m_subtitleTranslation->addSubtitleFile(result.srtPath,
                    item->audioPath, item->libraryRoot, item->libraryRelativePath)) ++rejected;
        }
        if (rejected > 0)
            m_feedback = QStringLiteral("%1 份已生成字幕未能加入翻译列表；结果已保留，可稍后右键“翻译成中文…”重试。")
                .arg(rejected);
    }
    persistQueueHistory();
    updatePresentation();
    QPointer<ChickenRiceTranslateDialog> guard(this);
    if (notify) emit batchFinished(results);
    if (guard) finishShutdownIfReady();
}

void ChickenRiceTranslateDialog::scheduleEngineCheck()
{
    if (m_shuttingDown) return;
    if (m_validationCancelled) m_validationCancelled->store(true);
    ++m_validationGeneration;
    m_validatedEngineDirectory.clear();
    m_validationError.clear();
    const QString engine = normalizedPath(m_engineDirectory->text());
    m_checkingEngine = !engine.isEmpty();
    m_validationTimer->stop();
    if (m_checkingEngine) m_validationTimer->start();
    else if (!m_engineDirectory->text().trimmed().isEmpty())
        m_validationError = QStringLiteral("请选择有效的绝对目录路径。");
    updatePresentation();
}

void ChickenRiceTranslateDialog::startEngineCheck()
{
    if (m_shuttingDown) return;
    const QString engine = normalizedPath(m_engineDirectory->text());
    if (engine.isEmpty()) return;
    const auto generation = m_validationGeneration;
    m_validationCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_validationCancelled;
    const auto validator = m_validator;
    ++m_validationWorkers;
    auto promise = std::make_shared<QPromise<QString>>();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, engine, generation, cancelled] {
        const auto error = watcher->result();
        watcher->deleteLater();
        --m_validationWorkers;
        if (!m_shuttingDown && generation == m_validationGeneration
            && samePath(engine, normalizedPath(m_engineDirectory->text()))) {
            m_checkingEngine = false;
            m_validationError = error;
            if (!cancelled->load() && error.isEmpty()) m_validatedEngineDirectory = engine;
        }
        updatePresentation();
        finishShutdownIfReady();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, engine, cancelled, validator] {
        promise->start();
        QString error;
        try {
            if (!cancelled->load()) {
                if (validator) error = validator(engine, *cancelled);
                else if (validateChickenRiceEngine(engine, &error)) {
                    const QString configPath = QDir(engine).filePath(QStringLiteral("generation_config.json5"));
                    QFile config(configPath);
                    if (QFileInfo(configPath).isSymLink() || !config.open(QIODevice::ReadOnly)
                        || config.size() <= 0 || config.size() > 256 * 1024)
                        error = QStringLiteral("generation_config.json5 缺失、不可读或文件大小不正确。");
                }
            }
        } catch (const std::exception &exception) { error = QString::fromUtf8(exception.what()); }
        catch (...) { error = QStringLiteral("组件检查发生未知错误。"); }
        promise->addResult(error);
        promise->finish();
    });
}

void ChickenRiceTranslateDialog::closePickers()
{
    if (m_fasterWhisperPanel) m_fasterWhisperPanel->closePicker();
    if (m_folderPicker)
        m_folderPicker->reject();
    if (m_audioPicker)
        m_audioPicker->reject();
}

void ChickenRiceTranslateDialog::done(int result)
{
    QPointer<ChickenRiceTranslateDialog> guard(this);
    if (m_subtitleTranslation) m_subtitleTranslation->closeTransientDialogs();
    if (!guard) return;
    if (m_translationSettingsPanel) m_translationSettingsPanel->deactivate();
    if (!guard) return;
    persistDraftSettings();
    persistQueueHistory();
    closePickers();
    if (m_errorDialog) m_errorDialog->close();
    QDialog::done(result);
}

void ChickenRiceTranslateDialog::shutdown()
{
    if (!m_shuttingDown) {
        m_shuttingDown = true;
        m_shutdownStarting = true;
        m_validationTimer->stop();
        m_checkingEngine = false;
        ++m_validationGeneration;
        if (m_validationCancelled) m_validationCancelled->store(true);
        if (m_discoveryCancelled)
            m_discoveryCancelled->store(true);
        QPointer<ChickenRiceTranslateDialog> guard(this);
        closePickers();
        if (!guard)
            return;
        if (m_queueRunning) cancelQueue();
        else persistQueueHistory(); // Closing the app leaves an unstarted queue pending.
        if (!guard)
            return;
        if (m_service)
            m_service->shutdown();
        if (!guard)
            return;
        if (m_transcribeService)
            m_transcribeService->shutdown();
        if (!guard)
            return;
        if (m_componentPanel)
            m_componentPanel->shutdown();
        if (!guard)
            return;
        if (m_fasterWhisperPanel)
            m_fasterWhisperPanel->shutdown();
        if (!guard)
            return;
        if (m_subtitleTranslation)
            m_subtitleTranslation->shutdown();
        if (!guard)
            return;
        if (m_translationSettingsPanel)
            m_translationSettingsPanel->shutdown();
        if (!guard)
            return;
        m_shutdownStarting = false;
    }
    updatePresentation();
    if (m_shuttingDown) persistQueueHistory();
    finishShutdownIfReady();
}

void ChickenRiceTranslateDialog::finishShutdownIfReady()
{
    if (m_shuttingDown && !m_shutdownStarting && !isBusy() && !m_shutdownNotified
        && (!m_subtitleTranslation || m_subtitleTranslation->shutdownComplete())) {
        m_shutdownNotified = true;
        m_taskQueueLease.reset();
        emit shutdownFinished();
    }
}

void ChickenRiceTranslateDialog::showEvent(QShowEvent *event)
{
    showGenerationPage();
    QDialog::showEvent(event);
    if (!translationBusy()) {
        if (japaneseMode()) {
            const bool alreadyStarted = m_fasterWhisperCheckStarted;
            ensureFasterWhisperPanel();
            if (alreadyStarted && !m_fasterWhisperPanel->isBusy()) m_fasterWhisperPanel->recheck();
        } else if (!subtitleTranslationMode() && !(m_componentPanel && m_componentPanel->installationBusy()) && !m_checkingEngine) {
            scheduleEngineCheck();
        }
    }
    if (subtitleTranslationMode()) m_subtitleTranslation->refreshSettings();
    // DialogFrame measures once more on first show. Keep its decoration while
    // allowing a compact viewport to scroll either page after that measurement.
    refreshContentMinimum();
}

bool ChickenRiceTranslateDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    return m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result);
}
} // namespace asmr
