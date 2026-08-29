#include "services/database_exporter.h"

#include "data/torrent_repository.h"

#include <QFileInfo>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace rats::service {

namespace {

// Torrents per database page. Also the size of the IN() batch that fetches the
// page's file lists, and both must stay under Manticore's max_matches (1000).
constexpr int kPageSize = 500;
// Pages the database reader may run ahead of the serialiser. Three is enough to
// ride out one slow query without letting the queue grow into a second copy of
// the index in memory.
constexpr size_t kReadAhead = 3;

// Hand-off between the thread reading the database and the thread turning pages
// into compressed frames. Bounded in both directions: the reader blocks once it
// is kReadAhead pages ahead, the serialiser blocks while the queue is empty and
// the reader is still going.
class PageQueue {
public:
    using Page = QVector<domain::Torrent>;

    // Returns false if the queue was closed while we waited (bail out).
    bool push(Page page)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this] { return closed_ || pages_.size() < kReadAhead; });
        if (closed_)
            return false;
        pages_.push_back(std::move(page));
        notEmpty_.notify_one();
        return true;
    }

    // Returns false once the queue is drained and the reader has finished.
    bool pop(Page& out)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return closed_ || finished_ || !pages_.empty(); });
        if (pages_.empty())
            return false;
        out = std::move(pages_.front());
        pages_.pop_front();
        notFull_.notify_one();
        return true;
    }

    // Reader is done producing; pop() may still drain what is left.
    void finish()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        notEmpty_.notify_all();
    }

    // Hard stop for both sides — nobody blocks or produces after this.
    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        pages_.clear();
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Page> pages_;
    bool finished_ = false;
    bool closed_ = false;
};

} // namespace

DatabaseExporter::DatabaseExporter(data::TorrentRepository* repository) : repository_(repository) { }

DatabaseExporter::Result DatabaseExporter::run(
    const QString& path, const dump::Header& header, const CancelToken& cancel, const ProgressFn& onProgress)
{
    Result result;
    if (!repository_) {
        result.error = tr("Database is not available.");
        return result;
    }

    DumpWriter writer;
    QString error;
    if (!writer.open(path, header, &error)) {
        result.error = error;
        return result;
    }

    PageQueue queue;
    // Published by the serialiser, read by this thread for progress. Only ever
    // grow, and a slightly stale read is fine for a progress line.
    std::atomic<qint64> written { 0 };
    std::atomic<qint64> bytes { 0 };
    std::atomic<bool> writeFailed { false };
    QString writeError;

    // The database stays on *this* thread on purpose: Manticore hands out one
    // connection per QThread, and a connection opened on a raw std::thread would
    // outlive it. The spawned thread therefore gets the half that touches no Qt
    // SQL at all — JSON, zlib and the file write.
    std::thread serialiser([&] {
        PageQueue::Page page;
        while (queue.pop(page)) {
            for (const domain::Torrent& t : page) {
                if (!writer.write(t, &writeError)) {
                    writeFailed = true;
                    queue.close(); // stop the reader; it is waiting on us
                    return;
                }
                written.fetch_add(1);
            }
            bytes.store(writer.bytesWritten());
        }
    });

    // Keyset ("seek") pagination, for the reason spelled out on pageAfterId(): an
    // OFFSET sweep cannot walk past max_matches, and rows added mid-sweep shift
    // the offsets already consumed.
    qint64 afterId = 0;
    while (!cancel.cancelled() && !writeFailed.load()) {
        PageQueue::Page page = repository_->pageAfterId(afterId, kPageSize);
        if (page.isEmpty())
            break;
        afterId = page.last().id;

        QStringList hashes;
        hashes.reserve(page.size());
        for (const domain::Torrent& t : page)
            hashes << t.hash;
        const QHash<QString, QVector<domain::File>> files = repository_->filesOf(hashes);
        for (domain::Torrent& t : page)
            t.fileList = files.value(t.hash);

        if (!queue.push(std::move(page)))
            break;
        if (onProgress)
            onProgress(written.load(), bytes.load());
    }

    queue.finish();
    if (cancel.cancelled())
        queue.close(); // unblock a serialiser parked on an empty queue
    serialiser.join();

    result.torrents = written.load();

    if (cancel.cancelled()) {
        writer.abort();
        result.ok = false;
        return result; // cancellation is not an error; the caller knows it asked
    }
    if (writeFailed.load() || !writer.finish(&error)) {
        writer.abort();
        const QString reason = writeFailed.load() ? writeError : error;
        result.error = reason.isEmpty() ? tr("Failed to write the dump.") : reason;
        return result;
    }

    result.ok = true;
    result.bytes = QFileInfo(path).size();
    if (onProgress)
        onProgress(result.torrents, result.bytes);
    return result;
}

} // namespace rats::service
