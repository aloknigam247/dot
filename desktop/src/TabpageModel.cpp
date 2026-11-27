#include "TabpageModel.h"

#include <algorithm>
#include <iterator>

namespace dot {

static bool containsHandle(QList<TabpageEntry>::const_iterator first,
                           QList<TabpageEntry>::const_iterator last, const TabpageHandle &handle) {
    return std::any_of(first, last,
                       [&handle](const TabpageEntry &entry) { return entry.handle == handle; });
}

static bool containsHandle(const QList<TabpageEntry> &entries, const TabpageHandle &handle) {
    return containsHandle(entries.cbegin(), entries.cend(), handle);
}

TabpageModel::TabpageModel(QObject *parent) : QAbstractListModel(parent) {}

int TabpageModel::rowCount(const QModelIndex &parent) const {
    if(parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant TabpageModel::data(const QModelIndex &index, int role) const {
    const int row = index.row();
    if(!index.isValid() || row < 0 || row >= m_rows.size()) return {};
    const TabpageEntry &entry = m_rows[row];
    switch(role) {
        case CurrentRole:
            return entry.handle.id == m_currentHandle;
        case HandleRole:
            return static_cast<qlonglong>(entry.handle.id);
        case NameRole:
            return entry.name;
        default:
            return {};
    }
}

QHash<int, QByteArray> TabpageModel::roleNames() const {
    return {
        { CurrentRole, QByteArrayLiteral("current") },
        { HandleRole, QByteArrayLiteral("handle") },
        { NameRole, QByteArrayLiteral("name") },
    };
}

std::expected<void, QString> TabpageModel::apply(const TablineUpdate &update) {
    for(auto tab = update.tabs.cbegin(); tab != update.tabs.cend(); ++tab) {
        if(containsHandle(update.tabs.cbegin(), tab, tab->handle))
            return std::unexpected(QStringLiteral("duplicate tabpage %1").arg(tab->handle.id));
    }
    if(!containsHandle(update.tabs, update.current))
        return std::unexpected(
            QStringLiteral("current tabpage %1 is not in tabs").arg(update.current.id));

    const qint64 previousCurrent = m_currentHandle;
    m_currentHandle = update.current.id;
    for(qsizetype row = m_rows.size() - 1; row >= 0; --row) {
        if(containsHandle(update.tabs, m_rows[row].handle)) continue;
        beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
        m_rows.removeAt(row);
        endRemoveRows();
    }
    // Rows before i already match the update, and every remaining row is still in the update, so
    // each existing tab is found at or after i and only ever moves up.
    for(qsizetype i = 0; i < update.tabs.size(); ++i) {
        const TabpageEntry &entry = update.tabs[i];
        const int target = static_cast<int>(i);
        const auto found = std::find_if(m_rows.cbegin() + i, m_rows.cend(),
                                        [&entry](const TabpageEntry &row) {
                                            return row.handle == entry.handle;
                                        });
        if(found == m_rows.cend()) {
            beginInsertRows({}, target, target);
            m_rows.insert(i, entry);
            endInsertRows();
            continue;
        }
        const auto from = static_cast<int>(std::distance(m_rows.cbegin(), found));
        if(from != target) {
            beginMoveRows({}, from, from, {}, target);
            m_rows.move(from, i);
            endMoveRows();
        }
        TabpageEntry &row = m_rows[i];
        QList<int> roles;
        if((row.handle.id == previousCurrent) != (entry.handle.id == m_currentHandle))
            roles.append(CurrentRole);
        if(row.name != entry.name) roles.append(NameRole);
        row = entry;
        if(!roles.isEmpty()) emit dataChanged(index(target), index(target), roles);
    }
    if(m_currentHandle != previousCurrent) emit currentHandleChanged();
    if(m_pending && !containsHandle(m_rows, m_pending->handle))
        cancelSelection(QStringLiteral("Tabpage closed"));
    return {};
}

std::optional<PendingSelection> TabpageModel::beginSelection(qint64 handle) {
    clearSelectionError();
    const auto found = std::ranges::find_if(
        m_rows, [handle](const TabpageEntry &entry) { return entry.handle.id == handle; });
    if(found == m_rows.cend()) {
        failSelection(handle, QStringLiteral("Unknown tabpage"));
        return std::nullopt;
    }
    const TabpageHandle target = found->handle;
    const bool wasPending = m_pending.has_value();
    if(wasPending)
        failSelection(m_pending->handle.id, QStringLiteral("Superseded by a newer selection"));
    m_pending = PendingSelection{ ++m_selectionGeneration, target };
    if(!wasPending) emit selectionPendingChanged();
    return m_pending;
}

void TabpageModel::cancelSelection(const QString &reason) {
    if(!m_pending) return;
    const qint64 id = m_pending->handle.id;
    m_pending.reset();
    emit selectionPendingChanged();
    failSelection(id, reason);
}

void TabpageModel::finishSelection(quint64 generation, const std::expected<void, QString> &result) {
    if(!m_pending || m_pending->generation != generation) return;
    if(!result) {
        cancelSelection(result.error());
        return;
    }
    const qint64 id = m_pending->handle.id;
    m_pending.reset();
    emit selectionPendingChanged();
    clearSelectionError();
    emit selectionFinished(id, true, {});
}

void TabpageModel::reset(const QString &reason) {
    clearSelectionError();
    cancelSelection(reason);
    if(!m_rows.isEmpty()) {
        beginResetModel();
        m_rows.clear();
        endResetModel();
    }
    if(m_currentHandle != 0) {
        m_currentHandle = 0;
        emit currentHandleChanged();
    }
}

void TabpageModel::clearSelectionError() {
    if(m_selectionError.isEmpty()) return;
    m_selectionError.clear();
    emit selectionErrorChanged();
}

void TabpageModel::failSelection(qint64 id, const QString &reason) {
    if(m_selectionError != reason) {
        m_selectionError = reason;
        emit selectionErrorChanged();
    }
    emit selectionFinished(id, false, reason);
}

} // namespace dot
