#pragma once

#include "ui/FloatingSubtitleSettings.h"

#include <QDialog>

class QComboBox;
class QFontComboBox;
class QLabel;
class QSlider;

namespace asmr {

class DialogFrame;

// Edits appearance values only; MainWindow applies them and owns persistence.
class FloatingSubtitleAppearanceDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit FloatingSubtitleAppearanceDialog(QWidget *parent = nullptr);

    void setAppearance(const FloatingSubtitleAppearance &appearance);
    [[nodiscard]] FloatingSubtitleAppearance appearance() const;

signals:
    void appearanceChangeRequested(const FloatingSubtitleAppearance &appearance);

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void requestCurrentAppearance();

    QFontComboBox *m_fontFamily = nullptr;
    QComboBox *m_fontPointSize = nullptr;
    QComboBox *m_fontColor = nullptr;
    QSlider *m_backgroundOpacity = nullptr;
    QLabel *m_backgroundOpacityLabel = nullptr;
    DialogFrame *m_dialogFrame = nullptr;
};

} // namespace asmr
