package com.dot.companion.net

import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
import kotlinx.coroutines.flow.StateFlow

/**
 * The surface [com.dot.companion.ChatClient] drives on a single host connection:
 * observable connection state, diagnostic progress, the folded transcript, and the
 * discovered sessions, plus the lifecycle and session-selection commands. [AhpConnection]
 * is the live implementation; the seam lets the coordinator be unit-tested against a fake
 * without a real socket.
 */
interface HostConnection {
    val state: StateFlow<ConnectionState>
    val progress: StateFlow<HostProgress>
    val transcript: StateFlow<List<UiMessage>>
    val availableSessions: StateFlow<List<SessionInfo>>

    fun start()

    fun close()

    fun send(text: String): Boolean

    fun selectSession(resource: String)
}
