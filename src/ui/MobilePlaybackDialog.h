#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QShowEvent;

namespace asmr {

class MobilePlaybackService;
class DialogFrame;

class MobilePlaybackDialog final : public QDialog {
public:
    explicit MobilePlaybackDialog(MobilePlaybackService *service, QWidget *parent = nullptr);
    void refreshInterfaces();

protected:
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;

private:
    void updateState();
    void fitContentHeight();
    DialogFrame *m_frame = nullptr;
    MobilePlaybackService *m_service;
    QComboBox *m_interface;
    QLabel *m_networkHint;
    QLabel *m_status;
    QLabel *m_address;
    QLabel *m_code;
    QWidget *m_pairingPanel;
    QLabel *m_qr;
    QLabel *m_scanHint;
    QString m_qrLink;
    QPushButton *m_toggle;
    QPushButton *m_copy;
    QPushButton *m_renew;
};

} // namespace asmr
