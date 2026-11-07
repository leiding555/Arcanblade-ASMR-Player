#pragma once

#include <QImage>
#include <QLabel>

namespace asmr {

// A passive projection of already decoded artwork. It performs no file access.
class CoverArtWidget final : public QLabel
{
    Q_OBJECT

public:
    explicit CoverArtWidget(QWidget *parent = nullptr);

    void setImage(const QImage &image, const QString &sourcePath = {}, const QString &detail = {});
    void clearImage(const QString &message = QStringLiteral("暂无封面"));
    // Reuse the passive view for browsed artwork without describing it as playback.
    void setContextName(const QString &name);
    [[nodiscard]] bool hasImage() const { return !m_image.isNull(); }
    [[nodiscard]] const QImage &image() const { return m_image; }
    [[nodiscard]] const QString &sourcePath() const { return m_sourcePath; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    QString m_sourcePath;
    QString m_contextName = QStringLiteral("当前播放封面");
    QString m_detail;
    QString m_placeholderMessage;
};

} // namespace asmr
