#include "ui/WorkEditDialog.h"

#include "ui/CoverArtWidget.h"
#include "ui/TitleBarWidget.h"
#include "ui/WindowFrameController.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPromise>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>

#include <exception>
#include <utility>

namespace asmr {
namespace {
// QLineEdit's built-in clear tool button inherits the application's generic
// tool-button padding. Give this control its own hit area and geometry instead.
class TitleClearButton final : public QToolButton {
public:
    explicit TitleClearButton(QWidget *parent) : QToolButton(parent)
    {
        setObjectName(QStringLiteral("workEditTitleClearButton"));
        setAccessibleName(QStringLiteral("清空作品标题"));
        setToolTip(QStringLiteral("清空作品标题"));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::ArrowCursor);
        setStyleSheet(QStringLiteral(
            "QToolButton#workEditTitleClearButton { background: transparent; "
            "border: 1px solid transparent; border-radius: 4px; padding: 0; "
            "margin: 0; min-width: 0; min-height: 0; outline: none; }"
            "QToolButton#workEditTitleClearButton:hover:enabled { background: #303A42; }"
            "QToolButton#workEditTitleClearButton:pressed:enabled { background: #3B4A4C; }"
            "QToolButton#workEditTitleClearButton[buttonKeyboardFocus=\"true\"]:focus:enabled { border-color: #69C8A5; }"));
    }
protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(isEnabled() ? QColor("#ADB5C1") : QColor("#69717D"),
                            1.4, Qt::SolidLine, Qt::RoundCap));
        painter.translate(width() / 2.0, height() / 2.0);
        painter.drawLine(QPointF(-3.5, -3.5), QPointF(3.5, 3.5));
        painter.drawLine(QPointF(-3.5, 3.5), QPointF(3.5, -3.5));
    }
};

class TitleLineEdit final : public QLineEdit {
public:
    TitleLineEdit(const QString &text, QWidget *parent) : QLineEdit(text, parent)
    {
        setMinimumHeight(36);
        m_clear = new TitleClearButton(this);
        connect(m_clear, &QToolButton::clicked, this, [this] {
            clear();
            setFocus(Qt::OtherFocusReason);
        });
        connect(this, &QLineEdit::textChanged, this, [this] { updateClearButton(); });
        updateClearButton();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLineEdit::resizeEvent(event);
        updateClearButton();
    }
    void changeEvent(QEvent *event) override
    {
        QLineEdit::changeEvent(event);
        if (event->type() == QEvent::LayoutDirectionChange) updateClearButton();
    }
private:
    void updateClearButton()
    {
        if (!m_clear) return;
        const int side = qBound(0, height() - 8, 28);
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        m_clear->setGeometry(rtl ? 4 : width() - side - 4, (height() - side) / 2, side, side);
        const int reserve = text().isEmpty() ? 0 : side + 4;
        setTextMargins(rtl ? reserve : 0, 0, rtl ? 0 : reserve, 0);
        m_clear->setVisible(!text().isEmpty());
    }
    QToolButton *m_clear = nullptr;
};

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
QString safeTooltip(const QString &text)
{
    return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped());
}
}

WorkEditDialog::WorkEditDialog(const QString &workTitle, const QString &rootPath, QWidget *parent)
    : QDialog(parent), m_originalTitle(workTitle.trimmed()), m_rootPath(rootPath)
{
    const QString directoryName = rootPath.isEmpty() ? QString()
        : QFileInfo(QDir::cleanPath(QDir::fromNativeSeparators(rootPath))).fileName().trimmed();
    m_defaultTitle = directoryName.isEmpty() ? m_originalTitle : directoryName;
    setObjectName(QStringLiteral("workEditDialog"));
    setWindowTitle(QStringLiteral("编辑作品"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_StyledBackground);
    setAccessibleName(QStringLiteral("编辑作品标题和封面"));
    QSize available(800, 800);
    const auto *currentScreen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    if (currentScreen) available = currentScreen->availableGeometry().size();
    const QSize maximum = available - QSize(32, 64);
    setMinimumSize(QSize(380, 360).boundedTo(maximum.expandedTo(QSize(1, 1))));
    resize(QSize(520, 620).boundedTo(maximum.expandedTo(QSize(1, 1))));

    m_titleBar = new TitleBarWidget(this, TitleBarWidget::Mode::Dialog);
    m_titleBar->setTitle(QStringLiteral("编辑作品"));
    m_windowFrame = new WindowFrameController(this, m_titleBar);
    connect(m_titleBar, &TitleBarWidget::closeRequested, this, &QDialog::reject);
    connect(m_titleBar, &TitleBarWidget::systemMoveRequested,
            m_windowFrame, &WindowFrameController::startSystemMove);
    connect(m_titleBar, &TitleBarWidget::systemMenuRequested,
            m_windowFrame, &WindowFrameController::showSystemMenu);

    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("workEditContent"));
    auto *scroll = new QScrollArea(content);
    scroll->setObjectName(QStringLiteral("workEditScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#workEditScrollArea { background: transparent; }"));
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("workEditBody"));
    body->setStyleSheet(QStringLiteral("QWidget#workEditBody { background: transparent; }"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(4, 4, 4, 4);
    bodyLayout->setSpacing(12);
    auto *titleLabel = plainLabel(QStringLiteral("workEditTitleLabel"), body);
    titleLabel->setText(QStringLiteral("作品标题"));
    m_titleEdit = new TitleLineEdit(workTitle, body);
    m_titleEdit->setObjectName(QStringLiteral("workEditTitleEdit"));
    m_titleEdit->setAccessibleName(QStringLiteral("作品标题"));
    titleLabel->setBuddy(m_titleEdit);
    bodyLayout->addWidget(titleLabel);
    bodyLayout->addWidget(m_titleEdit);
    m_preview = new CoverArtWidget(body);
    m_preview->setObjectName(QStringLiteral("workEditCoverPreview"));
    m_preview->setContextName(QStringLiteral("编辑作品封面预览"));
    m_preview->setFixedSize(220, 220);
    m_preview->clearImage(QStringLiteral("正在读取当前封面…"));
    bodyLayout->addWidget(m_preview, 0, Qt::AlignHCenter);
    m_chooseButton = new QPushButton(QStringLiteral("选择本地文件"), body);
    m_chooseButton->setObjectName(QStringLiteral("workEditChooseFileButton"));
    m_chooseButton->setAccessibleName(QStringLiteral("选择任意本地图片作为作品封面"));
    bodyLayout->addWidget(m_chooseButton);
    auto *hint = plainLabel(QStringLiteral("workEditCoverHintLabel"), body);
    hint->setProperty("subtleLabel", true);
    hint->setText(QStringLiteral("支持 JPG、JPEG、PNG、WebP、BMP、GIF；最大 32 MiB、3200 万像素。"
                                "GIF 使用首帧。保存时由应用保留图片副本，不修改原图或音频。"));
    bodyLayout->addWidget(hint);
    bodyLayout->addStretch();
    scroll->setWidget(body);

    m_feedbackLabel = plainLabel(QStringLiteral("workEditFeedbackLabel"), content);
    m_feedbackLabel->hide();
    auto *buttons = new QWidget(content);
    buttons->setObjectName(QStringLiteral("workEditButtonBox"));
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);
    buttonLayout->addStretch();
    m_autoButton = new QPushButton(QStringLiteral("恢复默认"), buttons);
    m_autoButton->setObjectName(QStringLiteral("workEditAutomaticButton"));
    m_autoButton->setAccessibleName(QStringLiteral("恢复文件夹标题和封面自动查找"));
    m_autoButton->setToolTip(QStringLiteral("将标题恢复为作品文件夹名称，并恢复封面自动查找；保存后生效。"));
    m_saveButton = new QPushButton(QStringLiteral("保存"), buttons);
    m_saveButton->setObjectName(QStringLiteral("workEditSaveButton"));
    m_saveButton->setProperty("primary", true);
    m_saveButton->setDefault(true);
    m_cancelButton = new QPushButton(QStringLiteral("取消"), buttons);
    m_cancelButton->setObjectName(QStringLiteral("workEditCancelButton"));
    buttonLayout->addWidget(m_autoButton);
    buttonLayout->addWidget(m_saveButton);
    buttonLayout->addWidget(m_cancelButton);
    for (auto *button : {m_chooseButton, m_autoButton, m_cancelButton}) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(20, 16, 20, 16);
    contentLayout->setSpacing(10);
    contentLayout->addWidget(scroll, 1);
    contentLayout->addWidget(m_feedbackLabel);
    contentLayout->addWidget(buttons);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(m_titleBar);
    layout->addWidget(content, 1);
    auto *titleClear = m_titleEdit->findChild<QToolButton *>(QStringLiteral("workEditTitleClearButton"));
    setTabOrder(m_titleEdit, titleClear);
    setTabOrder(titleClear, m_chooseButton);
    setTabOrder(m_chooseButton, m_autoButton);
    setTabOrder(m_autoButton, m_saveButton);
    setTabOrder(m_saveButton, m_cancelButton);
    connect(m_titleEdit, &QLineEdit::textChanged, this, &WorkEditDialog::updateControls);
    connect(m_chooseButton, &QPushButton::clicked, this, &WorkEditDialog::chooseLocalFile);
    connect(m_autoButton, &QPushButton::clicked, this, &WorkEditDialog::restoreAutomatic);
    connect(m_saveButton, &QPushButton::clicked, this, &WorkEditDialog::requestSave);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    updateControls();
    m_titleEdit->setFocus(Qt::OtherFocusReason);
}

WorkEditDialog::~WorkEditDialog()
{
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    if (m_active) m_lifetime.waitForFinished();
}

void WorkEditDialog::setCoverState(const CoverArtResult &result, bool hasManual)
{
    if (m_shuttingDown) return;
    m_initialized = true;
    m_hasManual = hasManual;
    if (m_coverChange == CoverChange::Keep && !m_loading) {
        if (result.status == CoverArtStatus::Ready && !result.image.isNull())
            m_preview->setImage(result.image, result.sourcePath, result.message);
        else m_preview->clearImage(result.message.isEmpty() ? QStringLiteral("暂无封面") : result.message);
    }
    updateControls();
}

QString WorkEditDialog::title() const { return m_titleEdit->text().trimmed(); }

bool WorkEditDialog::validTitle() const
{
    const auto raw = m_titleEdit->text();
    return !raw.trimmed().isEmpty() && !raw.contains(QChar::Null)
        && !raw.contains(QLatin1Char('\r')) && !raw.contains(QLatin1Char('\n'))
        && !raw.contains(QChar(0x2028)) && !raw.contains(QChar(0x2029));
}

void WorkEditDialog::setSaving(bool saving)
{
    m_saving = saving;
    if (saving) setFeedback(QStringLiteral("正在保存作品…"));
    updateControls();
}

void WorkEditDialog::setFeedback(const QString &message)
{
    m_feedbackLabel->setText(message);
    m_feedbackLabel->setToolTip(safeTooltip(message));
    m_feedbackLabel->setVisible(!message.isEmpty());
}

void WorkEditDialog::chooseLocalFile()
{
    if (m_shuttingDown || m_saving || !m_initialized || m_fileDialog) return;
    auto *picker = new QFileDialog(this, QStringLiteral("选择作品封面"), m_rootPath);
    picker->setObjectName(QStringLiteral("workEditFileDialog"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setFileMode(QFileDialog::ExistingFile);
    picker->setAcceptMode(QFileDialog::AcceptOpen);
    picker->setNameFilters({QStringLiteral("图片 (*.jpg *.jpeg *.png *.webp *.bmp *.gif)"),
                            QStringLiteral("所有文件 (*)")});
    if (!m_useNativeFileDialog) picker->setOption(QFileDialog::DontUseNativeDialog);
    m_fileDialog = picker;
    connect(picker, &QFileDialog::fileSelected, this, &WorkEditDialog::selectLocalFile);
    connect(picker, &QFileDialog::finished, this, [this] {
        m_fileDialog = nullptr;
        updateControls();
    });
    picker->open();
    updateControls();
}

void WorkEditDialog::selectLocalFile(const QString &path)
{
    if (m_shuttingDown || m_saving || !m_initialized || path.isEmpty()) return;
    m_coverChange = CoverChange::Replace;
    m_draft = {};
    requestPreview({++m_generation, path, false});
}

void WorkEditDialog::restoreAutomatic()
{
    if (m_shuttingDown || m_saving || !m_initialized) return;
    m_titleEdit->setText(m_defaultTitle);
    m_coverChange = m_hasManual ? CoverChange::Automatic : CoverChange::Keep;
    m_draft = {};
    requestPreview({++m_generation, {}, true});
}

void WorkEditDialog::requestPreview(PreviewTask task)
{
    m_loading = true;
    m_preview->clearImage(QStringLiteral("正在读取封面预览…"));
    setFeedback(QStringLiteral("正在读取封面预览…"));
    if (m_active) {
        m_cancelled->store(true, std::memory_order_relaxed);
        m_pending = std::move(task);
    } else startPreview(std::move(task));
    updateControls();
}

void WorkEditDialog::startPreview(PreviewTask task)
{
    m_active = true;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    auto promise = std::make_shared<QPromise<PreviewResult>>();
    auto *watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this,
            [this, watcher, id = task.id, automatic = task.automatic, path = task.path] {
        auto result = watcher->result();
        watcher->deleteLater();
        m_active = false;
        m_cancelled.reset();
        if (!m_shuttingDown && id == m_generation) {
            m_loading = false;
            if (automatic) {
                if (result.automatic.status == CoverArtStatus::Ready && !result.automatic.image.isNull())
                    m_preview->setImage(result.automatic.image, result.automatic.sourcePath);
                else m_preview->clearImage(QStringLiteral("暂无自动封面"));
                setFeedback(result.automatic.status == CoverArtStatus::Ready
                    ? QStringLiteral("自动封面预览已就绪，保存后生效。")
                    : result.automatic.message.isEmpty() ? QStringLiteral("没有可用的自动封面，将使用占位。")
                                                         : result.automatic.message);
            } else {
                m_draft = std::move(result.draft);
                if (m_draft.success && !m_draft.image.isNull()) {
                    m_preview->setImage(m_draft.image, path);
                    setFeedback(QStringLiteral("图片预览已就绪，点击保存后生效。"));
                } else {
                    m_preview->clearImage(QStringLiteral("图片不可用"));
                    setFeedback(m_draft.errorMessage.isEmpty()
                        ? QStringLiteral("无法读取这张图片，请选择其他图片或恢复自动查找。")
                        : m_draft.errorMessage);
                }
            }
            updateControls();
        }
        if (!m_shuttingDown && m_pending) {
            auto next = std::move(*m_pending);
            m_pending.reset();
            startPreview(std::move(next));
        }
        finishShutdownIfReady();
    });
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    const auto cancelled = m_cancelled;
    const auto hook = m_beforePreviewTestHook;
    QThreadPool::globalInstance()->start([promise, task = std::move(task), root = m_rootPath,
                                         cancelled, hook] {
        promise->start();
        PreviewResult result;
        try {
            if (hook) (*hook)();
            if (task.automatic) {
                CoverArtRequest request;
                request.rootPath = root;
                request.libraryWork = true;
                request.forceReload = true;
                result.automatic = CoverArtLoader::load(request, {}, cancelled);
            } else result.draft = ManagedCoverFile::readExternal(task.path, cancelled);
        } catch (const std::exception &) {
            result.draft.errorMessage = QStringLiteral("封面预览失败，请重试。");
            result.automatic.message = result.draft.errorMessage;
        } catch (...) {
            result.draft.errorMessage = QStringLiteral("封面预览失败，请重试。");
            result.automatic.message = result.draft.errorMessage;
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void WorkEditDialog::updateControls()
{
    const bool enabled = !m_shuttingDown && !m_saving;
    m_titleEdit->setEnabled(enabled);
    m_chooseButton->setEnabled(enabled && m_initialized && !m_fileDialog);
    m_autoButton->setEnabled(enabled && m_initialized
        && (title() != m_defaultTitle || m_hasManual || m_coverChange == CoverChange::Replace));
    const bool changed = title() != m_originalTitle || m_coverChange != CoverChange::Keep;
    const bool validCover = m_coverChange != CoverChange::Replace || m_draft.success;
    m_saveButton->setEnabled(enabled && m_initialized && !m_loading && !m_fileDialog
        && changed && validTitle() && validCover);
    m_cancelButton->setEnabled(enabled);
    m_titleBar->setClosing(m_shuttingDown);
    m_titleBar->closeButton()->setEnabled(enabled);
    m_saveButton->setToolTip(!validTitle() ? QStringLiteral("请输入非空的单行作品标题。")
                                         : QString());
}

void WorkEditDialog::requestSave()
{
    if (m_saveButton->isEnabled()) emit saveRequested();
}

bool WorkEditDialog::isIdle() const { return !m_active && !m_pending; }

bool WorkEditDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_windowFrame && m_windowFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}

void WorkEditDialog::closeEvent(QCloseEvent *event)
{
    if (m_saving && !m_shuttingDown) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void WorkEditDialog::done(int result)
{
    if (m_saving && !m_shuttingDown) return;
    m_requestedResult = result;
    hide();
    shutdown();
}

void WorkEditDialog::shutdown()
{
    if (!m_shuttingDown) {
        m_shuttingDown = true;
        ++m_generation;
        m_pending.reset();
        if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
        if (m_fileDialog) m_fileDialog->reject();
        updateControls();
    }
    QMetaObject::invokeMethod(this, &WorkEditDialog::finishShutdownIfReady, Qt::QueuedConnection);
}

void WorkEditDialog::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || !isIdle()) return;
    m_shutdownNotified = true;
    QPointer<WorkEditDialog> guard(this);
    emit shutdownFinished();
    if (guard && m_requestedResult) QDialog::done(*m_requestedResult);
}
} // namespace asmr
