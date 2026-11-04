#include "ColorPalette.h"

#include "HighlightTable.h"
#include "ModeInfo.h"

#include <algorithm>
#include <utility>
#include <QDebug>

namespace dot {

constinit const std::array<QLatin1StringView, ColorPalette::kRoleCount> ColorPalette::kRoleNames{
    QLatin1StringView("DotAccent"),       QLatin1StringView("DotChanged"),
    QLatin1StringView("DotError"),        QLatin1StringView("DotFrame"),
    QLatin1StringView("DotInformation"),  QLatin1StringView("DotMutedText"),
    QLatin1StringView("DotRaised"),       QLatin1StringView("DotResting"),
    QLatin1StringView("DotSelectedText"), QLatin1StringView("DotSelection"),
    QLatin1StringView("DotSuccess"),      QLatin1StringView("DotText"),
    QLatin1StringView("DotWarning"),
};

ColorPalette::ColorPalette(HighlightTable &highlights, ModeInfo &mode, QObject *parent) :
    QObject(parent), m_highlights(highlights), m_mode(mode) {
    const auto markDirty = [this] { m_contentDirty = true; };
    connect(&highlights, &HighlightTable::changed, this, markDirty);
    connect(&mode, &ModeInfo::currentChanged, this, markDirty);
}

bool ColorPalette::chromeValid() const {
    return !m_snapshot.chrome.theme.isEmpty() &&
           std::ranges::all_of(m_snapshot.chrome.colors, &QColor::isValid);
}

void ColorPalette::invalidateChrome(const QString &error) {
    m_pendingChrome.reset();
    ContentSnapshot next = m_snapshot;
    next.chrome = {};
    next.chrome.errors.append(QStringLiteral("chrome: ") + error);
    commitContent(next);
}

void ColorPalette::stageChromeSnapshot(const msgpack::object &snapshot) {
    auto parsed = parseChromeSnapshot(snapshot);
    if(parsed) {
        m_pendingChrome = std::move(*parsed);
    } else {
        m_pendingChrome = ChromeDefinition{};
        m_pendingChrome->error = QStringLiteral("chrome: invalid snapshot: ") + parsed.error();
    }
    m_contentDirty = true;
}

std::expected<ColorPalette::ChromeDefinition, QString>
    ColorPalette::parseChromeSnapshot(const msgpack::object &snapshot) {
    ChromeDefinition next;
    const msgpack::object *roles = nullptr;
    std::bitset<kRoleCount> seen;
    const auto fail = [](const QString &message) { return std::unexpected(message); };
    if(snapshot.type != msgpack::type::MAP || snapshot.via.map.size != 2)
        return fail(QStringLiteral("expected background and roles"));
    for(uint32_t i = 0; i < snapshot.via.map.size; ++i) {
        const auto &entry = snapshot.via.map.ptr[i];
        if(entry.key.type != msgpack::type::STR)
            return fail(QStringLiteral("field names must be strings"));
        const QString name = QString::fromUtf8(entry.key.via.str.ptr, entry.key.via.str.size);
        if(name == QStringLiteral("background") && next.theme.isEmpty()) {
            if(entry.val.type != msgpack::type::STR)
                return fail(QStringLiteral("background must be dark or light"));
            next.theme = QString::fromUtf8(entry.val.via.str.ptr, entry.val.via.str.size);
            if(next.theme != QStringLiteral("dark") && next.theme != QStringLiteral("light"))
                return fail(QStringLiteral("background must be dark or light"));
        } else if(name == QStringLiteral("roles") && !roles) {
            roles = &entry.val;
        } else {
            return fail(QStringLiteral("unexpected or duplicate field %1").arg(name));
        }
    }
    if(!roles || roles->type != msgpack::type::MAP)
        return fail(QStringLiteral("roles must be a map"));
    for(uint32_t i = 0; i < roles->via.map.size; ++i) {
        const auto &entry = roles->via.map.ptr[i];
        if(entry.key.type != msgpack::type::STR)
            return fail(QStringLiteral("role names must be strings"));
        const QString name = QString::fromUtf8(entry.key.via.str.ptr, entry.key.via.str.size);
        const auto found = std::ranges::find(kRoleNames, name);
        if(found == kRoleNames.end()) return fail(QStringLiteral("unknown role %1").arg(name));
        const auto index = static_cast<size_t>(found - kRoleNames.begin());
        if(seen.test(index)) return fail(QStringLiteral("duplicate role %1").arg(name));
        seen.set(index);
        if(entry.val.type != msgpack::type::MAP || entry.val.via.map.size != 4)
            return fail(QStringLiteral("%1 requires bg, exists, fg, and reverse").arg(name));
        auto &attr = next.attributes.at(index);
        QStringList fields;
        for(uint32_t j = 0; j < entry.val.via.map.size; ++j) {
            const auto &field = entry.val.via.map.ptr[j];
            if(field.key.type != msgpack::type::STR)
                return fail(QStringLiteral("%1 field names must be strings").arg(name));
            const QString key = QString::fromUtf8(field.key.via.str.ptr, field.key.via.str.size);
            if(fields.contains(key))
                return fail(QStringLiteral("%1 duplicate field %2").arg(name, key));
            fields.append(key);
            if(key == QStringLiteral("bg") || key == QStringLiteral("fg")) {
                if(field.val.type == msgpack::type::NIL) continue;
                if(field.val.type != msgpack::type::POSITIVE_INTEGER ||
                   field.val.via.u64 > 0xffffff)
                    return fail(QStringLiteral("%1 %2 must be an RGB color or nil").arg(name, key));
                const QColor color = QColor::fromRgb(static_cast<QRgb>(field.val.via.u64));
                if(key == QStringLiteral("bg")) attr.background = color;
                else attr.foreground = color;
            } else if(key == QStringLiteral("exists") || key == QStringLiteral("reverse")) {
                if(field.val.type != msgpack::type::BOOLEAN)
                    return fail(QStringLiteral("%1 %2 must be a boolean").arg(name, key));
                if(key == QStringLiteral("exists")) attr.exists = field.val.via.boolean;
                else attr.reverse = field.val.via.boolean;
            } else {
                return fail(QStringLiteral("%1 unknown field %2").arg(name, key));
            }
        }
    }
    return next;
}

ColorPalette::ChromeSnapshot ColorPalette::resolveChrome(const ChromeDefinition &definition) const {
    ChromeSnapshot next;
    if(!definition.error.isEmpty()) {
        next.errors.append(definition.error);
        return next;
    }
    next.theme = definition.theme;
    for(size_t i = 0; i < kRoleCount; ++i) {
        const auto &attr = definition.attributes.at(i);
        const auto role = static_cast<Role>(i);
        if(!attr.exists) {
            next.errors.append(QStringLiteral("chrome: missing %1").arg(kRoleNames.at(i)));
            continue;
        }
        QColor background = attr.background.isValid() ? attr.background : m_highlights.defaultBg();
        QColor foreground = attr.foreground.isValid() ? attr.foreground : m_highlights.defaultFg();
        if(attr.reverse) std::swap(background, foreground);
        const bool surface = role == Role::Frame || role == Role::Raised || role == Role::Resting ||
                             role == Role::Selection;
        next.colors.at(i) = surface ? background : foreground;
        if(!next.colors.at(i).isValid()) {
            next.errors.append(QStringLiteral("chrome: %1 missing %2")
                                   .arg(kRoleNames.at(i), surface ? QStringLiteral("background")
                                                                  : QStringLiteral("foreground")));
        }
    }
    return next;
}

void ColorPalette::publishContent() {
    if(!m_contentDirty) return;
    m_contentDirty = false;

    ContentSnapshot next;
    if(m_pendingChrome) {
        next.chrome = resolveChrome(*m_pendingChrome);
        m_pendingChrome.reset();
    } else {
        next.chrome = m_snapshot.chrome;
    }

    const auto missing = [&next](Error error) { next.errors.set(static_cast<size_t>(error)); };
    const auto requirePair = [&missing](Pair &pair, Error foreground, Error background) {
        if(!pair.foreground.isValid()) missing(foreground);
        if(!pair.background.isValid()) missing(background);
        if(!pair.isValid()) pair = {};
    };
    next.pane = { m_highlights.defaultBg(), m_highlights.defaultFg() };
    requirePair(next.pane, Error::PaneForeground, Error::PaneBackground);

    if(!m_mode.currentModeValid()) {
        missing(Error::CursorMode);
    } else if(!m_mode.currentHighlightValid()) {
        missing(Error::CursorId);
    } else if(!m_highlights.contains(m_mode.attrId())) {
        missing(Error::CursorAttribute);
    } else {
        if(m_mode.attrId() == 0) {
            next.cursor = { m_highlights.defaultFg(), m_highlights.defaultBg() };
        } else {
            const HlAttr attr = m_highlights.resolved(m_mode.attrId());
            next.cursor = { attr.bg, attr.fg };
        }
        requirePair(next.cursor, Error::CursorForeground, Error::CursorBackground);
    }

    const auto resolveGroup = [&](RequiredHighlight role, Pair &pair, Error association,
                                  Error attribute, Error foreground, Error background) {
        const auto id = m_highlights.requiredAttrId(role);
        if(!id) {
            missing(association);
            return;
        }
        if(!m_highlights.contains(*id)) {
            missing(attribute);
            return;
        }
        const HlAttr attr = m_highlights.resolved(*id);
        pair = { attr.bg, attr.fg };
        requirePair(pair, foreground, background);
    };
    resolveGroup(RequiredHighlight::Pmenu, next.menu, Error::MenuAssociation, Error::MenuAttribute,
                 Error::MenuForeground, Error::MenuBackground);
    resolveGroup(RequiredHighlight::NormalFloat, next.overlay, Error::OverlayAssociation,
                 Error::OverlayAttribute, Error::OverlayForeground, Error::OverlayBackground);
    resolveGroup(RequiredHighlight::Visual, next.textSelection, Error::SelectionAssociation,
                 Error::SelectionAttribute, Error::SelectionForeground, Error::SelectionBackground);
    commitContent(next);
}

void ColorPalette::resetContent() {
    ContentSnapshot empty;
    m_pendingChrome.reset();
    for(const Error error: {
            Error::PaneForeground,
            Error::PaneBackground,
            Error::CursorMode,
            Error::MenuAssociation,
            Error::OverlayAssociation,
            Error::SelectionAssociation,
        }) {
        empty.errors.set(static_cast<size_t>(error));
    }
    m_contentDirty = true;
    commitContent(empty);
}

void ColorPalette::commitContent(const ContentSnapshot &snapshot) {
    static const std::array messages{
        QStringLiteral("pane: missing foreground"),
        QStringLiteral("pane: missing background"),
        QStringLiteral("cursor: missing current mode"),
        QStringLiteral("cursor: missing highlight id"),
        QStringLiteral("cursor: missing highlight attribute"),
        QStringLiteral("cursor: missing foreground"),
        QStringLiteral("cursor: missing background"),
        QStringLiteral("menu: missing Pmenu association"),
        QStringLiteral("menu: missing highlight attribute"),
        QStringLiteral("menu: missing foreground"),
        QStringLiteral("menu: missing background"),
        QStringLiteral("overlay: missing NormalFloat association"),
        QStringLiteral("overlay: missing highlight attribute"),
        QStringLiteral("overlay: missing foreground"),
        QStringLiteral("overlay: missing background"),
        QStringLiteral("text selection: missing Visual association"),
        QStringLiteral("text selection: missing highlight attribute"),
        QStringLiteral("text selection: missing foreground"),
        QStringLiteral("text selection: missing background"),
    };
    static_assert(messages.size() == static_cast<size_t>(Error::Count));
    if(snapshot == m_snapshot) return;
    QStringList newErrors;
    if(snapshot.errors != m_snapshot.errors || snapshot.chrome.errors != m_snapshot.chrome.errors) {
        QStringList errors;
        errors.reserve(static_cast<qsizetype>(snapshot.errors.count()) +
                       snapshot.chrome.errors.size());
        for(size_t i = 0; i < messages.size(); ++i) {
            if(snapshot.errors.test(i)) errors.append(messages.at(i));
        }
        errors.append(snapshot.chrome.errors);
        for(const auto &error: errors) {
            if(!m_resolutionErrors.contains(error)) newErrors.append(error);
        }
        m_resolutionErrors = std::move(errors);
    }
    m_snapshot = snapshot;
    emit paletteChanged();
    for(const auto &error: newErrors) qCritical().noquote() << "ColorPalette:" << error;
}

} // namespace dot
