#ifndef CHATMODEL_H
#define CHATMODEL_H

#include <QAbstractListModel>
#include <qqmlregistration.h>
#include <QString>
#include <QVector>

namespace dot {

// Passive transcript of externally supplied blocks. Never drives a backend or
// fabricates replies.
//
// Creatable QML_ELEMENT (unlike the nvim-owned models, which are
// QML_UNCREATABLE): chat is dot-native, drives no nvim RPC, and is
// instantiated directly by ChatPanel.qml — which keeps it unit-testable in
// isolation without a live NvimConnector.
class ChatModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        AuthorRole = Qt::UserRole + 1,
        TextRole,
    };

    explicit ChatModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_msgs.size()); }

    // Appends a block verbatim. `author` is "user", "assistant", or "system";
    // anything else is treated as "system". No-op on empty `text`.
    Q_INVOKABLE void appendBlock(const QString &author, const QString &text);

    // Read helpers for bindings and tests. Return empty on out-of-range.
    Q_INVOKABLE QString textAt(int row) const;
    Q_INVOKABLE QString authorAt(int row) const;

signals:
    void countChanged();

private:
    enum class Author {
        User,
        Assistant,
        System,
    };

    struct Message {
        Author author;
        QString text;
    };

    void appendMessage(Author author, const QString &text);

    static QString authorName(Author a);
    static Author authorFromName(const QString &name);

    QVector<Message> m_msgs;
};

} // namespace dot

#endif
