#ifndef RATS_SERVICE_DATABASE_IMPORTER_H
#define RATS_SERVICE_DATABASE_IMPORTER_H

#include "services/database_worker.h"

#include <QCoreApplication>
#include <QString>
#include <functional>

namespace rats::service {

class IndexingService;

// Merges a .ratsdb dump into the local index.
//
// A merge, never a replacement: every torrent goes through IndexingService, the
// single write path, so a dump behaves exactly like a very fast peer — new
// torrents are indexed, known ones keep the local row and only gain what the
// incoming copy adds.
//
// Like DatabaseExporter this is a plain blocking call with plain callbacks, so it
// can be driven from a test without an event loop.
class DatabaseImporter {
    Q_DECLARE_TR_FUNCTIONS(DatabaseImporter)

public:
    struct Options {
        // Run the local filter policy over the incoming torrents. On by default: a
        // foreign index must not smuggle content past the user's own filters.
        bool applyFilters = true;
        // Byte offset to resume from; 0 starts at the first frame.
        qint64 startOffset = 0;
    };

    struct Result {
        bool ok = false;
        bool cancelled = false;
        // The dump ended without its footer: the producer died or the transfer was
        // cut. Everything read before that point was still merged.
        bool truncated = false;
        qint64 processed = 0;
        qint64 inserted = 0;
        qint64 merged = 0;
        qint64 rejected = 0;
        qint64 collided = 0;
        qint64 offset = 0; // where reading stopped, for a later resume
        QString error;
    };

    // Called after each committed frame. `offset` is safe to persist as a resume
    // point: it is only reported once the batch before it is in the index, so a
    // resume can repeat work but never skip it.
    using ProgressFn = std::function<void(const Result& progress)>;

    explicit DatabaseImporter(IndexingService* indexing);

    Result run(const QString& path, const Options& options, const CancelToken& cancel, const ProgressFn& onProgress);

private:
    IndexingService* indexing_;
};

} // namespace rats::service

#endif // RATS_SERVICE_DATABASE_IMPORTER_H
