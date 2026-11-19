package com.dot.companion

import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
import com.dot.companion.net.CatalogHost
import com.dot.companion.net.ConnectionState
import com.dot.companion.net.HostConnection
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.withContext
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Drives [ChatClient]'s discovery, merge, selection, send and teardown logic against a
 * fake [HostConnection] and a stubbed host-catalog fetch, so no real socket is involved.
 * An [UnconfinedTestDispatcher] makes every launch in the supplied scope
 * (including StateFlow collectors) run eagerly, so state is observable synchronously.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class ChatClientTest {
    private val created = mutableListOf<Pair<String, FakeConnection>>()
    private val scope = TestScope(UnconfinedTestDispatcher())

    @After
    fun tearDown() {
        scope.cancel()
    }

    private fun factory(): (String) -> HostConnection =
        { endpoint -> FakeConnection().also { created += endpoint to it } }

    private fun client(fetch: suspend (String) -> List<CatalogHost>): ChatClient =
        ChatClient(scope, connectionFactory = factory(), fetchHosts = fetch)

    @Test
    fun `blank endpoint is a no-op`() {
        val vm = client { error("should not fetch") }
        vm.setEndpoint("   ")
        vm.connect()
        assertEquals(ConnectionState.Disconnected, vm.connectionState.value)
        assertTrue(created.isEmpty())
    }

    @Test
    fun `catalog with hosts opens one connection per host and merges sessions`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1"), CatalogHost("h2", 1002, "L2")) }
        vm.setEndpoint("host:8770")
        vm.connect()

        assertEquals(2, created.size)
        assertEquals("host:8770/ws/1001", created[0].first)
        assertEquals("host:8770/ws/1002", created[1].first)
        assertTrue(created.all { it.second.started })

        created[0].second.sessionsFlow.value = listOf(SessionInfo("r1", "T1"))
        created[1].second.sessionsFlow.value = listOf(SessionInfo("r2", "T2"))

        val merged = vm.sessions.value
        assertEquals(listOf("r1", "r2"), merged.map { it.resource })
        assertEquals(SessionInfo("r1", "T1", hostId = "h1", hostLabel = "L1"), merged[0])
        assertEquals(SessionInfo("r2", "T2", hostId = "h2", hostLabel = "L2"), merged[1])
    }

    @Test
    fun `connection state reflects the strongest host state`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1"), CatalogHost("h2", 1002, "L2")) }
        vm.setEndpoint("host:8770")
        vm.connect()

        created[0].second.stateFlow.value = ConnectionState.Connecting
        assertEquals(ConnectionState.Connecting, vm.connectionState.value)

        created[1].second.stateFlow.value = ConnectionState.Connected
        assertEquals(ConnectionState.Connected, vm.connectionState.value)

        created[0].second.stateFlow.value = ConnectionState.Disconnected
        created[1].second.stateFlow.value = ConnectionState.Disconnected
        assertEquals(ConnectionState.Disconnected, vm.connectionState.value)
    }

    @Test
    fun `empty catalog stays disconnected and opens no connection`() {
        val vm = client { emptyList() }
        vm.setEndpoint("host:8770")
        vm.connect()

        assertEquals(ConnectionState.Disconnected, vm.connectionState.value)
        assertTrue(created.isEmpty())
    }

    @Test
    fun `catalog failure falls back to a single direct connection`() {
        val vm = client { throw IllegalStateException("no /hosts here") }
        vm.setEndpoint("direct:9000")
        vm.connect()

        assertEquals(1, created.size)
        assertEquals("direct:9000", created[0].first)
        assertTrue(created[0].second.started)
    }

    @Test
    fun `selectSession routes transcript and selects on the owning connection`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1"), CatalogHost("h2", 1002, "L2")) }
        vm.setEndpoint("host:8770")
        vm.connect()

        vm.selectSession(SessionInfo("r2", "T2", hostId = "h2", hostLabel = "L2"))
        assertEquals("r2", vm.selectedSession.value)
        assertEquals("r2", created[1].second.selected)
        assertNull(created[0].second.selected)

        val rows = listOf(UiMessage("m1", "assistant", "hi", streaming = false, kind = "markdown"))
        created[1].second.transcriptFlow.value = rows
        assertEquals(rows, vm.messages.value)
    }

    @Test
    fun `selectSession falls back to first connection for an unknown host`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1")) }
        vm.setEndpoint("host:8770")
        vm.connect()

        vm.selectSession(SessionInfo("r9", "T9", hostId = "missing"))
        assertEquals("r9", created[0].second.selected)
    }

    @Test
    fun `selectSession is a no-op without connections`() {
        val vm = client { emptyList() }
        vm.selectSession(SessionInfo("r1", "T1"))
        assertNull(vm.selectedSession.value)
    }

    @Test
    fun `send trims and forwards to the active connection and ignores blank`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1"), CatalogHost("h2", 1002, "L2")) }
        vm.setEndpoint("host:8770")
        vm.connect()
        vm.selectSession(SessionInfo("r2", "T2", hostId = "h2"))

        vm.send("   ")
        vm.send("  hello  ")
        assertTrue(created[0].second.sent.isEmpty())
        assertEquals(listOf("hello"), created[1].second.sent)
    }

    @Test
    fun `send without an active connection does nothing`() {
        val vm = client { emptyList() }
        vm.send("hello")
        assertTrue(created.isEmpty())
    }

    @Test
    fun `switchSession reconnects and closes the previous connections`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1")) }
        vm.setEndpoint("host:8770")
        vm.connect()
        val first = created[0].second

        vm.switchSession()
        assertTrue(first.closed)
        assertEquals(2, created.size)
        assertFalse(created[1].second.closed)
    }

    @Test
    fun `a stale in-flight discovery is discarded on reconnect`() {
        var call = 0
        val gate = CompletableDeferred<List<CatalogHost>>()
        val vm =
            ChatClient(
                scope,
                connectionFactory = factory(),
                fetchHosts = {
                    if (call++ == 0) gate.await() else listOf(CatalogHost("h2", 2002, "L2"))
                },
            )
        vm.setEndpoint("host:8770")
        vm.connect()
        vm.connect()

        gate.complete(listOf(CatalogHost("h1", 1001, "L1")))

        assertEquals(1, created.size)
        assertEquals("host:8770/ws/2002", created[0].first)
    }

    @Test
    fun `close tears down open connections`() {
        val vm = client { listOf(CatalogHost("h1", 1001, "L1")) }
        vm.setEndpoint("host:8770")
        vm.connect()
        val conn = created[0].second

        vm.close()
        assertTrue(conn.closed)
        assertTrue(scope.isActive)
    }

    @Test
    fun `non-cancellable stale discovery cannot open a connection after replacement`() {
        var call = 0
        val gate = CompletableDeferred<List<CatalogHost>>()
        val vm =
            client {
                if (call++ == 0) {
                    withContext(NonCancellable) { gate.await() }
                } else {
                    listOf(CatalogHost("h2", 2002))
                }
            }
        vm.setEndpoint("host:8770")
        vm.connect()
        vm.connect()
        gate.complete(listOf(CatalogHost("h1", 1001)))

        assertEquals(listOf("host:8770/ws/2002"), created.map { it.first })
    }

    @Test
    fun `replacement and close cancel suspended discovery without cancelling owner scope`() {
        for (replace in listOf(false, true)) {
            var cancelled = false
            var call = 0
            val gate = CompletableDeferred<List<CatalogHost>>()
            val vm =
                client {
                    try {
                        if (call++ == 0) gate.await() else emptyList()
                    } catch (failure: CancellationException) {
                        cancelled = true
                        throw failure
                    }
                }
            vm.setEndpoint("host:8770")
            vm.connect()
            assertFalse(cancelled)

            if (replace) vm.connect() else vm.close()

            assertTrue(cancelled)
            assertTrue(scope.isActive)
            assertTrue(created.isEmpty())
        }
    }

    @Test
    fun `replacement and close stop collecting old connection flows`() {
        for (replace in listOf(false, true)) {
            val vm = client { listOf(CatalogHost("h1", 1001)) }
            vm.setEndpoint("host:8770")
            vm.connect()
            vm.selectSession(SessionInfo("r1", "T1", hostId = "h1"))
            val old = created.last().second
            old.sessionsFlow.value = listOf(SessionInfo("r1", "T1"))
            old.stateFlow.value = ConnectionState.Connected
            old.transcriptFlow.value = listOf(UiMessage("m1", "assistant", "old", streaming = false))

            if (replace) vm.switchSession() else vm.close()

            val before = Triple(vm.connectionState.value, vm.sessions.value, vm.messages.value)
            old.sessionsFlow.value = listOf(SessionInfo("stale", "Stale"))
            old.stateFlow.value = ConnectionState.Connecting
            old.transcriptFlow.value = listOf(UiMessage("stale", "assistant", "stale", streaming = true))

            assertEquals(before, Triple(vm.connectionState.value, vm.sessions.value, vm.messages.value))
            assertEquals(0, old.sessionsFlow.subscriptionCount.value)
            assertEquals(0, old.stateFlow.subscriptionCount.value)
            assertEquals(0, old.transcriptFlow.subscriptionCount.value)
            assertTrue(old.closed)
            assertTrue(scope.isActive)
            vm.close()
        }
    }

    @Test
    fun `UiMessage defaults kind to message`() {
        val msg = UiMessage("i1", "user", "hi", streaming = false)
        assertEquals("message", msg.kind)
    }

    private class FakeConnection : HostConnection {
        val stateFlow = MutableStateFlow(ConnectionState.Disconnected)
        val transcriptFlow = MutableStateFlow<List<UiMessage>>(emptyList())
        val sessionsFlow = MutableStateFlow<List<SessionInfo>>(emptyList())
        override val state: StateFlow<ConnectionState> = stateFlow
        override val transcript: StateFlow<List<UiMessage>> = transcriptFlow
        override val availableSessions: StateFlow<List<SessionInfo>> = sessionsFlow
        var started = false
        var closed = false
        var selected: String? = null
        val sent = mutableListOf<String>()

        override fun start() {
            started = true
        }

        override fun close() {
            closed = true
        }

        override fun send(text: String): Boolean {
            sent.add(text)
            return true
        }

        override fun selectSession(resource: String) {
            selected = resource
        }
    }
}
