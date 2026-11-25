#include "CmdlineModel.h"

#include <QVariantMap>

#include <algorithm>
#include <iterator>
#include <utility>

namespace dot {

namespace {

struct CodePoint {
    qsizetype length = 1;
    char32_t value = QChar::ReplacementCharacter;
};

} // namespace

// Malformed input decodes as U+FFFD consuming only the offending byte, so every byte that does
// not continue a well-formed sequence is a cursor boundary.
static CodePoint decodeCodePoint(const QByteArray &bytes, qsizetype index) {
    const auto lead = static_cast<unsigned char>(bytes[index]);
    if(lead < 0x80) return { 1, lead };
    CodePoint decoded;
    unsigned char maximum = 0xBF;
    unsigned char minimum = 0x80;
    if(lead >= 0xC2 && lead <= 0xDF) {
        decoded = { 2, static_cast<char32_t>(lead & 0x1FU) };
    } else if(lead >= 0xE0 && lead <= 0xEF) {
        decoded = { 3, static_cast<char32_t>(lead & 0x0FU) };
        if(lead == 0xE0) minimum = 0xA0;
        if(lead == 0xED) maximum = 0x9F;
    } else if(lead >= 0xF0 && lead <= 0xF4) {
        decoded = { 4, static_cast<char32_t>(lead & 0x07U) };
        if(lead == 0xF0) minimum = 0x90;
        if(lead == 0xF4) maximum = 0x8F;
    } else {
        return {};
    }
    if(index + decoded.length > bytes.size()) return {};
    for(qsizetype i = 1; i < decoded.length; ++i) {
        const auto byte = static_cast<unsigned char>(bytes[index + i]);
        if(byte < minimum || byte > maximum) return {};
        decoded.value = (decoded.value << 6U) | (byte & 0x3FU);
        maximum = 0xBF;
        minimum = 0x80;
    }
    return decoded;
}

// Appends the text offset of every byte in `bytes` to `offsets` when it is non-null.
static QString decodeUtf8(const QByteArray &bytes, qsizetype textBase, QList<qsizetype> *offsets) {
    QString text;
    text.reserve(bytes.size());
    qsizetype index = 0;
    while(index < bytes.size()) {
        const CodePoint codePoint = decodeCodePoint(bytes, index);
        if(offsets) {
            offsets->append(textBase + text.size());
            for(qsizetype i = 1; i < codePoint.length; ++i) offsets->append(-1);
        }
        if(QChar::requiresSurrogates(codePoint.value)) {
            text.append(QChar(QChar::highSurrogate(codePoint.value)));
            text.append(QChar(QChar::lowSurrogate(codePoint.value)));
        } else {
            text.append(QChar(static_cast<char16_t>(codePoint.value)));
        }
        index += codePoint.length;
    }
    return text;
}

// The final entry of `offsets`, when requested, is the text offset of the end of the line.
static QVariantList chunkList(const CmdlineLine &line, QList<qsizetype> *offsets) {
    QVariantList chunks;
    chunks.reserve(line.size());
    qsizetype textBase = 0;
    for(const CmdlineChunk &chunk: line) {
        const QString text = decodeUtf8(chunk.utf8, textBase, offsets);
        textBase += text.size();
        chunks.append(QVariantMap{
            { QStringLiteral("attrId"), chunk.attrId },
            { QStringLiteral("highlightId"), chunk.highlightId },
            { QStringLiteral("text"), text },
        });
    }
    if(offsets) offsets->append(textBase);
    return chunks;
}

static std::expected<qsizetype, QString> textOffset(const QList<qsizetype> &offsets,
                                                    qsizetype position) {
    if(position < 0 || position >= offsets.size()) {
        return std::unexpected(QStringLiteral("cursor byte offset %1 is outside %2 content bytes")
                                   .arg(position)
                                   .arg(offsets.size() - 1));
    }
    if(offsets[position] < 0) {
        return std::unexpected(
            QStringLiteral("cursor byte offset %1 is inside a UTF-8 sequence").arg(position));
    }
    return offsets[position];
}

static QString invisibleLevel(int level) {
    return QStringLiteral("level %1 is not visible").arg(level);
}

static QString nonPositiveLevel(int level) {
    return QStringLiteral("level %1 is not positive").arg(level);
}

CmdlineModel::CmdlineModel(QObject *parent) : QAbstractListModel(parent) {}

int CmdlineModel::rowCount(const QModelIndex &parent) const {
    if(parent.isValid()) return 0;
    return static_cast<int>(m_levels.size());
}

QVariant CmdlineModel::data(const QModelIndex &index, int role) const {
    const int row = index.row();
    if(!index.isValid() || row < 0 || row >= m_levels.size()) return {};
    const Level &level = m_levels[row];
    switch(role) {
        case ChunksRole:
            return level.chunks;
        case CursorByteOffsetRole:
            return static_cast<qlonglong>(level.cursorByteOffset);
        case CursorTextOffsetRole:
            return static_cast<qlonglong>(level.cursorTextOffset);
        case FirstCharacterRole:
            return level.firstCharacter;
        case HighlightIdRole:
            return level.highlightId;
        case IndentRole:
            return level.indent;
        case LevelRole:
            return level.level;
        case PromptRole:
            return level.prompt;
        case SpecialCharacterRole:
            return level.specialCharacter;
        case SpecialShiftRole:
            return level.specialShift;
        default:
            return {};
    }
}

QHash<int, QByteArray> CmdlineModel::roleNames() const {
    return {
        { ChunksRole, QByteArrayLiteral("chunks") },
        { CursorByteOffsetRole, QByteArrayLiteral("cursorByteOffset") },
        { CursorTextOffsetRole, QByteArrayLiteral("cursorTextOffset") },
        { FirstCharacterRole, QByteArrayLiteral("firstCharacter") },
        { HighlightIdRole, QByteArrayLiteral("highlightId") },
        { IndentRole, QByteArrayLiteral("indent") },
        { LevelRole, QByteArrayLiteral("level") },
        { PromptRole, QByteArrayLiteral("prompt") },
        { SpecialCharacterRole, QByteArrayLiteral("specialCharacter") },
        { SpecialShiftRole, QByteArrayLiteral("specialShift") },
    };
}

std::expected<void, QString> CmdlineModel::appendBlock(const CmdlineLine &line) {
    if(!m_blockActive) return std::unexpected(QStringLiteral("no block is visible"));
    m_blockLines.append(QVariant(chunkList(line, nullptr)));
    emit blockChanged();
    return {};
}

std::expected<void, QString> CmdlineModel::hide(int level) {
    if(level < 1) return std::unexpected(nonPositiveLevel(level));
    const qsizetype row = rowForLevel(level);
    if(row < 0) return {};
    const int previousActiveLevel = activeLevel();
    beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
    m_levels.removeAt(row);
    endRemoveRows();
    if(activeLevel() != previousActiveLevel) emit activeLevelChanged();
    return {};
}

void CmdlineModel::hideBlock() {
    if(!m_blockActive) return;
    m_blockActive = false;
    m_blockLines.clear();
    emit blockChanged();
}

void CmdlineModel::reset() {
    if(!m_levels.isEmpty()) {
        beginResetModel();
        m_levels.clear();
        endResetModel();
        emit activeLevelChanged();
    }
    hideBlock();
}

std::expected<void, QString> CmdlineModel::setPosition(int level, qsizetype position) {
    const qsizetype row = rowForLevel(level);
    if(row < 0) return std::unexpected(invisibleLevel(level));
    Level &current = m_levels[row];
    const auto offset = textOffset(current.textOffsets, position);
    if(!offset) return std::unexpected(offset.error());
    if(current.cursorByteOffset == position) return {};
    current.cursorByteOffset = position;
    current.cursorTextOffset = *offset;
    const QModelIndex changed = index(static_cast<int>(row));
    emit dataChanged(changed, changed, { CursorByteOffsetRole, CursorTextOffsetRole });
    return {};
}

std::expected<void, QString> CmdlineModel::setSpecialCharacter(int level, const QString &character,
                                                               bool shift) {
    const qsizetype row = rowForLevel(level);
    if(row < 0) return std::unexpected(invisibleLevel(level));
    Level &current = m_levels[row];
    if(current.specialCharacter == character && current.specialShift == shift) return {};
    current.specialCharacter = character;
    current.specialShift = shift;
    const QModelIndex changed = index(static_cast<int>(row));
    emit dataChanged(changed, changed, { SpecialCharacterRole, SpecialShiftRole });
    return {};
}

std::expected<void, QString> CmdlineModel::show(int level, const CmdlineSession &session) {
    if(level < 1) return std::unexpected(nonPositiveLevel(level));
    Level next;
    next.chunks = chunkList(session.content, &next.textOffsets);
    const auto offset = textOffset(next.textOffsets, session.position);
    if(!offset) return std::unexpected(offset.error());
    next.cursorByteOffset = session.position;
    next.cursorTextOffset = *offset;
    next.firstCharacter = session.firstCharacter;
    next.highlightId = session.highlightId;
    next.indent = session.indent;
    next.level = level;
    next.prompt = session.prompt;

    const qsizetype row = lowerBound(level);
    if(row < m_levels.size() && m_levels[row].level == level) {
        m_levels[row] = std::move(next);
        const QModelIndex changed = index(static_cast<int>(row));
        emit dataChanged(changed, changed);
        return {};
    }
    const int previousActiveLevel = activeLevel();
    beginInsertRows({}, static_cast<int>(row), static_cast<int>(row));
    m_levels.insert(row, std::move(next));
    endInsertRows();
    if(activeLevel() != previousActiveLevel) emit activeLevelChanged();
    return {};
}

void CmdlineModel::showBlock(const QList<CmdlineLine> &lines) {
    m_blockLines.clear();
    m_blockLines.reserve(lines.size());
    for(const CmdlineLine &line: lines) m_blockLines.append(QVariant(chunkList(line, nullptr)));
    m_blockActive = true;
    emit blockChanged();
}

qsizetype CmdlineModel::lowerBound(int level) const {
    const auto it = std::ranges::lower_bound(m_levels, level, {}, &Level::level);
    return std::distance(m_levels.cbegin(), it);
}

qsizetype CmdlineModel::rowForLevel(int level) const {
    const qsizetype row = lowerBound(level);
    return row < m_levels.size() && m_levels[row].level == level ? row : -1;
}

} // namespace dot
