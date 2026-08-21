#ifndef RATS_APP_SEARCH_HISTORY_STORE_H
#define RATS_APP_SEARCH_HISTORY_STORE_H

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace rats::app {

// Recent user search queries, persisted to search-history.json in the data
// directory. Most-recent-first, deduplicated case-insensitively (a repeated
// query moves back to the front and bumps its counter) and capped at
// kMaxEntries, so the file can never grow without bound.
//
// Recording is deliberately explicit: only front-ends acting on a *user*
// initiated search call add() — the P2P and replication search paths never do.
class SearchHistoryStore : public QObject {
    Q_OBJECT

public:
    struct Entry {
        QString query;
        QDateTime lastSearchedAt;
        int count = 1;
    };

    // Anything longer is a paste accident, not a query worth remembering.
    static constexpr int kMaxQueryLength = 256;
    static constexpr int kMaxEntries = 100;

    explicit SearchHistoryStore(const QString& dataDirectory, QObject* parent = nullptr);

    void load();
    void save();

    // Records a query. Returns false (and changes nothing) when history is
    // disabled or the query is empty/blank after trimming.
    bool add(const QString& query);
    bool remove(const QString& query); // false if not present
    bool clear(); // false if already empty

    // Pushed from ConfigStore through Application::applyConfig(). While
    // disabled add() is a no-op; existing entries are kept until the user
    // clears them explicitly.
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool enabled);

    QVector<Entry> entries() const { return entries_; }
    int size() const { return entries_.size(); }

    // Query strings only, most recent first — what the UI completer needs.
    QStringList queries(int limit = kMaxEntries) const;

signals:
    void historyChanged();

private:
    int indexOf(const QString& query) const; // case-insensitive, -1 if absent

    QString filePath_;
    QVector<Entry> entries_;
    bool enabled_ = true;
};

} // namespace rats::app

#endif // RATS_APP_SEARCH_HISTORY_STORE_H
