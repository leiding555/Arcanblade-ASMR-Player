#include "ui/MobilePlaybackDialog.h"

#include "mobile/MobilePlaybackService.h"
#include "ui/DialogFrame.h"
#include "ui/TitleBarWidget.h"

#include <qrcodegen.hpp>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkInterface>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace asmr {
namespace {

constexpr int QrSide = 160;

QPixmap pairingQr(const QString &link, qreal devicePixelRatio)
{
    const auto qr = qrcodegen::QrCode::encodeText(link.toLatin1().constData(),
                                                  qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int quietZone = 4;
    const int side = qr.getSize() + quietZone * 2;
    const int scale = std::max(1, int(QrSide * devicePixelRatio) / side);
    QPixmap pixmap(side * scale, side * scale);
    pixmap.fill(Qt::white);
    QPainter painter(&pixmap);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y))
                painter.fillRect((x + quietZone) * scale, (y + quietZone) * scale, scale, scale, Qt::black);
        }
    }
    painter.end();
    pixmap.setDevicePixelRatio(devicePixelRatio);
    return pixmap;
}

int interfacePriority(QNetworkInterface::InterfaceType type, const QHostAddress &address)
{
    // A tunnel can also report Ethernet. Put the benchmarking subnet and
    // self-assigned addresses after ordinary LAN addresses.
    if (address.isInSubnet(QHostAddress(QStringLiteral("198.18.0.0")), 15)
        || address.isLinkLocal() || type == QNetworkInterface::Virtual)
        return 3;
    if (type == QNetworkInterface::Wifi) return 0;
    if (type == QNetworkInterface::Ethernet) return 1;
    return 2;
}

} // namespace

MobilePlaybackDialog::MobilePlaybackDialog(MobilePlaybackService *service, QWidget *parent)
    : QDialog(parent), m_service(service), m_interface(new QComboBox(this)),
      m_networkHint(new QLabel(this)), m_status(new QLabel(this)),
      m_address(new QLabel(this)), m_code(new QLabel(this)), m_pairingPanel(new QWidget(this)),
      m_qr(new QLabel(m_pairingPanel)), m_scanHint(new QLabel(QStringLiteral("用手机相机扫码"), m_pairingPanel)),
      m_toggle(new QPushButton(this)), m_copy(new QPushButton(QStringLiteral("复制"), this)),
      m_renew(new QPushButton(QStringLiteral("换新码"), this))
{
    setWindowTitle(QStringLiteral("手机播放"));
    setObjectName(QStringLiteral("mobilePlaybackDialog"));
    setMinimumWidth(520);
    m_interface->setObjectName(QStringLiteral("mobilePlaybackInterface"));
    m_interface->setMinimumContentsLength(24);
    m_interface->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_toggle->setObjectName(QStringLiteral("mobilePlaybackToggle"));
    m_copy->setObjectName(QStringLiteral("mobilePlaybackCopyAddress"));
    m_renew->setObjectName(QStringLiteral("mobilePlaybackRenew"));
    m_copy->setAccessibleName(QStringLiteral("复制浏览器地址"));
    m_renew->setToolTip(QStringLiteral("更新二维码和配对码。重新配对成功后会替换原连接。"));
    m_address->setObjectName(QStringLiteral("mobilePlaybackAddress"));
    m_code->setObjectName(QStringLiteral("mobilePlaybackPairingCode"));
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_code->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_qr->setObjectName(QStringLiteral("mobilePlaybackQr"));
    m_qr->setAlignment(Qt::AlignCenter);
    m_qr->setWordWrap(true);
    m_qr->setFixedSize(QrSide, QrSide);
    m_scanHint->setObjectName(QStringLiteral("mobilePlaybackScanHint"));
    m_scanHint->setAlignment(Qt::AlignCenter);
    m_scanHint->setProperty("uiRole", QStringLiteral("muted"));
    auto scanHintPolicy = m_scanHint->sizePolicy();
    scanHintPolicy.setRetainSizeWhenHidden(true);
    m_scanHint->setSizePolicy(scanHintPolicy);
    m_pairingPanel->hide();
    m_networkHint->setObjectName(QStringLiteral("mobilePlaybackNetworkHint"));
    m_networkHint->setProperty("uiRole", QStringLiteral("muted"));
    m_networkHint->setWordWrap(true);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout;
    layout->setSpacing(10);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("网络接口"), m_interface);
    layout->addLayout(form);
    layout->addWidget(m_networkHint);

    auto *pairingLayout = new QHBoxLayout(m_pairingPanel);
    pairingLayout->setContentsMargins(0, 0, 0, 0);
    pairingLayout->setSpacing(16);
    auto *scan = new QVBoxLayout;
    scan->addWidget(m_qr, 0, Qt::AlignHCenter);
    scan->addWidget(m_scanHint);
    pairingLayout->addLayout(scan);

    auto *manual = new QGridLayout;
    manual->setColumnStretch(1, 1);
    manual->setRowStretch(5, 1);
    auto *manualHeading = new QLabel(QStringLiteral("手动配对"), this);
    manualHeading->setProperty("uiRole", QStringLiteral("heading"));
    manual->addWidget(manualHeading, 0, 0, 1, 3);
    manual->addWidget(new QLabel(QStringLiteral("浏览器地址"), this), 1, 0, 1, 3);
    manual->addWidget(m_address, 2, 0, 1, 2);
    manual->addWidget(m_copy, 2, 2, Qt::AlignRight);
    manual->addWidget(new QLabel(QStringLiteral("配对码"), this), 3, 0);
    manual->addWidget(m_code, 3, 1);
    manual->addWidget(m_renew, 3, 2, Qt::AlignRight);
    auto *validity = new QLabel(QStringLiteral("5 分钟内有效，仅可使用一次"), this);
    validity->setProperty("uiRole", QStringLiteral("muted"));
    manual->addWidget(validity, 4, 0, 1, 3);
    pairingLayout->addLayout(manual, 1);
    layout->addWidget(m_pairingPanel);

    layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_toggle);
    buttons->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName(QStringLiteral("mobilePlaybackClose"));
    buttons->addWidget(close);
    layout->addLayout(buttons);
    m_frame = new DialogFrame(this, layout, false);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_interface, &QComboBox::currentTextChanged, m_interface, &QWidget::setToolTip);
    connect(m_service, &MobilePlaybackService::changed, this, &MobilePlaybackDialog::updateState);
    connect(m_renew, &QPushButton::clicked, m_service, &MobilePlaybackService::renewPairingCode);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_service->address());
    });
    connect(m_toggle, &QPushButton::clicked, this, [this] {
        if (m_service->isIdle()) {
            const auto endpoint = m_interface->currentData().toString();
            const auto ip = endpoint.section(u'/', 0, 0);
            const auto prefix = endpoint.section(u'/', 1, 1).toInt();
            m_service->start(QHostAddress(ip), prefix);
        } else {
            m_service->stop();
        }
    });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible() && m_service->state() == MobilePlaybackService::State::Listening)
            updateState();
    });
    timer->start(1000);
    refreshInterfaces();
}

void MobilePlaybackDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    updateState();
    fitContentHeight();
}

bool MobilePlaybackDialog::nativeEvent(const QByteArray &type, void *message, qintptr *result)
{
    return m_frame && m_frame->handleNativeEvent(type, message, result)
        ? true : QDialog::nativeEvent(type, message, result);
}

void MobilePlaybackDialog::fitContentHeight()
{
    if (!m_frame || !isVisible()) return;
    auto *body = m_frame->contentWidget();
    auto *bodyLayout = body->layout();
    const auto margins = layout()->contentsMargins();
    const int bodyWidth = width() - margins.left() - margins.right();
    const int bodyHeight = std::max(bodyLayout->totalMinimumSize().height(),
                                    bodyLayout->totalHeightForWidth(bodyWidth));
    const int decoration = m_frame->titleBar()->height() + margins.top() + margins.bottom();
    const int available = screen() ? screen()->availableGeometry().height() - 32 : height();
    const int targetHeight = std::min(bodyHeight + decoration, std::max(1, available));
    body->setMinimumHeight(bodyHeight);
    setMinimumHeight(targetHeight);
    resize(width(), targetHeight);
}

void MobilePlaybackDialog::refreshInterfaces()
{
    if (!m_service->isIdle()) return;
    const auto previous = m_interface->currentData();
    m_interface->clear();
    struct InterfaceEntry {
        int priority;
        QString label;
        QString endpoint;
    };
    QList<InterfaceEntry> entries;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.ip().isLoopback()
                || entry.ip() == QHostAddress::AnyIPv4 || entry.ip().isMulticast()
                || entry.prefixLength() <= 0)
                continue;
            const auto address = entry.ip().toString();
            entries.append({interfacePriority(interface.type(), entry.ip()),
                QStringLiteral("%1 (%2)").arg(interface.humanReadableName(), address),
                QStringLiteral("%1/%2").arg(address).arg(entry.prefixLength())});
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        return left.priority < right.priority;
    });
    for (const auto &entry : entries) {
        m_interface->addItem(entry.label, entry.endpoint);
        m_interface->setItemData(m_interface->count() - 1, entry.label, Qt::ToolTipRole);
    }
    const int index = m_interface->findData(previous);
    if (index >= 0) m_interface->setCurrentIndex(index);
    updateState();
}

void MobilePlaybackDialog::updateState()
{
    using State = MobilePlaybackService::State;
    const auto state = m_service->state();
    const bool listening = state == State::Listening;
    const auto address = listening ? m_service->address() : QString{};
    m_interface->setEnabled(state == State::Stopped);
    m_toggle->setText(state == State::Stopped ? QStringLiteral("开启共享") : QStringLiteral("停止共享"));
    m_toggle->setEnabled(state == State::Starting || listening
        || (state == State::Stopped && m_interface->count() > 0));
    m_copy->setEnabled(listening);
    m_renew->setEnabled(listening);
    const auto pairing = m_service->pairingCodeInfo();
    const bool panelChanged = m_pairingPanel->isHidden() == listening;
    m_pairingPanel->setVisible(listening);
    m_networkHint->setText(state == State::Stopped
        ? QStringLiteral("请连接同一可信局域网。若 Windows 防火墙询问，请仅为可信专用网络放行。")
        : QStringLiteral("请让电脑和手机连接同一可信局域网。"));
    m_address->setText(address);
    m_address->setToolTip(address);
    using PairingCodeState = MobilePlaybackService::PairingCodeState;
    const auto unavailableText = pairing.state == PairingCodeState::Used ? QStringLiteral("已使用")
        : pairing.state == PairingCodeState::Expired ? QStringLiteral("已过期") : QString{};
    m_code->setText(pairing.code.isEmpty() ? unavailableText : pairing.code);
    m_scanHint->setVisible(pairing.state == PairingCodeState::Ready);
    QString qrLink;
    if (pairing.state == PairingCodeState::Ready) {
        QUrl url(address);
        url.setFragment(QStringLiteral("pair=") + pairing.code);
        qrLink = url.toString(QUrl::FullyEncoded);
    }
    const auto ratio = m_qr->devicePixelRatioF();
    if (qrLink != m_qrLink || (!qrLink.isEmpty() && m_qr->pixmap().devicePixelRatio() != ratio)) {
        m_qrLink = qrLink;
        if (qrLink.isEmpty()) {
            m_qr->setText(unavailableText.isEmpty() ? QString{} : QStringLiteral("配对码") + unavailableText);
        } else {
            m_qr->setPixmap(pairingQr(qrLink, ratio));
        }
    }
    m_qr->setAccessibleName(qrLink.isEmpty() ? m_qr->text() : QStringLiteral("手机配对二维码"));
    QString status = m_service->error();
    if (status.isEmpty()) {
        switch (state) {
        case State::Listening: break;
        case State::Starting:
            status = QStringLiteral("正在开启…");
            break;
        case State::Stopping:
            status = QStringLiteral("正在停止…");
            break;
        case State::Stopped:
            status = m_interface->count() ? QStringLiteral("共享已关闭。")
                : QStringLiteral("未发现可用的局域网地址，连接网络后重新打开此窗口。");
            break;
        }
    }
    const bool statusChanged = m_status->text() != status;
    if (statusChanged) m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    if (panelChanged || statusChanged) fitContentHeight();
}

} // namespace asmr
