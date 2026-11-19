package com.dot.companion

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.dot.companion.model.SessionInfo
import com.dot.companion.net.AhpConnection
import com.dot.companion.net.CatalogHost
import com.dot.companion.net.HostCatalog
import com.dot.companion.net.HostConnection

class ChatViewModel(
    connectionFactory: (String) -> HostConnection = { AhpConnection(it, AndroidChatLogger) },
    fetchHosts: suspend (String) -> List<CatalogHost> = { HostCatalog().fetch(it) },
) : ViewModel() {
    private val client = ChatClient(viewModelScope, connectionFactory, fetchHosts)

    val connectionState = client.connectionState
    val endpoint = client.endpoint
    val messages = client.messages
    val selectedSession = client.selectedSession
    val sessions = client.sessions

    fun connect() = client.connect()

    fun selectSession(session: SessionInfo) = client.selectSession(session)

    fun send(text: String) = client.send(text)

    fun setEndpoint(value: String) = client.setEndpoint(value)

    fun switchSession() = client.switchSession()

    override fun onCleared() = client.close()
}
