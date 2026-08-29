#ifndef RATS_SERVICE_DATABASE_EXPORTER_H
#define RATS_SERVICE_DATABASE_EXPORTER_H

#include "services/database_dump.h"
#include "services/database_worker.h"

#include <QCoreApplication>
#include <QString>
#include <functional>

namespace rats::data {
class TorrentRepository;
}

namespace rats::service {

// Writes the whole torrent index out as a .ratsdb dump.
//
// Deliberately free of Qt signals, peers and service state: it is a blocking
// function that a caller runs on whatever thread it likes, reports through plain
// callbacks, and stops when its CancelToken is raised. That is what makes it
// testable without an event loop, and what keeps the orchestration in
// DatabaseSyncService from leaking into the part that touches the database.
//
// Internally it runs the database reads on a second thread, because the two
// halves of the job are comparable in cost and strictly serial otherwise: pulling
// a page out of Manticore is roughly as expensive as turning it into compressed
// JSON, so overlapping them nearly halves the wall-clock time on a large index.
class DatabaseExporter {
    Q_DECLARE_TR_FUNCTIONS(DatabaseExporter)

public:
    struct Result {
        bool ok = false;
        qint64 torrents = 0;
        qint64 bytes = 0;
        QString error;
    };

    // Called on the exporter's own thread, roughly once per page. `bytes` is the
    // size of the dump so far.
    using ProgressFn = std::function<void(qint64 torrents, qint64 bytes)>;

    explicit DatabaseExporter(data::TorrentRepository* repository);

    // Write every torrent to `path`. A cancelled run removes the partial file and
    // returns ok == false with an empty error.
    Result run(
        const QString& path, const dump::Header& header, const CancelToken& cancel, const ProgressFn& onProgress);

private:
    data::TorrentRepository* repository_;
};

} // namespace rats::service

#endif // RATS_SERVICE_DATABASE_EXPORTER_H
