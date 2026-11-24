#ifndef CMDLINEMODEL_H
#define CMDLINEMODEL_H

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <qqmlregistration.h>
#include <QString>
#include <QVariantList>

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

// Neovim-owned command-line state from ext_cmdline redraw events. Rows are the visible nesting
// levels in ascending level order; the last row is the active level. Cursor byte offsets index the
// UTF-8 content Neovim sent, and cursor text offsets index the concatenated chunk texts.
class CmdlineModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by NvimConnector")

    Q_PROPERTY(int activeLevel READ activeLevel NOTIFY activeLevelChanged)
    Q_PROPERTY(bool blockActive READ blockActive NOTIFY blockChanged)
    Q_PROPERTY(QVariantList blockLines READ blockLines NOTIFY blockChanged)

public:
    enum Roles {
        ChunksRole = Qt::UserRole + 1,
        CursorByteOffsetRole,
        CursorTextOffsetRole,
        FirstCharacterRole,
        HighlightIdRole,
        IndentRole,
        LevelRole,
        PromptRole,
        SpecialCharacterRole,
        SpecialShiftRole,
    };

    explicit CmdlineModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeLevel() const { return m_levels.isEmpty() ? 0 : m_levels.constLast().level; }
    bool blockActive() const { return m_blockActive; }
    QVariantList blockLines() const { return m_blockLines; }

    std::expected<void, QString> appendBlock(const CmdlineLine &line);
    void hide(int level, bool abort);
    void hideBlock();
    void reset();
    std::expected<void, QString> setPosition(int level, qsizetype position);
    std::expected<void, QString> setSpecialCharacter(int level, const QString &character,
                                                     bool shift);
    std::expected<void, QString> show(int level, const CmdlineSession &session);
    void showBlock(const QList<CmdlineLine> &lines);

signals:
    void activeLevelChanged();
    void blockChanged();
    void cmdlineHidden(int level, bool abort);

private:
    struct Level {
        QVariantList chunks;
        qsizetype cursorByteOffset = 0;
        qsizetype cursorTextOffset = 0;
        QString firstCharacter;
        int highlightId = -1;
        int indent = 0;
        int level = 0;
        QString prompt;
        QString specialCharacter;
        bool specialShift = false;
        // Text offset of the code point starting at each content byte; -1 inside a sequence.
        QList<qsizetype> textOffsets;
    };

    qsizetype rowForLevel(int level) const;
    qsizetype lowerBound(int level) const;

    bool m_blockActive = false;
    QVariantList m_blockLines;
    QList<Level> m_levels;
};

} // namespace dot

#endif
