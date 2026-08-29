#ifndef MIGRATIONPROGRESSWINDOW_H
#define MIGRATIONPROGRESSWINDOW_H

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QProgressBar;

namespace rats::service {
class MigrationService;
} // namespace rats::service

/**
 * @brief Splash window for the blocking pre-start data migrations.
 *
 * Those migrations run on the main thread inside Application::start() — before
 * MainWindow exists and before the event loop is entered. On a large index the
 * v2.2.8 re-keying sweep takes minutes, during which the process would show
 * nothing at all and read as hung. This window closes that gap: it appears when
 * the first sync migration starts, and hides when they finish.
 *
 * Because the migration owns the thread, nothing pumps the event queue for us:
 * every progress tick calls processEvents() itself so the window keeps painting
 * from inside the blocking call. That pump is throttled — a repaint per 500-row
 * page would cost more than the migration.
 *
 * Construct it between building Application and calling start(); if no
 * migration is pending it never shows itself.
 */
class MigrationProgressWindow : public QWidget {
    Q_OBJECT

public:
    MigrationProgressWindow(rats::service::MigrationService* migrations, bool darkMode, QWidget* parent = nullptr);

private:
    void setupUi();
    void applyTheme(bool darkMode);
    void onMigrationStarted(const QString& migrationId, const QString& description);
    void onProgress(const QString& migrationId, qint64 current, qint64 total);
    void onFinished();
    // Repaint and drain the queue so the window stays responsive inside the
    // blocking migration. User input is excluded: the window is a status
    // display, and a click delivered mid-migration has nothing safe to do.
    void pumpEvents(bool force = false);

    QLabel* headingLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QElapsedTimer repaintClock_;
};

#endif // MIGRATIONPROGRESSWINDOW_H
