#ifndef RATS_DATA_TORRENT_REPOSITORY_H
#define RATS_DATA_TORRENT_REPOSITORY_H

#include "domain/torrent.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <optional>

namespace rats::data {

class Database;

// Repository for the `torrents` and `files` Manticore tables. Owns the mapping
// between rows and domain::Torrent, all search queries, and the incrementally
// maintained statistics. Process lifecycle lives in Manticore and raw SQL in
// Database/SelectQuery.
class TorrentRepository : public QObject {
    Q_OBJECT

public:
    struct SearchQuery {
        QString text;
        int offset = 0;
        int limit = 10;
        QString sort; // user sort key ("seeders", "size", "added"); mapped to a
                      // safe column
        bool descending = true;
        bool safeSearch = false;
        QString contentType; // "video".."archive", or "application" (Software+Games)
        qint64 sizeMin = 0;
        qint64 sizeMax = 0;
        int filesMin = 0;
        int filesMax = 0;
    };

    struct Statistics {
        qint64 torrents = 0;
        qint64 files = 0;
        qint64 totalSize = 0;
    };

    explicit TorrentRepository(Database* db, QObject* parent = nullptr);

    // Load id counters and statistics from the existing tables. Call once after
    // the database is up.
    void primeFromDatabase();

    // CRUD ---------------------------------------------------------------------
    // Pass skipExistsCheck when the caller has just proven the hash is absent
    // (e.g. IndexingService's dedup get()), to avoid a redundant existence query.
    bool add(const domain::Torrent& torrent, bool skipExistsCheck = false);
    bool update(const domain::Torrent& torrent);
    bool remove(const QString& hash);
    bool exists(const QString& hash);
    std::optional<domain::Torrent> get(const QString& hash, bool includeFiles = false);

    // Bulk variants used by the dump importer, where a per-row round trip would
    // dominate the runtime. `hashes` must not exceed Manticore's max_matches
    // (1000) — callers batch.
    QSet<QString> existingHashes(const QStringList& hashes);
    QHash<QString, domain::Torrent> getMany(const QStringList& hashes);
    // Insert torrents already known to be absent, in as few statements as the
    // packet limit allows. Statistics move once for the whole batch instead of
    // once per row, so a million-row import does not emit a million signals.
    // Returns the number of rows written.
    int addMany(const QVector<domain::Torrent>& torrents);

    // Search -------------------------------------------------------------------
    QVector<domain::SearchHit> searchTorrents(const SearchQuery& query);
    QVector<domain::SearchHit> searchFiles(const SearchQuery& query);
    QVector<domain::Torrent> recent(int limit = 10);
    QVector<domain::Torrent> top(const QString& type, const QString& time, int offset, int limit);
    QVector<domain::Torrent> random(int limit = 5, bool includeFiles = false);
    // A page of all torrents ordered by id — used by maintenance sweeps.
    // Keyset ("seek") pagination: pass 0 to start, then the id of the last row
    // of the previous page. OFFSET paging cannot be used for a full sweep —
    // Manticore refuses any offset >= max_matches (1000 by default) with
    // "offset out of bounds", and rows deleted mid-sweep shift the remaining
    // ones into the offsets already consumed.
    QVector<domain::Torrent> pageAfterId(qint64 afterId, int limit);

    // Files --------------------------------------------------------------------
    QVector<domain::File> filesOf(const QString& hash);
    QHash<QString, QVector<domain::File>> filesOf(const QStringList& hashes);

    // Partial updates ----------------------------------------------------------
    bool updateTrackerCounts(const QString& hash, int seeders, int leechers, int completed);
    // Replace the file list of an already-stored torrent and sync its file count
    // and statistics. Used to backfill a copy that was indexed from metadata
    // before its file list was available. No-op (returns false) on empty input.
    bool updateFiles(const QString& hash, const QVector<domain::File>& files);
    bool mergeInfo(const QString& hash, const QJsonObject& info);
    bool updateClassification(const QString& hash, domain::ContentType type, domain::ContentCategory category);

    Statistics statistics() const { return stats_; }

signals:
    void torrentUpdated(const QString& hash);
    void statisticsChanged(qint64 torrents, qint64 files, qint64 totalSize);

private:
    domain::Torrent rowToTorrent(const QVariantMap& row) const;
    QString buildNameIndex(const domain::Torrent& torrent) const;
    void saveFiles(const QString& hash, const QVector<domain::File>& files);
    // Column map for one torrents row, id included. Shared by add() and addMany().
    QVariantMap torrentRow(const domain::Torrent& torrent);
    // Column map for one files row (the "\n"-joined path/size blobs).
    QVariantMap filesRow(const QString& hash, const QVector<domain::File>& files);
    QVector<domain::Torrent> selectTorrents(const QString& sql, const QVariantList& params = {});

    // Map a user-supplied sort key to a whitelisted column, or empty if unknown.
    static QString resolveSortColumn(const QString& key);
    // Build the "contentType = N" / "contentType IN (5,6)" fragment for a filter.
    static QString contentTypeFilter(const QString& type);

    Database* db_;
    std::atomic<qint64> nextTorrentId_ { 1 };
    std::atomic<qint64> nextFilesId_ { 1 };
    Statistics stats_;
};

} // namespace rats::data

#endif // RATS_DATA_TORRENT_REPOSITORY_H
