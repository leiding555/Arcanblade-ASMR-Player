#pragma once

#include "library/WorkDirectoryDiscovery.h"

#include <QDialog>
#include <QStringList>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableView;

namespace asmr {

class BatchImportCandidatesModel;
class DialogFrame;

// Presentation only: discovery and all imports belong to the owner. Accepting
// returns the explicitly checked directories; rejecting performs no operation.
class BatchImportPreviewDialog final : public QDialog
{
    Q_OBJECT

public:
    enum CandidateDataRole {
        DirectoryRole = Qt::UserRole + 1,
        ExistingWorkIdRole,
        HasOverlapRole,
    };

    explicit BatchImportPreviewDialog(QWidget *parent = nullptr);
    void setDiscovery(const WorkDirectoryDiscoveryResult &discovery);
    [[nodiscard]] QStringList selectedDirectories() const;

public slots:
    void accept() override;

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void updateActions();

    BatchImportCandidatesModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_rootLabel = nullptr;
    QLabel *m_warningLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_feedbackLabel = nullptr;
    QPlainTextEdit *m_diagnostics = nullptr;
    QPushButton *m_detailsButton = nullptr;
    QPushButton *m_confirmButton = nullptr;
    bool m_ready = false;
    DialogFrame *m_dialogFrame = nullptr;
};

} // namespace asmr
