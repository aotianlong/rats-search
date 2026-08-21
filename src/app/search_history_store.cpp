#include "app/search_history_store.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace rats::app {

SearchHistoryStore::SearchHistoryStore(const QString& dataDirectory, QObject* parent)
    : QObject(parent), filePath_(dataDirectory + QStringLiteral("/search-history.json"))
{
    load();
}

void SearchHistoryStore::load()
{
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    file.close();

    entries_.clear();
    for (const QJsonValue& v : array) {
        const QJsonObject obj = v.toObject();
        Entry entry;
        entry.query = obj["query"].toString().trimmed();
        if (entry.query.isEmpty() || indexOf(entry.query) >= 0)
            continue;
        const qint64 ms = obj["lastSearchedAt"].toVariant().toLongLong();
        entry.lastSearchedAt = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime::currentDateTime();
        entry.count = qMax(1, obj["count"].toInt(1));
        entries_.append(entry);
        if (entries_.size() >= kMaxEntries)
            break;
    }
}

void SearchHistoryStore::save()
{
    QJsonArray array;
    for (const Entry& e : entries_) {
        array.append(QJsonObject {
            { "query", e.query }, { "lastSearchedAt", e.lastSearchedAt.toMSecsSinceEpoch() }, { "count", e.count } });
    }
    QFile file(filePath_);
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

bool SearchHistoryStore::add(const QString& query)
{
    if (!enabled_)
        return false;

    const QString trimmed = query.trimmed().left(kMaxQueryLength);
    if (trimmed.isEmpty())
        return false;

    Entry entry { trimmed, QDateTime::currentDateTime(), 1 };

    // A repeat keeps the count (and the original spelling is replaced by the
    // one just typed) and moves back to the front.
    const int existing = indexOf(trimmed);
    if (existing >= 0) {
        entry.count = entries_.at(existing).count + 1;
        entries_.remove(existing);
    }

    entries_.prepend(entry);
    while (entries_.size() > kMaxEntries)
        entries_.removeLast();

    save();
    emit historyChanged();
    return true;
}

bool SearchHistoryStore::remove(const QString& query)
{
    const int index = indexOf(query.trimmed());
    if (index < 0)
        return false;
    entries_.remove(index);
    save();
    emit historyChanged();
    return true;
}

bool SearchHistoryStore::clear()
{
    if (entries_.isEmpty())
        return false;
    entries_.clear();
    save();
    emit historyChanged();
    return true;
}

void SearchHistoryStore::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

QStringList SearchHistoryStore::queries(int limit) const
{
    QStringList list;
    for (const Entry& e : entries_) {
        if (limit >= 0 && list.size() >= limit)
            break;
        list.append(e.query);
    }
    return list;
}

int SearchHistoryStore::indexOf(const QString& query) const
{
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.at(i).query.compare(query, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

} // namespace rats::app
