package com.dot.companion

import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
import com.dot.companion.net.ConnectionState
import com.dot.companion.net.HostConnection
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ChatViewModelTest {
    private val store = ViewModelStore()

    @Before
    fun setUp() {
        Dispatchers.setMain(UnconfinedTestDispatcher())
    }

    @After
    fun tearDown() {
        store.clear()
        Dispatchers.resetMain()
    }

    @Test
    fun `adapter forwards every operation and flow and closes with its lifecycle`() {
        val created = mutableListOf<Pair<String, FakeConnection>>()
        val vm =
            ChatViewModel(
                connectionFactory = { endpoint -> FakeConnection().also { created += endpoint to it } },
                fetchHosts = { error("direct endpoint") },
            )
        store.put("chat", vm)

        vm.setEndpoint(" host:1 ")
        assertEquals("host:1", vm.endpoint.value)
        vm.connect()
        assertEquals(listOf("host:1"), created.map { it.first })
        val first = created.single().second
        assertTrue(first.started)

        first.state.value = ConnectionState.Connected
        first.availableSessions.value = listOf(SessionInfo("r1", "T1"))
        assertEquals(ConnectionState.Connected, vm.connectionState.value)
        assertEquals(listOf(SessionInfo("r1", "T1", hostId = "host:1")), vm.sessions.value)
        assertNull(vm.selectedSession.value)

        vm.selectSession(vm.sessions.value.single())
        assertEquals("r1", first.selected)
        assertEquals("r1", vm.selectedSession.value)
        val rows = listOf(UiMessage("m1", "assistant", "hi", streaming = true))
        first.transcript.value = rows
        assertSame(rows, vm.messages.value)
        vm.send("  hi  ")
        assertEquals(listOf("hi"), first.sent)

        vm.switchSession()
        assertEquals(2, created.size)
        val replacement = created.last().second
        assertTrue(first.closed)
        assertFalse(replacement.closed)
        assertTrue(replacement.started)
        assertNull(vm.selectedSession.value)
        assertTrue(vm.messages.value.isEmpty())

        store.clear()
        assertTrue(replacement.closed)
    }

    @Test
    fun `default construction wires real dependencies without connecting`() {
        val vm = ViewModelProvider(store, ViewModelProvider.NewInstanceFactory())[ChatViewModel::class.java]
        assertEquals(ConnectionState.Disconnected, vm.connectionState.value)
        assertEquals("", vm.endpoint.value)
        assertTrue(vm.messages.value.isEmpty())
        assertTrue(vm.sessions.value.isEmpty())
        assertNull(vm.selectedSession.value)
    }

    private class FakeConnection : HostConnection {
        var closed = false
        var selected: String? = null
        val sent = mutableListOf<String>()
        var started = false
        override val availableSessions = MutableStateFlow<List<SessionInfo>>(emptyList())
        override val state = MutableStateFlow(ConnectionState.Disconnected)
        override val transcript = MutableStateFlow<List<UiMessage>>(emptyList())

        override fun close() {
            closed = true
        }

        override fun selectSession(resource: String) {
            selected = resource
        }

        override fun send(text: String): Boolean = sent.add(text)

        override fun start() {
            started = true
        }
    }
}
