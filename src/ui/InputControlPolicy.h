#pragma once

#include <QObject>

namespace asmr {

// Registers the small PNG indicators used by the shared stylesheet, including
// isolated GUI programs that link the static window-frame library.
void initializeInputControlResources();

// Wheel input scrolls the surrounding page without changing a closed selector
// or numeric editor. Mouse buttons, direct input and keyboard editing stay Qt's.
class InputControlPolicy final : public QObject {
    Q_OBJECT
public:
    static void install(); // Idempotent; the QApplication owns the policy.
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    explicit InputControlPolicy(QObject *parent);
    bool m_forwarding = false;
};

} // namespace asmr
