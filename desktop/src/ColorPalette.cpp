#include "ColorPalette.h"

#include "Config.h"
#include "HighlightTable.h"
#include "ModeInfo.h"

#include <array>
#include <QDebug>

namespace dot {

ColorPalette::ColorPalette(HighlightTable &highlights, ModeInfo &mode, QObject *parent) :
    QObject(parent), m_highlights(highlights), m_mode(mode) {
    const auto markDirty = [this] { m_contentDirty = true; };
    connect(&highlights, &HighlightTable::changed, this, markDirty);
    connect(&mode, &ModeInfo::currentChanged, this, markDirty);
}

void ColorPalette::registerConfig(Config &config) {
    config.registerOption(QStringLiteral("theme"), ConfigType::String, QStringLiteral("dark"));
}

void ColorPalette::bindConfig(Config &config) {
    disconnect(m_configConnection);
    const auto applyTheme = [this, &config] {
        const QString value = config.value(QStringLiteral("theme")).toString();
        if(value != QStringLiteral("dark") && value != QStringLiteral("light")) {
            qWarning().noquote() << "ColorPalette: invalid theme" << value
                                 << "(expected dark or light)";
            return;
        }
        const Theme next = value == QStringLiteral("light") ? Theme::Light : Theme::Dark;
        if(next == m_theme) return;
        m_theme = next;
        emit paletteChanged();
    };
    applyTheme();
    m_configConnection =
        connect(&config, &Config::changed, this, [applyTheme](const QString &name) {
        if(name == QStringLiteral("theme")) applyTheme();
    });
}

QString ColorPalette::theme() const {
    return m_theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

const ColorPalette::FixedColors &ColorPalette::fixedColors() const {
    static constexpr FixedColors dark{
        .accent = 0xcba6f7,
        .changed = 0x89b4fa,
        .error = 0xf38ba8,
        .frame = 0x11111b,
        .information = 0x94e2d5,
        .mutedText = 0xbac2de,
        .raised = 0x45475a,
        .resting = 0x313244,
        .selectedText = 0x1e1e2e,
        .selection = 0xcba6f7,
        .success = 0xa6e3a1,
        .text = 0xcdd6f4,
        .warning = 0xf9e2af,
    };
    static constexpr FixedColors light{
        .accent = 0x8839ef,
        .changed = 0x1e66f5,
        .error = 0xd20f39,
        .frame = 0xdce0e8,
        .information = 0x179299,
        .mutedText = 0x5c5f77,
        .raised = 0xbcc0cc,
        .resting = 0xccd0da,
        .selectedText = 0xeff1f5,
        .selection = 0x8839ef,
        .success = 0x40a02b,
        .text = 0x4c4f69,
        .warning = 0xdf8e1d,
    };
    return m_theme == Theme::Dark ? dark : light;
}

void ColorPalette::publishContent() {
    if(!m_contentDirty) return;
    m_contentDirty = false;

    ContentSnapshot next;
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

void ColorPalette::commitContent(ContentSnapshot snapshot) {
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
    const Errors newErrors = snapshot.errors & ~m_snapshot.errors;
    if(snapshot.errors != m_snapshot.errors) {
        QStringList errors;
        errors.reserve(static_cast<qsizetype>(snapshot.errors.count()));
        for(size_t i = 0; i < messages.size(); ++i) {
            if(snapshot.errors.test(i)) errors.append(messages.at(i));
        }
        m_resolutionErrors = std::move(errors);
    }
    m_snapshot = snapshot;
    emit paletteChanged();
    for(size_t i = 0; i < messages.size(); ++i) {
        if(newErrors.test(i)) qCritical().noquote() << "ColorPalette:" << messages.at(i);
    }
}

} // namespace dot
