#include "ModeInfo.h"

#include <utility>
#include <QDebug>

namespace dot {

static QString mapKeyStr(const msgpack::object &mapObj, const char *key) {
    if(mapObj.type != msgpack::type::MAP) return {};
    const auto &m = mapObj.via.map;
    for(uint32_t i = 0; i < m.size; ++i) {
        if(m.ptr[i].key.type != msgpack::type::STR) continue;
        const auto &s = m.ptr[i].key.via.str;
        if(s.size != std::strlen(key) || std::memcmp(s.ptr, key, s.size) != 0) continue;
        const auto &v = m.ptr[i].val;
        if(v.type == msgpack::type::STR) return QString::fromUtf8(v.via.str.ptr, v.via.str.size);
        return {};
    }
    return {};
}

static int mapKeyInt(const msgpack::object &mapObj, const char *key, int def = 0) {
    if(mapObj.type != msgpack::type::MAP) return def;
    const auto &m = mapObj.via.map;
    for(uint32_t i = 0; i < m.size; ++i) {
        if(m.ptr[i].key.type != msgpack::type::STR) continue;
        const auto &s = m.ptr[i].key.via.str;
        if(s.size != std::strlen(key) || std::memcmp(s.ptr, key, s.size) != 0) continue;
        const auto &v = m.ptr[i].val;
        if(v.type == msgpack::type::POSITIVE_INTEGER && std::in_range<int>(v.via.u64))
            return static_cast<int>(v.via.u64);
        if(v.type == msgpack::type::NEGATIVE_INTEGER && std::in_range<int>(v.via.i64))
            return static_cast<int>(v.via.i64);
        return def;
    }
    return def;
}

static CursorShape parseShape(const QString &s) {
    if(s == QStringLiteral("horizontal")) return CursorShape::Horizontal;
    if(s == QStringLiteral("vertical")) return CursorShape::Vertical;
    return CursorShape::Block;
}

ModeInfo::ModeInfo(QObject *parent) : QObject(parent) {}

void ModeInfo::setModes(const msgpack::object &info, bool cursorStyleEnabled) {
    const bool styleChanged = m_cursorStyleEnabled != cursorStyleEnabled;
    m_cursorStyleEnabled = cursorStyleEnabled;
    m_modes.clear();
    if(info.type != msgpack::type::ARRAY) {
        qWarning("Invalid mode_info_set: mode_info must be an array");
    } else {
        const auto &arr = info.via.array;
        m_modes.reserve(arr.size);
        for(uint32_t i = 0; i < arr.size; ++i) {
            ModeDescriptor d;
            const auto &e = arr.ptr[i];
            const int attrId = mapKeyInt(e, "attr_id", -1);
            d.name = mapKeyStr(e, "name");
            d.shortName = mapKeyStr(e, "short_name");
            d.shape = parseShape(mapKeyStr(e, "cursor_shape"));
            d.cellPercentage = mapKeyInt(e, "cell_percentage", 100);
            d.blinkWait = mapKeyInt(e, "blinkwait", 0);
            d.blinkOn = mapKeyInt(e, "blinkon", 0);
            d.blinkOff = mapKeyInt(e, "blinkoff", 0);
            d.attrId = attrId >= 0 ? attrId : 0;
            d.highlightValid = attrId >= 0;
            d.valid = e.type == msgpack::type::MAP;
            m_modes.push_back(d);
        }
    }
    const ModeDescriptor current = currentDescriptor();
    if(styleChanged || current != m_current) {
        m_current = current;
        emit currentChanged();
    }
}

ModeDescriptor ModeInfo::currentDescriptor() const {
    ModeDescriptor descriptor;
    if(m_currentIndex >= 0 && m_currentIndex < m_modes.size()) {
        descriptor = m_modes[m_currentIndex];
    }
    if(descriptor.name.isEmpty()) descriptor.name = m_currentName;
    return descriptor;
}

void ModeInfo::setCurrentMode(const QString &name, int idx) {
    m_currentIndex = idx;
    m_currentName = name;
    m_current = currentDescriptor();
    emit currentChanged();
}

void ModeInfo::reset() {
    m_modes.clear();
    m_current = ModeDescriptor{};
    m_cursorStyleEnabled = false;
    m_currentIndex = -1;
    m_currentName.clear();
    emit currentChanged();
}

} // namespace dot
