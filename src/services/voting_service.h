#ifndef RATS_SERVICE_VOTING_SERVICE_H
#define RATS_SERVICE_VOTING_SERVICE_H

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

namespace rats::data {
class TorrentRepository;
}

namespace rats::service {

class P2PStore;
struct StoredRecord;

// Torrent up/down voting over the distributed store. Each vote is a record keyed
// "vote:{hash}:{peerId}" (one per peer per torrent) so aggregation across all
// peers' records yields swarm-wide good/bad counts. Ported from the old
// RatsAPI::vote / getVotes + P2PStoreManager voting helpers: the record format
// and key layout are preserved so already-replicated votes keep aggregating.
//
// All librats storage access is delegated to P2PStore; this service only speaks
// P2PStore + TorrentRepository. The aggregated counts are also mirrored onto the
// torrent's good/bad columns for fast local reads.
class VotingService : public QObject {
    Q_OBJECT

public:
    // Aggregated result of counting all vote records for a hash.
    struct VoteCounts {
        int good = 0;
        int bad = 0;
        bool selfVoted = false;
    };

    // Result callback, mirroring the API response shape: (ok, data, error).
    using ResultCallback = std::function<void(bool ok, const QJsonObject& result, const QString& error)>;

    VotingService(P2PStore* store, data::TorrentRepository* repository, QObject* parent = nullptr);

    // Cast a vote for `hash`. Idempotent per peer: a second call returns the
    // current counts with alreadyVoted=true instead of double counting.
    void vote(const QString& hash, bool good, ResultCallback callback = {});

    // Fetch aggregated votes for `hash`. Uses the distributed store when
    // available, otherwise falls back to the local torrent's good/bad columns.
    void getVotes(const QString& hash, ResultCallback callback);

    // Whether we have already stored a vote for `hash`.
    bool hasVoted(const QString& hash) const;

    /// The exact record a vote is replicated as.
    ///
    /// Public and free of any dependency so it can be pinned by a test, because
    /// what goes in here is not a local matter: the store replicates to every
    /// peer and re-sends its contents on every snapshot exchange, so a field
    /// added here is paid for by the whole network for as long as the vote
    /// exists. It once carried the entire torrent, file list and all, under a
    /// "_torrent" key that nothing ever read — 24 KB per vote instead of 300
    /// bytes. Keep this minimal, and keep the test that says so.
    static QJsonObject voteRecord(const QString& hash, bool good);

signals:
    // The aggregated counts for `hash` changed (after a local or remote vote).
    void votesUpdated(const QString& hash, int good, int bad);

private:
    // Count all vote records for `hash` across peers (from the distributed store).
    VoteCounts aggregate(const QString& hash) const;

    /// Write this node's vote into the replicated store. The entry carries the
    /// vote and nothing else: it is replicated to every peer and re-sent on every
    /// snapshot, so anything in it is paid for network-wide, forever.
    bool storeVote(const QString& hash, bool good);
    void onRecordStored(const StoredRecord& record, bool isRemote);

    P2PStore* store_;
    data::TorrentRepository* repository_;

    // Hashes this instance has already voted on, so repeated votes are deduped
    // even when the distributed store is unavailable (hasVoted() can only answer
    // when the store is up). Persistent dedup still comes from the store records.
    QSet<QString> selfVotedHashes_;
};

} // namespace rats::service

#endif // RATS_SERVICE_VOTING_SERVICE_H
