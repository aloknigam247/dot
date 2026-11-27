#ifndef TABPAGEMODEL_H
#define TABPAGEMODEL_H

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <qqmlregistration.h>
#include <QString>

#include <expected>
#include <optional>

namespace dot {

// A Neovim tabpage handle as a msgpack EXT value. encoded keeps the exact EXT body so a selection
// sends back the bytes Neovim sent; id is the decoded integer.
struct TabpageHandle {
    QByteArray encoded;
    qint8 extType = 0;
    qint64 id = 0;

    bool operator==(const TabpageHandle &other) const {
        return extType == other.extType && id == other.id;
    }
};

struct TabpageEntry {
    TabpageHandle handle;
    QString name;
};

struct TablineUpdate {
    TabpageHandle current;
    QList<TabpageEntry> tabs;
};

struct PendingSelection {
    quint64 generation = 0;
    TabpageHandle handle;
};

// Neovim-owned tabpages from ext_tabline redraw events, in Neovim's tab order. Only
// tabline_update changes currentHandle; a selection reports whether Neovim accepted it.
class TabpageModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by NvimConnector")

    Q_PROPERTY(qint64 currentHandle READ currentHandle NOTIFY currentHandleChanged)
    Q_PROPERTY(QString selectionError READ selectionError NOTIFY selectionErrorChanged)
    Q_PROPERTY(bool selectionPending READ selectionPending NOTIFY selectionPendingChanged)

public:
    enum Roles {
        CurrentRole = Qt::UserRole + 1,
        HandleRole,
        NameRole,
    };

    explicit TabpageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    qint64 currentHandle() const { return m_currentHandle; }
    QString selectionError() const { return m_selectionError; }
    bool selectionPending() const { return m_pending.has_value(); }

    std::expected<void, QString> apply(const TablineUpdate &update);
    std::optional<PendingSelection> beginSelection(qint64 handle);
    void cancelSelection(const QString &reason);
    void finishSelection(quint64 generation, const std::expected<void, QString> &result);
    void reset(const QString &reason);

signals:
    void currentHandleChanged();
    void selectionErrorChanged();
    void selectionFinished(qint64 handle, bool succeeded, const QString &error);
    void selectionPendingChanged();

private:
    void clearSelectionError();
    void failSelection(qint64 id, const QString &reason);

    qint64 m_currentHandle = 0;
    std::optional<PendingSelection> m_pending;
    QList<TabpageEntry> m_rows;
    QString m_selectionError;
    quint64 m_selectionGeneration = 0;
};

} // namespace dot

#endif
