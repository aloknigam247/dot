#ifndef COLORPALETTE_H
#define COLORPALETTE_H

#include <bitset>
#include <QColor>
#include <QObject>
#include <qqmlregistration.h>
#include <QStringList>

namespace dot {

class Config;
class HighlightTable;
class ModeInfo;

class ColorPalette : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by NvimConnector")
    Q_PROPERTY(QColor accent READ accent NOTIFY paletteChanged)
    Q_PROPERTY(QColor changed READ changed NOTIFY paletteChanged)
    Q_PROPERTY(QColor cursorBackground READ cursorBackground NOTIFY paletteChanged)
    Q_PROPERTY(QColor cursorForeground READ cursorForeground NOTIFY paletteChanged)
    Q_PROPERTY(bool cursorValid READ cursorValid NOTIFY paletteChanged)
    Q_PROPERTY(QColor error READ error NOTIFY paletteChanged)
    Q_PROPERTY(QColor frame READ frame NOTIFY paletteChanged)
    Q_PROPERTY(QColor information READ information NOTIFY paletteChanged)
    Q_PROPERTY(QColor menuBackground READ menuBackground NOTIFY paletteChanged)
    Q_PROPERTY(QColor menuForeground READ menuForeground NOTIFY paletteChanged)
    Q_PROPERTY(bool menuValid READ menuValid NOTIFY paletteChanged)
    Q_PROPERTY(QColor mutedText READ mutedText NOTIFY paletteChanged)
    Q_PROPERTY(QColor overlayBackground READ overlayBackground NOTIFY paletteChanged)
    Q_PROPERTY(QColor overlayForeground READ overlayForeground NOTIFY paletteChanged)
    Q_PROPERTY(bool overlayValid READ overlayValid NOTIFY paletteChanged)
    Q_PROPERTY(QColor paneBackground READ paneBackground NOTIFY paletteChanged)
    Q_PROPERTY(bool paneValid READ paneValid NOTIFY paletteChanged)
    Q_PROPERTY(QColor primaryText READ primaryText NOTIFY paletteChanged)
    Q_PROPERTY(QColor raised READ raised NOTIFY paletteChanged)
    Q_PROPERTY(QStringList resolutionErrors READ resolutionErrors NOTIFY paletteChanged)
    Q_PROPERTY(QColor resting READ resting NOTIFY paletteChanged)
    Q_PROPERTY(QColor selectedText READ selectedText NOTIFY paletteChanged)
    Q_PROPERTY(QColor selection READ selection NOTIFY paletteChanged)
    Q_PROPERTY(QColor success READ success NOTIFY paletteChanged)
    Q_PROPERTY(QColor text READ text NOTIFY paletteChanged)
    Q_PROPERTY(QColor textSelectionBackground READ textSelectionBackground NOTIFY paletteChanged)
    Q_PROPERTY(QColor textSelectionForeground READ textSelectionForeground NOTIFY paletteChanged)
    Q_PROPERTY(bool textSelectionValid READ textSelectionValid NOTIFY paletteChanged)
    Q_PROPERTY(QString theme READ theme NOTIFY paletteChanged)
    Q_PROPERTY(QColor warning READ warning NOTIFY paletteChanged)

public:
    explicit ColorPalette(HighlightTable &highlights, ModeInfo &mode, QObject *parent = nullptr);

    void bindConfig(Config &config);
    void publishContent();
    static void registerConfig(Config &config);
    void resetContent();

    QColor accent() const { return QColor::fromRgb(fixedColors().accent); }
    QColor changed() const { return QColor::fromRgb(fixedColors().changed); }
    QColor cursorBackground() const { return m_snapshot.cursor.background; }
    QColor cursorForeground() const { return m_snapshot.cursor.foreground; }
    bool cursorValid() const { return m_snapshot.cursor.isValid(); }
    QColor error() const { return QColor::fromRgb(fixedColors().error); }
    QColor frame() const { return QColor::fromRgb(fixedColors().frame); }
    QColor information() const { return QColor::fromRgb(fixedColors().information); }
    QColor menuBackground() const { return m_snapshot.menu.background; }
    QColor menuForeground() const { return m_snapshot.menu.foreground; }
    bool menuValid() const { return m_snapshot.menu.isValid(); }
    QColor mutedText() const { return QColor::fromRgb(fixedColors().mutedText); }
    QColor overlayBackground() const { return m_snapshot.overlay.background; }
    QColor overlayForeground() const { return m_snapshot.overlay.foreground; }
    bool overlayValid() const { return m_snapshot.overlay.isValid(); }
    QColor paneBackground() const { return m_snapshot.pane.background; }
    bool paneValid() const { return m_snapshot.pane.isValid(); }
    QColor primaryText() const { return m_snapshot.pane.foreground; }
    QColor raised() const { return QColor::fromRgb(fixedColors().raised); }
    QStringList resolutionErrors() const { return m_resolutionErrors; }
    QColor resting() const { return QColor::fromRgb(fixedColors().resting); }
    QColor selectedText() const { return QColor::fromRgb(fixedColors().selectedText); }
    QColor selection() const { return QColor::fromRgb(fixedColors().selection); }
    QColor success() const { return QColor::fromRgb(fixedColors().success); }
    QColor text() const { return QColor::fromRgb(fixedColors().text); }
    QColor textSelectionBackground() const { return m_snapshot.textSelection.background; }
    QColor textSelectionForeground() const { return m_snapshot.textSelection.foreground; }
    bool textSelectionValid() const { return m_snapshot.textSelection.isValid(); }
    QString theme() const;
    QColor warning() const { return QColor::fromRgb(fixedColors().warning); }

signals:
    void paletteChanged();

private:
    enum class Error {
        PaneForeground,
        PaneBackground,
        CursorMode,
        CursorId,
        CursorAttribute,
        CursorForeground,
        CursorBackground,
        MenuAssociation,
        MenuAttribute,
        MenuForeground,
        MenuBackground,
        OverlayAssociation,
        OverlayAttribute,
        OverlayForeground,
        OverlayBackground,
        SelectionAssociation,
        SelectionAttribute,
        SelectionForeground,
        SelectionBackground,
        Count,
    };

    struct FixedColors {
        QRgb accent;
        QRgb changed;
        QRgb error;
        QRgb frame;
        QRgb information;
        QRgb mutedText;
        QRgb raised;
        QRgb resting;
        QRgb selectedText;
        QRgb selection;
        QRgb success;
        QRgb text;
        QRgb warning;
    };

    enum class Theme {
        Dark,
        Light,
    };

    using Errors = std::bitset<static_cast<size_t>(Error::Count)>;

    struct Pair {
        QColor background;
        QColor foreground;

        bool isValid() const { return background.isValid() && foreground.isValid(); }
        bool operator==(const Pair &) const = default;
    };

    struct ContentSnapshot {
        Pair cursor;
        Errors errors;
        Pair menu;
        Pair overlay;
        Pair pane;
        Pair textSelection;

        bool operator==(const ContentSnapshot &) const = default;
    };

    void commitContent(ContentSnapshot snapshot);
    const FixedColors &fixedColors() const;

    QMetaObject::Connection m_configConnection;
    bool m_contentDirty = true;
    HighlightTable &m_highlights;
    ModeInfo &m_mode;
    QStringList m_resolutionErrors;
    ContentSnapshot m_snapshot;
    Theme m_theme = Theme::Dark;
};

} // namespace dot

#endif
