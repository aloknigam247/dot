#ifndef CMDLINEMODEL_H
#define CMDLINEMODEL_H

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <qqmlregistration.h>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <expected>

namespace dot {

struct CmdlineChunk {
    int attrId = 0;
    int highlightId = 0;
    QByteArray utf8;
};

using CmdlineLine = QList<CmdlineChunk>;

struct CmdlineSession {
    CmdlineLine content;
    QString firstCharacter;
    int highlightId = -1;
    int indent = 0;
    qsizetype position = 0;
    QString prompt;
};

// Lines of the visible command-line block, each with the chunk shape of a level's chunks.
class CmdlineBlockModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by CmdlineModel")

public:
    enum Roles {
        ChunksRole = Qt::UserRole + 1,
    };

    explicit CmdlineBlockModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const QList<QVariantList> &lines() const { return m_lines; }

private:
    friend class CmdlineModel;

    void append(QVariantList line);
    void replace(QList<QVariantList> lines);

    QList<QVariantList> m_lines;
};

// Neovim-owned command-line state from ext_cmdline redraw events. Rows are the visible nesting
// levels in ascending level order; the last row is the active level. Cursor text offsets index the
// concatenated chunk texts. Neovim reports inputsecret() cursors against the unmasked input, so a
// masked prompt's cursor is approximate. A row's prompt style is the attribute map of the
// highlight group its highlight id names, once resolved through setPromptStyle().
class CmdlineModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by NvimConnector")

    Q_PROPERTY(int activeLevel READ activeLevel NOTIFY activeLevelChanged)
    Q_PROPERTY(dot::CmdlineBlockModel *block READ block CONSTANT)
    Q_PROPERTY(bool blockActive READ blockActive NOTIFY blockActiveChanged)

public:
    enum Roles {
        ChunksRole = Qt::UserRole + 1,
        CursorGraphemeRole,
        CursorTextOffsetRole,
        FirstCharacterRole,
        HighlightIdRole,
        IndentRole,
        LevelRole,
        PromptRole,
        PromptStyleRole,
        SpecialCharacterRole,
        SpecialShiftRole,
    };

    explicit CmdlineModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeLevel() const { return m_levels.isEmpty() ? 0 : m_levels.constLast().level; }
    CmdlineBlockModel *block() const { return m_block; }
    bool blockActive() const { return m_blockActive; }

    std::expected<void, QString> appendBlock(const CmdlineLine &line);
    std::expected<void, QString> hide(int level);
    void hideBlock();
    void reset();
    std::expected<void, QString> setPosition(int level, qsizetype position);
    void setPromptStyle(int highlightId, const QVariantMap &style);
    std::expected<void, QString> setSpecialCharacter(int level, const QString &character,
                                                     bool shift);
    std::expected<void, QString> show(int level, const CmdlineSession &session);
    void showBlock(const QList<CmdlineLine> &lines);

signals:
    void activeLevelChanged();
    void blockActiveChanged();

private:
    struct Level {
        QVariantList chunks;
        qsizetype cursorTextOffset = 0;
        QString firstCharacter;
        int highlightId = -1;
        int indent = 0;
        int level = 0;
        QString prompt;
        QString specialCharacter;
        bool specialShift = false;
        QString text;
        // Text offset of the code point starting at each content byte; -1 inside a sequence.
        QList<qsizetype> textOffsets;
    };

    qsizetype rowForLevel(int level) const;
    qsizetype lowerBound(int level) const;

    CmdlineBlockModel *m_block;
    bool m_blockActive = false;
    QList<Level> m_levels;
    QHash<int, QVariantMap> m_promptStyles;
};

} // namespace dot

#endif
