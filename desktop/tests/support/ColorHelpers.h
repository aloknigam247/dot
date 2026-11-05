#pragma once

#include "NvimConnector.h"

#include <initializer_list>

namespace dot::test {

inline constexpr auto chromeRoles = std::to_array<const char *>(
    { "DotAccent", "DotChanged", "DotError", "DotFrame", "DotInformation", "DotMutedText",
      "DotRaised", "DotResting", "DotSelectedText", "DotSelection", "DotSuccess", "DotText",
      "DotWarning" });

inline msgpack::object_handle chromeAttributes(int foreground = 0xabcdef, int background = 0x123456,
                                               bool exists = true, bool reverse = false) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_map(4);
    packer.pack("bg");
    if(background < 0) packer.pack_nil();
    else packer.pack(background);
    packer.pack("exists");
    packer.pack(exists);
    packer.pack("fg");
    if(foreground < 0) packer.pack_nil();
    else packer.pack(foreground);
    packer.pack("reverse");
    packer.pack(reverse);
    return msgpack::unpack(buffer.data(), buffer.size());
}

inline msgpack::object_handle chromeSnapshot(const char *background = "dark",
                                             const PackFn &roles = {}) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_map(2);
    packer.pack("background");
    packer.pack(background);
    packer.pack("roles");
    if(roles) {
        roles(packer);
    } else {
        const auto attributes = chromeAttributes();
        packer.pack_map(static_cast<uint32_t>(chromeRoles.size()));
        for(const char *role: chromeRoles) {
            packer.pack(role);
            packer.pack(attributes.get());
        }
    }
    return msgpack::unpack(buffer.data(), buffer.size());
}

inline msgpack::object_handle colorAttrs(int foreground, int background, bool reverse = false,
                                         bool underline = false) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_map(4);
    packer.pack("background");
    packer.pack(background);
    packer.pack("foreground");
    packer.pack(foreground);
    packer.pack("reverse");
    packer.pack(reverse);
    packer.pack("underline");
    packer.pack(underline);
    return msgpack::unpack(buffer.data(), buffer.size());
}

inline msgpack::object_handle cursorModes(std::initializer_list<int> ids) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_array(static_cast<uint32_t>(ids.size()));
    for(const int id: ids) {
        packer.pack_map(2);
        packer.pack("attr_id");
        packer.pack(id);
        packer.pack("cursor_shape");
        packer.pack("block");
    }
    return msgpack::unpack(buffer.data(), buffer.size());
}

inline void seedColorState(NvimConnector &conn, int foreground = 0xffffff, int background = 0) {
    const auto chrome = chromeSnapshot();
    const auto modes = cursorModes({ 0 });
    conn.colorPalette()->stageChromeSnapshot(chrome.get());
    conn.highlights()->setDefaultColors(foreground, background, foreground);
    conn.highlights()->setHighlightGroup(QStringLiteral("NormalFloat"), 0);
    conn.highlights()->setHighlightGroup(QStringLiteral("Pmenu"), 0);
    conn.highlights()->setHighlightGroup(QStringLiteral("Visual"), 0);
    conn.modeInfo()->setModes(modes.get(), true);
    conn.modeInfo()->setCurrentMode(QStringLiteral("normal"), 0);
    conn.colorPalette()->publishContent();
}

inline bool colorNotification(NvimConnector &conn, quint64 generation,
                              const msgpack::object &snapshot) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_array(3);
    packer.pack(2);
    packer.pack("dot_colors");
    packer.pack_array(2);
    packer.pack(generation);
    packer.pack(snapshot);
    const Notification note{
        QStringLiteral("dot_colors"),
        std::make_shared<msgpack::object_handle>(msgpack::unpack(buffer.data(), buffer.size())),
    };
    return QMetaObject::invokeMethod(&conn, "onNotification", Qt::DirectConnection,
                                     Q_ARG(dot::Notification, note));
}

inline bool redrawEvent(NvimConnector &conn, const char *name, const PackFn &arguments) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_array(3);
    packer.pack(2);
    packer.pack("redraw");
    packer.pack_array(1);
    packer.pack_array(2);
    packer.pack(name);
    arguments(packer);
    const Notification note{
        QStringLiteral("redraw"),
        std::make_shared<msgpack::object_handle>(msgpack::unpack(buffer.data(), buffer.size())),
    };
    return QMetaObject::invokeMethod(&conn, "onNotification", Qt::DirectConnection,
                                     Q_ARG(dot::Notification, note));
}

} // namespace dot::test
