import QtQuick

NeutralControl {
    property bool iconOnly: false

    implicitWidth: {
        if (iconOnly) return implicitHeight
        let contentWidth = 0
        for (const item of _content.children)
            contentWidth = Math.max(contentWidth, item.implicitWidth)
        return Math.max(implicitHeight, contentWidth + 2 * padding)
    }
    transparentRest: iconOnly
}
