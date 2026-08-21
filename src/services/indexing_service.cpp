#include "services/indexing_service.h"

#include "data/torrent_repository.h"
#include "domain/content_classifier.h"
#include "services/filter_policy.h"

#include <QDebug>
#include <QSet>
#include <QStringList>

namespace rats::service {

IndexingService::IndexingService(data::TorrentRepository* repository, FilterPolicy* filter, QObject* parent)
    : QObject(parent), repository_(repository), filter_(filter)
{
}

IndexingService::Result IndexingService::insert(domain::Torrent torrent)
{
    Result result;

    if (!torrent.isValid()) {
        result.error = QStringLiteral("Invalid hash");
        return result;
    }
    if (torrent.name.isEmpty()) {
        result.error = QStringLiteral("Empty torrent name");
        return result;
    }

    // Already indexed? Merge what the incoming copy adds over the stored one
    // (e.g. from a peer) and return the stored entity.
    if (auto existing = repository_->get(torrent.hash)) {
        result.success = true;
        result.alreadyExists = true;
        result.torrent = *existing;

        // Backfill the file list when the stored copy has none but the incoming
        // one does. This heals a metadata-only row into a complete torrent
        // instead of leaving the two out of sync.
        if (existing->files == 0 && !torrent.fileList.isEmpty()
            && repository_->updateFiles(existing->hash, torrent.fileList)) {
            existing->fileList = torrent.fileList;
            existing->files = torrent.fileList.size();
            result.torrent = *existing;
        }

        if (torrent.good > existing->good || torrent.bad > existing->bad) {
            existing->good = qMax(existing->good, torrent.good);
            existing->bad = qMax(existing->bad, torrent.bad);
            repository_->update(*existing);
            result.torrent = *existing;
        }
        return result;
    }

    if (torrent.contentType == domain::ContentType::Unknown)
        domain::ContentClassifier::classify(torrent);

    if (filter_) {
        if (const QString reason = filter_->rejectionReason(torrent); !reason.isEmpty()) {
            qInfo() << "[Indexing] rejected" << torrent.hash.left(16) << "-" << reason;
            result.error = QStringLiteral("Rejected: ") + reason;
            return result;
        }
    }

    // The get() above already proved the hash is absent (single-threaded insert
    // path), so skip the redundant existence query inside add().
    if (!repository_->add(torrent, /*skipExistsCheck*/ true)) {
        result.error = QStringLiteral("Database insert failed");
        return result;
    }

    result.success = true;
    result.torrent = torrent;
    qInfo() << "[Indexing] indexed" << torrent.hash.left(16) << torrent.name.left(50)
            << "size:" << (torrent.size / (1024 * 1024)) << "MB files:" << torrent.files;

    emit torrentIndexed(torrent);
    return result;
}

IndexingService::BatchResult IndexingService::insertBatch(
    QVector<domain::Torrent> torrents, const BatchOptions& options)
{
    BatchResult result;
    if (torrents.isEmpty())
        return result;

    // 1. Validate and de-duplicate inside the batch itself. A dump can carry the
    //    same hash twice (two exports concatenated); letting both through would
    //    insert one row and then treat the second one as new as well.
    QVector<domain::Torrent> candidates;
    candidates.reserve(torrents.size());
    QSet<QString> seen;
    for (const domain::Torrent& t : torrents) {
        if (!t.isValid() || t.name.isEmpty()) {
            ++result.invalid;
            continue;
        }
        if (seen.contains(t.hash))
            continue;
        seen.insert(t.hash);
        candidates.append(t);
    }
    if (candidates.isEmpty())
        return result;

    // 2. Dedupe against the index with one lookup for the whole batch instead of
    //    a get() per torrent. Like insert(), this happens BEFORE classification
    //    and filtering: an already-stored torrent keeps its row and merges, and
    //    is never re-classified or re-judged by a filter it predates.
    QStringList hashes;
    hashes.reserve(candidates.size());
    for (const domain::Torrent& t : candidates)
        hashes << t.hash;
    const QHash<QString, domain::Torrent> existing = repository_->getMany(hashes);

    QVector<domain::Torrent> fresh;
    fresh.reserve(candidates.size());

    for (domain::Torrent& incoming : candidates) {
        auto it = existing.constFind(incoming.hash);
        if (it == existing.constEnd()) {
            if (incoming.contentType == domain::ContentType::Unknown)
                domain::ContentClassifier::classify(incoming);
            if (options.applyFilters && filter_ && !filter_->accepts(incoming)) {
                ++result.rejected;
                continue;
            }
            fresh.append(incoming);
            continue;
        }

        ++result.merged;
        if (!options.mergeExisting)
            continue;

        // Same merge rules as insert(): heal a metadata-only row with the file
        // list the incoming copy carries, and keep the higher vote counts.
        domain::Torrent stored = *it;
        if (stored.files == 0 && !incoming.fileList.isEmpty())
            repository_->updateFiles(stored.hash, incoming.fileList);

        if (incoming.good > stored.good || incoming.bad > stored.bad) {
            stored.good = qMax(stored.good, incoming.good);
            stored.bad = qMax(stored.bad, incoming.bad);
            repository_->update(stored);
        }
    }

    // 3. Everything genuinely new goes in as one multi-row INSERT.
    if (!fresh.isEmpty())
        result.inserted = repository_->addMany(fresh);

    return result;
}

bool IndexingService::accepts(const domain::Torrent& torrent) const
{
    return !filter_ || filter_->accepts(torrent);
}

} // namespace rats::service
