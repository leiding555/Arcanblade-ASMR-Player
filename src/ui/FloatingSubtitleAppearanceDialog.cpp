#include "ui/FloatingSubtitleAppearanceDialog.h"
#include "ui/DialogFrame.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>

namespace asmr {

FloatingSubtitleAppearanceDialog::FloatingSubtitleAppearanceDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("floatingAppearanceDialog"));
    setWindowTitle(QStringLiteral("悬浮字幕外观"));
    m_fontFamily = new QFontComboBox(this);
    m_fontFamily->setObjectName(QStringLiteral("floatingFontFamily"));
    m_fontPointSize = new QComboBox(this);
    m_fontPointSize->setObjectName(QStringLiteral("floatingFontPointSize"));
    m_fontPointSize->setEditable(false);
    for (int size = MinimumFloatingFontPointSize; size <= MaximumFloatingFontPointSize; ++size)
        m_fontPointSize->addItem(QStringLiteral("%1 pt").arg(size), size);
    m_fontColor = new QComboBox(this);
    m_fontColor->setObjectName(QStringLiteral("floatingFontColor"));
    m_fontColor->setEditable(false);
    const QStringList colorNames{QStringLiteral("白色"), QStringLiteral("金黄"),
        QStringLiteral("天蓝"), QStringLiteral("薄荷绿"), QStringLiteral("浅粉")};
    const auto colors = floatingSubtitleFontColors();
    for (qsizetype index = 0; index < colors.size(); ++index) {
        QPixmap swatch(18, 18);
        swatch.fill(colors[index]);
        m_fontColor->addItem(QIcon(swatch), colorNames[index], colors[index]);
    }
    m_backgroundOpacity = new QSlider(Qt::Horizontal, this);
    m_backgroundOpacity->setObjectName(QStringLiteral("floatingBackgroundOpacity"));
    m_backgroundOpacity->setRange(0, 100);
    m_backgroundOpacityLabel = new QLabel(this);
    m_backgroundOpacityLabel->setObjectName(QStringLiteral("floatingBackgroundOpacityLabel"));
    m_backgroundOpacityLabel->setTextFormat(Qt::PlainText);
    auto *opacityRow = new QHBoxLayout;
    opacityRow->addWidget(m_backgroundOpacity);
    opacityRow->addWidget(m_backgroundOpacityLabel);
    auto *defaults = new QPushButton(QStringLiteral("恢复默认"), this);
    defaults->setObjectName(QStringLiteral("restoreFloatingAppearanceDefaults"));
    auto *closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeButtons->setObjectName(QStringLiteral("floatingAppearanceCloseButtons"));
    closeButtons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    closeButtons->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    closeButtons->layout()->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("字体"), m_fontFamily);
    form->addRow(QStringLiteral("字号"), m_fontPointSize);
    form->addRow(QStringLiteral("字体颜色"), m_fontColor);
    form->addRow(QStringLiteral("背景不透明度"), opacityRow);
    auto *layout = new QVBoxLayout;
    layout->addLayout(form);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    actions->addWidget(defaults);
    actions->addWidget(closeButtons);
    layout->addLayout(actions);
    m_dialogFrame = new DialogFrame(this, layout);
    setAppearance(defaultFloatingSubtitleAppearance());

    connect(m_fontFamily, &QFontComboBox::currentFontChanged, this,
            [this]() { requestCurrentAppearance(); });
    connect(m_fontPointSize, &QComboBox::currentIndexChanged, this,
            [this]() { requestCurrentAppearance(); });
    connect(m_fontColor, &QComboBox::currentIndexChanged, this,
            [this]() { requestCurrentAppearance(); });
    connect(m_backgroundOpacity, &QSlider::valueChanged, this,
            [this]() { requestCurrentAppearance(); });
    connect(defaults, &QPushButton::clicked, this, [this]() {
        setAppearance(defaultFloatingSubtitleAppearance());
        emit appearanceChangeRequested(appearance());
    });
    // Keep the existing immediate-apply semantics: Close hides without rollback.
    connect(closeButtons, &QDialogButtonBox::rejected, this, &QDialog::hide);
}

bool FloatingSubtitleAppearanceDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}

void FloatingSubtitleAppearanceDialog::setAppearance(
    const FloatingSubtitleAppearance &appearance)
{
    const auto valid = validatedFloatingSubtitleAppearance(appearance);
    const QSignalBlocker familyBlocker(m_fontFamily);
    const QSignalBlocker sizeBlocker(m_fontPointSize);
    const QSignalBlocker colorBlocker(m_fontColor);
    const QSignalBlocker opacityBlocker(m_backgroundOpacity);
    m_fontFamily->setCurrentFont(QFont(valid.fontFamily));
    m_fontPointSize->setCurrentIndex(m_fontPointSize->findData(valid.fontPointSize));
    m_fontColor->setCurrentIndex(m_fontColor->findData(valid.fontColor));
    m_backgroundOpacity->setValue(valid.backgroundOpacityPercent);
    m_backgroundOpacityLabel->setText(QStringLiteral("%1%").arg(valid.backgroundOpacityPercent));
}

FloatingSubtitleAppearance FloatingSubtitleAppearanceDialog::appearance() const
{
    return validatedFloatingSubtitleAppearance({m_fontFamily->currentFont().family(),
        m_fontPointSize->currentData().toInt(), m_backgroundOpacity->value(),
        m_fontColor->currentData().value<QColor>()});
}

void FloatingSubtitleAppearanceDialog::requestCurrentAppearance()
{
    m_backgroundOpacityLabel->setText(
        QStringLiteral("%1%").arg(m_backgroundOpacity->value()));
    emit appearanceChangeRequested(appearance());
}

} // namespace asmr
