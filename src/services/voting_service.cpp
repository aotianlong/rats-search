#include "services/voting_service.h"

#include "data/torrent_repository.h"
#include "services/p2p_store.h"

#include <QDebug>

namespace rats::service {

VotingService::VotingService(P2PStore* store, data::TorrentRepository* repository, QObject* parent)
    : QObject(parent), store_(store), repository_(repository)
{
    if (store_) {
        connect(store_, &P2PStore::recordStored, this, &VotingService::onRecordStored);
    }
    qInfo() << "[VotingService] initialized";
}

// ============================================================================
// Public API
// ============================================================================

void VotingService::vote(const QString& hash, bool isGood, ResultCallback callback)
{
    qInfo() << "[VotingService] vote hash:" << hash.left(16) << "isGood:" << isGood;

    if (hash.length() != 40) {
        if (callback)
            callback(false, QJsonObject(), QStringLiteral("Invalid hash"));
        return;
    }

    if (!repository_) {
        if (callback)
            callback(false, QJsonObject(), QStringLiteral("Repository not initialized"));
        return;
    }

    // Without files: nothing on this path needs them. update() below rewrites the
    // torrents row only, and the replicated vote carries no torrent data at all.
    std::optional<domain::Torrent> existing = repository_->get(hash);
    if (!existing) {
        if (callback)
            callback(false, QJsonObject(), QStringLiteral("Torrent not found"));
        return;
    }
    domain::Torrent torrent = *existing;

    const bool storeReady = store_ && store_->isAvailable();

    // Already voted: return the current counts without double counting. The store
    // gives cross-session dedup when available; the in-memory set covers the case
    // where the store is down (otherwise every repeated click would keep bumping
    // the local good/bad columns without bound).
    if (selfVotedHashes_.contains(hash) || (storeReady && hasVoted(hash))) {
        int goodCount = torrent.good;
        int badCount = torrent.bad;
        if (storeReady) {
            const VoteCounts votes = aggregate(hash);
            goodCount = votes.good;
            badCount = votes.bad;
        }
        qInfo() << "[VotingService] already voted on" << hash.left(8) << "good:" << goodCount << "bad:" << badCount;

        QJsonObject result;
        result["hash"] = hash;
        result["good"] = goodCount;
        result["bad"] = badCount;
        result["selfVoted"] = true;
        result["alreadyVoted"] = true;
        if (callback)
            callback(true, result, QString());
        return;
    }

    // Store the vote in the distributed store (this replicates to all peers).
    bool storedInP2P = false;
    if (storeReady) {
        storedInP2P = storeVote(hash, isGood);
    }

    // Mirror the vote onto the local torrent counts for fast local access.
    if (isGood) {
        torrent.good++;
    } else {
        torrent.bad++;
    }
    repository_->update(torrent);
    selfVotedHashes_.insert(hash);

    // Prefer the distributed aggregate for the returned counts.
    int goodCount = torrent.good;
    int badCount = torrent.bad;
    if (storeReady) {
        VoteCounts votes = aggregate(hash);
        goodCount = votes.good;
        badCount = votes.bad;
    }

    emit votesUpdated(hash, goodCount, badCount);

    QJsonObject result;
    result["hash"] = hash;
    result["good"] = goodCount;
    result["bad"] = badCount;
    result["selfVoted"] = true;
    result["distributed"] = storedInP2P;
    if (callback)
        callback(true, result, QString());
}

void VotingService::getVotes(const QString& hash, ResultCallback callback)
{
    if (hash.length() != 40) {
        if (callback)
            callback(false, QJsonObject(), QStringLiteral("Invalid hash"));
        return;
    }

    QJsonObject result;
    result["hash"] = hash;

    if (store_ && store_->isAvailable()) {
        // Aggregates every peer's vote records.
        VoteCounts votes = aggregate(hash);
        result["good"] = votes.good;
        result["bad"] = votes.bad;
        result["selfVoted"] = votes.selfVoted;
        result["source"] = "distributed";
    } else if (repository_) {
        // Fall back to the local torrent's columns.
        std::optional<domain::Torrent> torrent = repository_->get(hash);
        if (torrent) {
            result["good"] = torrent->good;
            result["bad"] = torrent->bad;
            result["selfVoted"] = false; // cannot be determined locally
            result["source"] = "local";
        } else {
            result["good"] = 0;
            result["bad"] = 0;
            result["selfVoted"] = false;
            result["source"] = "none";
        }
    } else {
        result["good"] = 0;
        result["bad"] = 0;
        result["selfVoted"] = false;
        result["source"] = "unavailable";
    }

    if (callback)
        callback(true, result, QString());
}

VotingService::VoteCounts VotingService::aggregate(const QString& hash) const
{
    VoteCounts result;

    if (hash.length() != 40) {
        return result;
    }
    if (!store_ || !store_->isAvailable()) {
        return result;
    }

    const QString peerId = store_->ourPeerId();

    // One record per peer: "vote:{hash}:{peerId}".
    const QString prefix = QStringLiteral("vote:%1:").arg(hash);
    const QList<StoredRecord> records = store_->find(prefix);

    for (const StoredRecord& record : records) {
        const QString vote = record.data.value("vote").toString();
        if (vote == QLatin1String("good")) {
            result.good++;
        } else if (vote == QLatin1String("bad")) {
            result.bad++;
        }

        if (record.peerId == peerId) {
            result.selfVoted = true;
        }
    }

    return result;
}

bool VotingService::hasVoted(const QString& hash) const
{
    if (hash.length() != 40) {
        return false;
    }
    if (!store_ || !store_->isAvailable()) {
        return false;
    }

    const QString key = QStringLiteral("vote:%1:%2").arg(hash, store_->ourPeerId());
    return store_->has(key);
}

// ============================================================================
// Private
// ============================================================================

QJsonObject VotingService::voteRecord(const QString& hash, bool good)
{
    QJsonObject voteData;
    voteData["type"] = "vote";
    voteData["torrentHash"] = hash;
    voteData["vote"] = good ? "good" : "bad";
    voteData["_index"] = QStringLiteral("vote:%1").arg(hash);

    // Deliberately nothing else. A vote used to carry the whole torrent — every
    // file name included — under a "_torrent" key, nominally so a peer that did
    // not have the torrent could pick it up from the vote. Nothing ever read it
    // back: onRecordStored() uses torrentHash alone and returns early when the
    // torrent is unknown locally, which is exactly the case the payload was
    // supposed to serve. What it did do was inflate an entry from ~300 bytes to
    // ~24 KB, and since the store replicates to every peer and is re-sent on
    // every snapshot, that multiplied across the whole network. Anything a peer
    // needs about the torrent itself belongs in torrent replication, not here.
    return voteData;
}

bool VotingService::storeVote(const QString& hash, bool isGood)
{
    if (!store_ || !store_->isAvailable()) {
        qWarning() << "[VotingService] Storage not available for voting";
        return false;
    }

    // Key format: vote:{hash}:{peerId} — one vote per peer per torrent.
    const QString key = QStringLiteral("vote:%1:%2").arg(hash, store_->ourPeerId());

    const bool result = store_->put(key, voteRecord(hash, isGood));
    if (result) {
        qInfo() << "[VotingService] stored" << (isGood ? "good" : "bad") << "vote for" << hash.left(8);
    }
    return result;
}

void VotingService::onRecordStored(const StoredRecord& record, bool isRemote)
{
    // Local votes already emit votesUpdated from vote(); only react to peers'.
    if (!isRemote || record.type != QLatin1String("vote")) {
        return;
    }

    const QString hash = record.data.value("torrentHash").toString();
    if (hash.length() != 40 || !repository_) {
        return;
    }

    // A peer's vote changed the swarm aggregate: mirror it onto the local torrent
    // columns (so offline reads stay correct) and notify the UI/feed.
    const VoteCounts votes = aggregate(hash);
    std::optional<domain::Torrent> torrent = repository_->get(hash);
    if (torrent && (torrent->good != votes.good || torrent->bad != votes.bad)) {
        torrent->good = votes.good;
        torrent->bad = votes.bad;
        repository_->update(*torrent);
    }
    emit votesUpdated(hash, votes.good, votes.bad);
}

} // namespace rats::service
