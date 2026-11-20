package com.dot.companion.cli

import com.dot.companion.ChatClient
import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
import com.dot.companion.net.CatalogHost
import com.dot.companion.net.ConnectionState
import com.dot.companion.net.Diagnostic
import com.dot.companion.net.HostConnection
import com.dot.companion.net.HostProgress
import com.dot.companion.net.ProgressStage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.PrintWriter
import java.io.StringWriter
import java.util.concurrent.Executors

class ChatCliTest {
    private val fakes = mutableListOf<FakeConnection>()
    private val hosts = listOf(CatalogHost("h1", 1001, "One"), CatalogHost("h2", 1002, "Two"))
    private val scope = CoroutineScope(UnconfinedTestDispatcher() + Job())
    private val text = StringWriter()
    private val client = ChatClient(scope, { FakeConnection(it).also(fakes::add) }, { hosts })
    private val cli = ChatCli(client, scope, Terminal(PrintWriter(text)))

    @After
    fun tearDown() {
        scope.cancel()
    }

    @Test
    fun invalidOrStaleSelectionDoesNotSubscribe() {
        cli.handleCommand("select 1")
        assertOutput("error: unknown or stale session")
        connect()
        val (one, two) = fakes
        one.list(SESSION)
        cli.handleCommand("select 1")
        assertOutput("error: host catalogue is not ready")
        one.ready()
        two.list(SESSION)
        two.ready()
        for (stale in listOf("select 9", "select x", "select h3 $SESSION", "select h1 ahp-session:/missing")) {
            cli.handleCommand(stale)
        }
        assertEquals(4, Regex("unknown or stale session").findAll(out()).count() - 1)
        assertTrue(one.selected.isEmpty() && two.selected.isEmpty())

        cli.handleCommand("select h2 $SESSION")
        assertEquals(emptyList<String>(), one.selected)
        assertEquals(listOf(SESSION), two.selected)
        assertOutput("selected h2 $SESSION")
        cli.handleCommand("select h1 $SESSION")
        assertOutput("error: use sessions before selecting again")
        assertEquals(emptyList<String>(), one.selected)
    }

    @Test
    fun sessionsReplacesAWithB() {
        connect()
        val (oldOne, oldTwo) = fakes.toList()
        readyBoth()
        cli.handleCommand("select h1 $SESSION")
        cli.handleCommand("sessions")
        assertEquals(1, oldOne.closed)
        assertEquals(1, oldTwo.closed)
        val (newOne, newTwo) = fakes.drop(2)
        oldOne.transcript.value = listOf(UiMessage("late", "assistant", "stale", streaming = false))
        assertFalse(out().contains("stale"))
        newOne.list(SESSION)
        newOne.ready()
        newTwo.list(SESSION)
        newTwo.ready()
        cli.handleCommand("select 2")
        cli.handleCommand("send next")
        assertEquals(emptyList<String>(), oldOne.sent + oldTwo.sent + newOne.sent)
        assertEquals(listOf("next"), newTwo.sent)
    }

    @Test
    fun sendReportsOnlyQueueResult() {
        cli.handleCommand("send early")
        assertOutput("not queued")
        connect()
        readyBoth()
        cli.handleCommand("select 1")
        val host = fakes.first()
        cli.handleCommand("send   ")
        assertEquals(emptyList<String>(), host.sent)
        cli.handleCommand("send  hello  ")
        assertEquals(listOf("hello"), host.sent)
        assertOutput("queued (not acknowledged)")
        host.accepts = false
        cli.handleCommand("send again")
        assertEquals(3, Regex("(?m)^not queued$").findAll(out()).count())
        assertFalse(out().contains("accepted"))
    }

    @Test
    fun updatesPrintWhileInputWaits() {
        connect()
        readyBoth()
        cli.handleCommand("select 1")
        val host = fakes.first()
        val prompt = UiMessage("p", "user", "hello", streaming = false)
        host.transcript.value = listOf(prompt, UiMessage("r", "assistant", "Hi", streaming = true, kind = "markdown"))
        assertOutput("user: hello\nassistant: Hi")
        host.transcript.value = listOf(prompt, UiMessage("r", "assistant", "Hi there", streaming = true))
        assertOutput("assistant: Hi there")
        host.transcript.value = listOf(prompt, UiMessage("r", "assistant", "Hi there\u0007", streaming = false))
        assertOutput("assistant: Hi there\n")
        host.transcript.value = listOf(prompt, UiMessage("r", "assistant", "Rewritten", streaming = false))
        assertOutput("assistant: Rewritten\n")
        cli.handleCommand("show")
        assertOutput("user: hello\nassistant: Rewritten\n")
        host.transcript.value = emptyList()
        cli.handleCommand("status")
        assertOutput("host h1 (One): CatalogReady")
    }

    @Test
    fun invalidCommandKeepsRunning() {
        val inputs =
            listOf(
                "bogus",
                "",
                "connect",
                "connect bad host",
                "connect http://[",
                "help extra",
                "quit now",
                "status extra",
                "sessions",
            )
        inputs.forEach { assertTrue(it, cli.handleCommand(it)) }
        assertOutput("error: unknown command")
        assertOutput("error: connect requires a valid endpoint")
        assertOutput("error: unexpected arguments")
        assertOutput("error: connect first")
        assertTrue(fakes.isEmpty())
        assertTrue(cli.handleCommand("help"))
        assertOutput("select <host> <resource>")
    }

    @Test
    fun eofAndQuitCloseClient() {
        connect()
        fakes.first().fail()
        assertOutput("host h1 (One): Failed during Connecting | websocket: IOException")
        assertTrue(cli.handleCommand("disconnect"))
        assertOutput("disconnected")
        assertTrue(fakes.all { it.closed == 1 })
        cli.handleCommand("connect wss://user:secret@h:1/?token=value")
        assertOutput("discovery: Catalogue hosts=2")
        cli.handleCommand("status")
        assertOutput("endpoint: wss://h:1/?token=***")
        assertFalse(cli.handleCommand("quit"))
        assertFalse(cli.handleCommand("help"))
        cli.close()
        assertEquals(1, Regex("(?m)^closed$").findAll(out()).count())
        assertTrue(fakes.all { it.closed == 1 })
    }

    @Test
    fun runHandlesCommandsUntilEof() {
        val executor = Executors.newSingleThreadExecutor()
        val dispatcher = executor.asCoroutineDispatcher()
        try {
            val serial = CoroutineScope(dispatcher + Job())
            val output = StringWriter()
            val serialClient = ChatClient(serial, { FakeConnection(it).also(fakes::add) }, { hosts })
            val serialCli = ChatCli(serialClient, serial, Terminal(PrintWriter(output)))
            serialCli.run("connect h:1\nbogus\n".reader().buffered(), dispatcher)
            assertTrue(output.toString(), output.toString().contains("error: unknown command"))
            assertTrue(output.toString().trimEnd().endsWith("closed"))
            assertTrue(fakes.size == 2 && fakes.all { it.closed == 1 })
            serial.cancel()
        } finally {
            executor.shutdown()
        }
    }

    private fun assertOutput(expected: String) {
        assertTrue("Missing '$expected' in:\n${out()}", out().contains(expected))
    }

    private fun out(): String = text.toString().replace("\r\n", "\n")

    private fun connect() {
        assertTrue(cli.handleCommand("connect h:1"))
    }

    private fun readyBoth() {
        fakes.takeLast(2).forEach {
            it.list(SESSION)
            it.ready()
        }
    }

    private class FakeConnection(val endpoint: String) : HostConnection {
        override val state = MutableStateFlow(ConnectionState.Disconnected)
        override val progress = MutableStateFlow(HostProgress())
        override val transcript = MutableStateFlow<List<UiMessage>>(emptyList())
        override val availableSessions = MutableStateFlow<List<SessionInfo>>(emptyList())
        val selected = mutableListOf<String>()
        val sent = mutableListOf<String>()
        var accepts = true
        var closed = 0

        override fun start() {
            state.value = ConnectionState.Connecting
            progress.value = HostProgress(ProgressStage.Connecting)
        }

        override fun close() {
            closed++
        }

        override fun send(text: String): Boolean {
            if (accepts) sent += text
            return accepts
        }

        override fun selectSession(resource: String) {
            selected += resource
        }

        fun list(resource: String) {
            availableSessions.value = listOf(SessionInfo(resource, "Title"))
        }

        fun ready() {
            state.value = ConnectionState.Connected
            progress.value = HostProgress(ProgressStage.CatalogReady)
        }

        fun fail() {
            progress.value =
                HostProgress(ProgressStage.Failed, ProgressStage.Connecting, Diagnostic("websocket", "IOException"))
        }
    }

    private companion object {
        const val SESSION = "ahp-session:/shared"
    }
}
