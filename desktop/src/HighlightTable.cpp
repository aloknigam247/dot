#include "HighlightTable.h"

#include <algorithm>
#include <string_view>
#include <QDebug>

namespace dot {

static constexpr std::array<std::string_view, 3> kRequiredNames{ "NormalFloat", "Pmenu", "Visual" };

static QColor fromRgb24(int rgb) {
    if(rgb < 0) return {};
    return QColor::fromRgb(static_cast<QRgb>(rgb));
}

static int findKey(const msgpack::object &mapObj, const char *key) {
    if(mapObj.type != msgpack::type::MAP) return -1;
    const auto &m = mapObj.via.map;
    for(uint32_t i = 0; i < m.size; ++i) {
        if(m.ptr[i].key.type == msgpack::type::STR) {
            const auto &s = m.ptr[i].key.via.str;
            if(s.size == std::strlen(key) && std::memcmp(s.ptr, key, s.size) == 0) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

static bool getBool(const msgpack::object &mapObj, const char *key) {
    const int i = findKey(mapObj, key);
    if(i < 0) return false;
    const auto &v = mapObj.via.map.ptr[i].val;
    return v.type == msgpack::type::BOOLEAN && v.via.boolean;
}

static int getInt(const msgpack::object &mapObj, const char *key, int def = -1) {
    const int i = findKey(mapObj, key);
    if(i < 0) return def;
    const auto &v = mapObj.via.map.ptr[i].val;
    if(v.type == msgpack::type::POSITIVE_INTEGER) return static_cast<int>(v.via.u64);
    if(v.type == msgpack::type::NEGATIVE_INTEGER) return static_cast<int>(v.via.i64);
    return def;
}

// Returns the STR value at `key` if present, or an empty QString.
static QString getString(const msgpack::object &mapObj, const char *key) {
    const int i = findKey(mapObj, key);
    if(i < 0) return {};
    const auto &v = mapObj.via.map.ptr[i].val;
    if(v.type != msgpack::type::STR) return {};
    const auto &s = v.via.str;
    return QString::fromUtf8(s.ptr, static_cast<int>(s.size));
}

// ext_hlstate gives us an `info` array of dicts describing how the resolved
// attribute was composed (kind/ui_name/hi_name). Collect every named entry
// (preferring `ui_name`, falling back to `hi_name`).
static QStringList namesFromInfo(const msgpack::object &info) {
    QStringList out;
    if(info.type != msgpack::type::ARRAY) return out;
    const auto &arr = info.via.array;
    out.reserve(static_cast<int>(arr.size));
    for(uint32_t i = 0; i < arr.size; ++i) {
        const auto &e = arr.ptr[i];
        if(e.type != msgpack::type::MAP) continue;
        QString n = getString(e, "ui_name");
        if(n.isEmpty()) n = getString(e, "hi_name");
        if(n.isEmpty()) continue;
        out.push_back(n);
    }
    return out;
}

HighlightTable::HighlightTable(QObject *parent) : QObject(parent) {}

void HighlightTable::setDefaultColors(int rgbFg, int rgbBg, int rgbSp) {
    m_defaultFg = fromRgb24(rgbFg);
    m_defaultBg = fromRgb24(rgbBg);
    m_defaultSp = fromRgb24(rgbSp);
    emit changed();
}

void HighlightTable::defineAttr(int id, const msgpack::object &rgbAttr,
                                const msgpack::object *info) {
    HlAttr a;
    const int fg = getInt(rgbAttr, "foreground");
    const int bg = getInt(rgbAttr, "background");
    const int sp = getInt(rgbAttr, "special");
    if(fg >= 0) a.fg = fromRgb24(fg);
    if(bg >= 0) a.bg = fromRgb24(bg);
    if(sp >= 0) a.sp = fromRgb24(sp);
    a.bold = getBool(rgbAttr, "bold");
    a.italic = getBool(rgbAttr, "italic");
    a.underline = getBool(rgbAttr, "underline");
    a.undercurl = getBool(rgbAttr, "undercurl");
    a.strikethrough = getBool(rgbAttr, "strikethrough");
    a.reverse = getBool(rgbAttr, "reverse");
    a.blend = getInt(rgbAttr, "blend", 0);
    if(info) a.names = namesFromInfo(*info);
    a.isRounded = std::ranges::any_of(
        a.names, [this](const QString &n) { return m_roundedHighlights.contains(n); });
    m_attrs[id] = a;
    if(info && info->type == msgpack::type::ARRAY) {
        const auto &entries = info->via.array;
        for(uint32_t i = 0; i < entries.size; ++i) {
            for(const char *key: { "hi_name", "ui_name" }) {
                const int keyIndex = findKey(entries.ptr[i], key);
                if(keyIndex < 0) continue;
                const auto &value = entries.ptr[i].via.map.ptr[keyIndex].val;
                if(value.type != msgpack::type::STR) continue;
                const std::string_view name(value.via.str.ptr, value.via.str.size);
                for(size_t role = 0; role < kRequiredNames.size(); ++role) {
                    if(name == kRequiredNames.at(role)) m_requiredAttrIds.at(role) = id;
                }
            }
        }
    }
    emit changed();
}

void HighlightTable::clear() {
    m_attrs.clear();
    m_defaultBg = QColor();
    m_defaultFg = QColor();
    m_defaultSp = QColor();
    m_requiredAttrIds.fill(std::nullopt);
    emit changed();
}

bool HighlightTable::contains(int id) const { return id == 0 || m_attrs.contains(id); }

std::optional<int> HighlightTable::requiredAttrId(RequiredHighlight role) const {
    return m_requiredAttrIds.at(static_cast<size_t>(role));
}

void HighlightTable::setHighlightGroup(const QString &name, int id) {
    if(id < 0) {
        qWarning("Invalid hl_group_set: highlight id must be non-negative");
        return;
    }
    for(size_t role = 0; role < kRequiredNames.size(); ++role) {
        const auto expected = kRequiredNames.at(role);
        if(name != QLatin1StringView(expected.data(), static_cast<qsizetype>(expected.size())))
            continue;
        if(m_requiredAttrIds.at(role) == id) return;
        m_requiredAttrIds.at(role) = id;
        emit changed();
        return;
    }
}

void HighlightTable::setRoundedHighlights(const QStringList &names) {
    QSet<QString> next(names.cbegin(), names.cend());
    if(next == m_roundedHighlights) return;
    m_roundedHighlights = std::move(next);
    for(auto &kv: m_attrs) {
        HlAttr &a = kv.second;
        a.isRounded = std::ranges::any_of(
            a.names, [this](const QString &n) { return m_roundedHighlights.contains(n); });
    }
    emit changed();
}

HlAttr HighlightTable::attr(int id) const {
    const auto it = m_attrs.find(id);
    if(it == m_attrs.end()) return {};
    return it->second;
}

bool HighlightTable::isRounded(int id) const {
    const auto it = m_attrs.find(id);
    if(it == m_attrs.end()) return false;
    return it->second.isRounded;
}

HlAttr HighlightTable::resolved(int id) const {
    HlAttr a = attr(id);
    if(!a.fg.isValid()) a.fg = m_defaultFg;
    if(!a.bg.isValid()) a.bg = m_defaultBg;
    if(!a.sp.isValid()) a.sp = m_defaultSp;
    if(a.reverse) std::swap(a.fg, a.bg);
    return a;
}

} // namespace dot
