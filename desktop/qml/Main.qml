import QtQuick
import QtQuick.Window
import Dot 1.0

Window {
    id: window
    width: 1200
    height: 780
    // Start hidden. main.cpp fires nvim_ui_attach(80, 24) before this engine
    // is constructed, so the very first redraw flush paints at the 80x24
    // placeholder size. We flip visible=true only after the post-attach
    // tryResize has reached the real geometry — otherwise the user sees the
    // window pop up at 80x24 and visibly jump to 1200x780 a moment later.
    visible: false
    title: $connector.title || "dot"
    color: _startupFailed ? systemPalette.window : $connector.colorPalette.frame

    SystemPalette {
        id: systemPalette
    }

    // Keep the first frame hidden until Neovim flushes the current startup target.
    property int _requestedCols: 0
    property int _requestedRows: 0
    property bool _shown: false
    property bool _showSafetyExpired: false
    readonly property bool _startupFailed: $connector.startupError.length > 0

    Component.onCompleted: {
        if (_startupFailed) {
            _syncTitleBar()
            Qt.callLater(function() { window._showStartupFailure() })
        } else {
            _recomputeTarget()
            _maybeResize()
            _syncTitleBar()
        }
    }

    // Recompute target (cols, rows) from the current window and cell dimensions.
    // Called at startup and again if guifont changes before the window is shown.
    function _recomputeTarget() {
        const cells = shell.metrics.cellsForWindow(window.width, window.height, chatPanel.visible)
        _requestedCols = cells.width
        _requestedRows = cells.height
    }

    // main.cpp fires nvim_ui_attach(80, 24) early (before this engine is
    // constructed) so the handshake overlaps with QML cold load. By the time
    // this runs `$connector.attached` is usually true; if it isn't yet (slow
    // RPC, or a test that didn't pre-attach), fall back to attachUi here.
    // The onAttachedChanged Connections below re-runs this when attach lands
    // late, which fires tryResize against an already-attached UI.
    property bool _attachIssued: false
    function _maybeResize() {
        if (_startupFailed) return
        if (_requestedCols === 0 || _requestedRows === 0) return
        if ($connector.attached) {
            $connector.tryResize(_requestedCols, _requestedRows)
            _showSafetyTimer.restart()
        } else if (!_attachIssued) {
            _attachIssued = true
            $connector.attachUi(_requestedCols, _requestedRows)
        }
    }

    function _showIfReady() {
        if (_shown) return
        if (_startupFailed) return
        if (!$connector.colorPalette.chromeValid || !$connector.colorPalette.paneValid) return
        if (_requestedCols === 0 || _requestedRows === 0) return
        const g = $connector.grid
        const gc = g.gridCols(1)
        const gr = g.gridRows(1)
        if (gc > 0 && gr > 0 && (_showSafetyExpired || (gc === _requestedCols && gr === _requestedRows))) {
            _shown = true
            _applyNvimSize(gc, gr)
            window.visible = true
        }
    }

    function _showStartupFailure() {
        if (_shown || !_startupFailed) return
        _showSafetyTimer.stop()
        _shown = true
        window.visible = true
    }

    // Invert the pane fit rather than adding the current chat width: a smaller
    // requested window can cross the dock's half-window allocation threshold.
    function _applyNvimSize(cols, rows) {
        if (!_shown) return
        const size = shell.metrics.windowSizeForGrid(cols, rows, chatPanel.visible)
        window.width = size.width
        window.height = size.height
    }

    function _toggleFullScreen() {
        if (window.visibility === Window.FullScreen) {
            window.visibility = Window.Windowed
            return
        }
        // Fit Neovim in parallel with the OS fullscreen transition.
        const scr = window.screen
        if (scr) {
            const cells = shell.metrics.cellsForWindow(scr.width, scr.height, chatPanel.visible)
            $connector.tryResize(cells.width, cells.height)
        }
        window.visibility = Window.FullScreen
    }

    Connections {
        target: $connector
        function onStartupErrorChanged() {
            _syncTitleBar()
            if ($connector.startupError.length > 0) {
                Qt.callLater(function() { window._showStartupFailure() })
            }
        }
        // When attach lands after Component.onCompleted (engine cold-loaded
        // faster than nvim handshake), fire the deferred tryResize — but only
        // during the initial boot show. attachedChanged also fires on a :restart
        // re-attach (which already re-attaches at the live size); re-running the
        // boot resize there would clobber it with the stale boot-time request.
        function onAttachedChanged() { if (!_shown) _maybeResize() }
        // The first flush whose grid 1 matches the requested geometry is
        // the cue: at this point nvim has resized AND repainted at the real
        // size, so the very first frame Qt swaps will be correct.
        function onFlush() { _showIfReady() }
        // nvim resized grid 1 itself (e.g. :set columns/lines) — match the
        // window client area to it. NvimConnector only emits this for sizes it
        // didn't request, so GUI-drag acks never reach here.
        function onNvimRequestedResize(cols, rows) { _applyNvimSize(cols, rows) }
    }

    // React to cellWidthChanged (not guifontChanged) because GridItem
    // re-measures AFTER the guifontChanged signal — at that moment
    // shell.cellWidth is still the old value.
    Connections {
        target: shell
        function onCellWidthChanged() {
            if (_shown) return
            _recomputeTarget()
            _maybeResize()
        }
    }

    Timer {
        id: _showSafetyTimer
        interval: 1000
        repeat: false
        onTriggered: {
            _showSafetyExpired = true
            _showIfReady()
        }
    }

    function _syncTitleBar() {
        if (typeof $windowChrome === "undefined") return
        if (_startupFailed) {
            $windowChrome.applyToWindow(window, systemPalette.window)
            return
        }
        if (!$connector.colorPalette.chromeValid) return
        $windowChrome.applyToWindow(window, $connector.colorPalette.frame)
    }

    Connections {
        target: $connector.colorPalette
        function onPaletteChanged() {
            _syncTitleBar()
            _showIfReady()
        }
    }

    Loader {
        id: startupErrorLoader
        anchors.fill: parent
        active: window._startupFailed
        z: 200

        sourceComponent: Component {
            Rectangle {
                color: systemPalette.window

                Flickable {
                    id: startupErrorFlickable
                    anchors.fill: parent
                    anchors.margins: 32
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    contentHeight: startupErrorText.implicitHeight
                    contentWidth: width

                    Text {
                        id: startupErrorText
                        objectName: "startupErrorText"
                        width: startupErrorFlickable.width
                        color: systemPalette.windowText
                        font.pixelSize: 16
                        text: $connector.startupError
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere

                        Accessible.name: text
                        Accessible.role: Accessible.StaticText
                    }
                }
            }
        }
    }

    Rectangle {
        objectName: "editorPane"
        readonly property rect bounds: shell.metrics.editorPaneRect(window.width, window.height, chatPanel.visible)
        x: bounds.x
        y: bounds.y
        width: bounds.width
        height: bounds.height
        color: $connector.colorPalette.paneBackground
        opacity: $connector.colorPalette.paneValid ? 1 : 0
        radius: shell.metrics.paneCornerRadius
        visible: !window._startupFailed
    }

    Shell {
        id: shell
        objectName: "shell"
        readonly property rect bounds: metrics.gridRectForWindow(window.width, window.height, chatPanel.visible)
        x: bounds.x
        y: bounds.y
        width: bounds.width
        height: bounds.height
        enabled: visible
        opacity: $connector.colorPalette.paneValid ? 1 : 0
        visible: !window._startupFailed
    }

    ChatPanel {
        id: chatPanel
        readonly property rect bounds: metrics.chatPaneRect(window.width, window.height, chatPanel.visible)
        x: bounds.x
        y: bounds.y
        width: bounds.width
        height: bounds.height
        enabled: !window._startupFailed
        metrics: shell.metrics
        visible: false
        onClosed: shell.focusGrid()
    }

    NeutralButton {
        id: chatToggle
        objectName: "chatToggle"
        Accessible.name: chatPanel.visible ? qsTr("Close chat") : qsTr("Open chat")
        activeFocusOnTab: false
        enabled: visible
        iconOnly: true
        implicitHeight: metrics.topBandHeight
        metrics: shell.metrics
        selectable: true
        selected: chatPanel.visible
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: metrics.frameGap
        visible: !window._startupFailed
        z: 150

        onActivated: {
            if (chatPanel.visible) chatPanel.requestClose()
            else chatPanel.open()
        }

        Canvas {
            id: chatIcon
            objectName: "chatIcon"
            anchors.fill: parent
            property color tint: chatToggle.foreground
            readonly property real cornerRadius: chatToggle.cornerRadius
            readonly property real padding: chatToggle.padding
            readonly property real strokeWidth: Math.min(1.5, symbolSize)
            readonly property real symbolSize: Math.max(0, Math.min(width - 2 * padding, height - 2 * padding,
                                                                   chatToggle.metrics.iconSize))

            onHeightChanged: requestPaint()
            onSymbolSizeChanged: requestPaint()
            onTintChanged: requestPaint()
            onWidthChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = tint
                ctx.lineJoin = "round"
                ctx.lineWidth = strokeWidth
                const inset = strokeWidth / 2
                const bx = (width - symbolSize) / 2 + inset
                const by = (height - symbolSize) / 2 + inset
                const bw = Math.max(0, symbolSize - strokeWidth)
                const tailHeight = Math.min(chatToggle.padding, bw / 3)
                const bh = Math.max(0, bw - tailHeight)
                const r = Math.min(cornerRadius, bw / 2, bh / 2)
                const tailWidth = Math.min(chatToggle.padding, Math.max(0, bw - 2 * r))
                const tailX = Math.min(bw - r - tailWidth, Math.max(r, bw / 3))
                ctx.beginPath()
                ctx.moveTo(bx + r, by)
                ctx.lineTo(bx + bw - r, by)
                ctx.arcTo(bx + bw, by, bx + bw, by + r, r)
                ctx.lineTo(bx + bw, by + bh - r)
                ctx.arcTo(bx + bw, by + bh, bx + bw - r, by + bh, r)
                ctx.lineTo(bx + tailX + tailWidth, by + bh)
                ctx.lineTo(bx + tailX, by + bh + tailHeight)
                ctx.lineTo(bx + tailX, by + bh)
                ctx.lineTo(bx + r, by + bh)
                ctx.arcTo(bx, by + bh, bx, by + bh - r, r)
                ctx.lineTo(bx, by + r)
                ctx.arcTo(bx, by, bx + r, by, r)
                ctx.closePath()
                ctx.stroke()
            }
        }
    }

    Shortcut {
        sequence: "F11"
        context: Qt.WindowShortcut
        onActivated: window._toggleFullScreen()
    }
}
