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
    color: "#1e1e1e"

    // Target grid size requested by Component.onCompleted. Set once; the
    // flush-listener below compares against this to decide when to show.
    property int _requestedCols: 0
    property int _requestedRows: 0
    property bool _shown: false

    Component.onCompleted: {
        _recomputeTarget()
        _maybeResize()
        _syncTitleBar()
    }

    // Recompute target (cols, rows) from the current window and cell dimensions.
    // Called at startup and again if guifont changes before the window is shown.
    function _recomputeTarget() {
        const cw = Math.max(1, shell.cellWidth)
        const ch = Math.max(1, shell.cellHeight)
        const cols = Math.max(80, Math.floor(width  / cw))
        const rows = Math.max(24, Math.floor(height / ch))
        _requestedCols = cols
        _requestedRows = rows
    }

    // main.cpp fires nvim_ui_attach(80, 24) early (before this engine is
    // constructed) so the handshake overlaps with QML cold load. By the time
    // this runs `$connector.attached` is usually true; if it isn't yet (slow
    // RPC, or a test that didn't pre-attach), fall back to attachUi here.
    // The onAttachedChanged Connections below re-runs this when attach lands
    // late, which fires tryResize against an already-attached UI.
    property bool _attachIssued: false
    function _maybeResize() {
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
        if (_requestedCols === 0 || _requestedRows === 0) return
        const g = $connector.grid
        const gc = g.gridCols(1)
        const gr = g.gridRows(1)
        if (gc === _requestedCols && gr === _requestedRows) {
            _shown = true
            const cw = shell.cellWidth
            const ch = shell.cellHeight
            if (cw > 0 && ch > 0) {
                window.width  = _requestedCols * cw
                window.height = _requestedRows * ch
            }
            window.visible = true
        }
    }

    // nvim changed grid 1's size itself (:set columns/lines). Resize the window
    // client area to fit exactly, in the nvim->GUI direction — the inverse of
    // _recomputeTarget (which derives the grid FROM the window). dockW mirrors
    // the F11 handler: Shell ends at chatPanel.left, so the grid needs
    // cols*cellWidth of Shell width, not window width.
    function _applyNvimSize(cols, rows) {
        if (!_shown) return
        const cw = shell.cellWidth
        const ch = shell.cellHeight
        if (cw <= 0 || ch <= 0) return
        const dockW = chatPanel.visible ? chatPanel.width : 0
        window.width  = cols * cw + dockW
        window.height = rows * ch
    }

    Connections {
        target: $connector
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
            if (!_shown) {
                _shown = true
                const g = $connector.grid
                const gc = g.gridCols(1)
                const gr = g.gridRows(1)
                const cw = shell.cellWidth
                const ch = shell.cellHeight
                if (gc > 0 && gr > 0 && cw > 0 && ch > 0) {
                    window.width  = gc * cw
                    window.height = gr * ch
                }
                window.visible = true
            }
        }
    }

    function _syncTitleBar() {
        if (typeof $windowChrome === "undefined") return
        const bg = $connector.defaultBackground
        if (!bg || !bg.valid) return
        $windowChrome.applyToWindow(window, bg)
    }

    Connections {
        target: $connector
        function onDefaultBackgroundChanged() { _syncTitleBar() }
    }

    Shell {
        id: shell
        objectName: "shell"
        anchors.left: parent.left
        anchors.right: chatPanel.visible ? chatPanel.left : parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }

    // Chat panel — docked on the right, hidden by default. Toggling `visible`
    // changes Shell's width, which fires GridItem::geometryChange ->
    // maybeResizeUi -> requestResize (the existing tryResize path — geometry is
    // never poked directly). Width is clamped so a narrow window never starves
    // the grid to <=0 width (which would suppress the resize entirely).
    ChatPanel {
        id: chatPanel
        metrics: shell.metrics
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: visible ? Math.min(360, Math.floor(parent.width * 0.5)) : 0
        visible: false
        onClosed: shell.focusGrid()
    }

    // Always-visible toggle button (a small speech-bubble icon) in the app's
    // top strip. Floating overlay so it never perturbs Shell geometry or the
    // boot resize sequence. Not in the native OS caption (that would need a
    // frameless-window rewrite).
    Item {
        id: chatToggle
        objectName: "chatToggle"
        width: metrics.rowHeight + 2 * padding
        height: width
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 6
        anchors.topMargin: 3
        z: 150

        readonly property real cornerRadius: metrics.cornerRadius
        readonly property DesktopMetrics metrics: shell.metrics
        readonly property real padding: metrics.padding

        Accessible.name: chatPanel.visible ? qsTr("Close chat") : qsTr("Open chat")
        Accessible.role: Accessible.Button
        Accessible.onPressAction: toggle()

        function toggle() {
            if (chatPanel.visible) chatPanel.requestClose()
            else chatPanel.open()
        }

        Canvas {
            id: chatIcon
            objectName: "chatIcon"
            anchors.fill: parent
            property color tint: chatPanel.visible ? "#5a9fd4"
                               : (toggleArea.containsMouse ? "#e0e0e0" : "#9d9d9d")
            readonly property real cornerRadius: chatToggle.cornerRadius
            readonly property real padding: chatToggle.padding
            readonly property real strokeWidth: Math.min(1.5, symbolSize)
            readonly property real symbolSize: Math.max(0, Math.min(width - 2 * padding, height - 2 * padding,
                                                                   chatToggle.metrics.textFont.pixelSize * 6 / 7))

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

        MouseArea {
            id: toggleArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: chatToggle.toggle()
        }
    }

    Shortcut {
        sequence: "F11"
        context: Qt.WindowShortcut
        onActivated: {
            const willBeFullscreen = window.visibility !== Window.FullScreen
            if (willBeFullscreen) {
                // Pre-issue tryResize so nvim resizes the grid IN PARALLEL with
                // the OS fullscreen animation. Otherwise the user sees the
                // window expand instantly but the grid lag by the RPC RTT.
                const scr = window.screen
                if (scr) {
                    const cw = Math.max(1, shell.cellWidth)
                    const ch = Math.max(1, shell.cellHeight)
                    const dockW = chatPanel.visible ? chatPanel.width : 0
                    const cols = Math.max(1, Math.floor((scr.width - dockW) / cw))
                    const rows = Math.max(1, Math.floor(scr.height / ch))
                    $connector.tryResize(cols, rows)
                }
                window.visibility = Window.FullScreen
            } else {
                window.visibility = Window.Windowed
            }
        }
    }
}
