#ifndef HIGHLIGHTTABLE_H
#define HIGHLIGHTTABLE_H

#include <array>
#include <msgpack.hpp>
#include <optional>
#include <unordered_map>
#include <QColor>
#include <QObject>
#include <qqmlregistration.h>
#include <QSet>
#include <QString>
#include <QStringList>

namespace dot {

enum class RequiredHighlight {
    NormalFloat,
    Pmenu,
    Visual,
};

struct HlAttr {
    QColor fg;
    QColor bg;
    QColor sp;
    bool bold = false;
    bool italic = false;
    bool isRounded = false;
    QStringList names;
    bool underline = false;
    bool undercurl = false;
    bool strikethrough = false;
    bool reverse = false;
    int blend = 0;
};

class HighlightTable : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by NvimConnector")

public:
    explicit HighlightTable(QObject *parent = nullptr);

    void setDefaultColors(int rgbFg, int rgbBg, int rgbSp);
    void defineAttr(int id, const msgpack::object &rgbAttr, const msgpack::object *info = nullptr);
    void clear();

    void setRoundedHighlights(const QStringList &names);

    bool contains(int id) const;
    std::optional<int> requiredAttrId(RequiredHighlight role) const;
    void setHighlightGroup(const QString &name, int id);

    HlAttr attr(int id) const;
    bool isRounded(int id) const;
    QColor defaultFg() const { return m_defaultFg; }
    QColor defaultBg() const { return m_defaultBg; }
    QColor defaultSp() const { return m_defaultSp; }

    HlAttr resolved(int id) const;

signals:
    void changed();

private:
    QColor m_defaultFg;
    QColor m_defaultBg;
    QColor m_defaultSp;
    QSet<QString> m_roundedHighlights;
    std::unordered_map<int, HlAttr> m_attrs;
    std::array<std::optional<int>, 3> m_requiredAttrIds;
};

} // namespace dot

#endif
