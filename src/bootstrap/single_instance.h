#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLockFile;

namespace rats::bootstrap {

/**
 * @brief Guards against a second instance running on the same data directory.
 *
 * Two processes sharing one data directory would fight over the same Manticore
 * instance, the same RT tables and the same rats.json, so exactly one is
 * allowed to live at a time. The guard is keyed by data directory (not by
 * machine): launching a second copy with its own --data-dir stays legal, which
 * is what development and multi-profile setups need.
 *
 * Two mechanisms, one per job:
 *  - a QLockFile in the data directory holds the exclusive claim and survives
 *    crashes (Qt drops a lock whose recorded pid is gone);
 *  - a QLocalServer keyed by the same directory lets the losing process tell
 *    the winner "the user just tried to start me again", so the GUI can raise
 *    its window instead of silently doing nothing.
 */
class SingleInstanceGuard : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceGuard(const QString& dataDirectory, QObject* parent = nullptr);
    ~SingleInstanceGuard() override;

    /**
     * @brief Claim the data directory for this process.
     * @return true if this process is the only (primary) instance. On false the
     *         caller must exit — another instance already owns the directory.
     */
    bool tryAcquire();

    bool isPrimary() const { return primary_; }

    /**
     * @brief Describe the process holding the lock ("pid 1234, RatsSearch").
     *        Empty when the holder could not be identified.
     */
    QString runningInstanceInfo() const;

    /**
     * @brief Ask the already-running instance to come to the foreground.
     *        Called by the losing process right before it exits.
     * @return true if the ping was delivered.
     */
    bool notifyRunningInstance(int timeoutMs = 1000);

signals:
    /// Emitted in the primary instance when another launch was attempted.
    void secondInstanceStarted();

private:
    void startActivationServer();

    QString lockFilePath_;
    QString serverName_;
    std::unique_ptr<QLockFile> lockFile_;
    QLocalServer* server_ = nullptr;
    bool primary_ = false;
};

} // namespace rats::bootstrap
