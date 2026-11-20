package com.dot.companion.cli

import com.microsoft.agenthostprotocol.Ahp
import com.microsoft.agenthostprotocol.generated.ActionEnvelope
import com.microsoft.agenthostprotocol.generated.ActionType
import com.microsoft.agenthostprotocol.generated.ChatDeltaAction
import com.microsoft.agenthostprotocol.generated.ChatState
import com.microsoft.agenthostprotocol.generated.ChatSummary
import com.microsoft.agenthostprotocol.generated.ChatTurnCompleteAction
import com.microsoft.agenthostprotocol.generated.ChatTurnStartedAction
import com.microsoft.agenthostprotocol.generated.DispatchActionParams
import com.microsoft.agenthostprotocol.generated.Implementation
import com.microsoft.agenthostprotocol.generated.InitializeResult
import com.microsoft.agenthostprotocol.generated.ListSessionsResult
import com.microsoft.agenthostprotocol.generated.Message
import com.microsoft.agenthostprotocol.generated.MessageKind
import com.microsoft.agenthostprotocol.generated.MessageOrigin
import com.microsoft.agenthostprotocol.generated.SUPPORTED_PROTOCOL_VERSIONS
import com.microsoft.agenthostprotocol.generated.SessionLifecycle
import com.microsoft.agenthostprotocol.generated.SessionState
import com.microsoft.agenthostprotocol.generated.SessionStatus
import com.microsoft.agenthostprotocol.generated.SessionSummary
import com.microsoft.agenthostprotocol.generated.Snapshot
import com.microsoft.agenthostprotocol.generated.SnapshotState
import com.microsoft.agenthostprotocol.generated.StateAction
import com.microsoft.agenthostprotocol.generated.StateActionChatDelta
import com.microsoft.agenthostprotocol.generated.StateActionChatTurnComplete
import com.microsoft.agenthostprotocol.generated.StateActionChatTurnStarted
import com.microsoft.agenthostprotocol.generated.SubscribeResult
import kotlinx.serialization.KSerializer
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import okhttp3.tls.HandshakeCertificates
import okhttp3.tls.HeldCertificate
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.Closeable
import java.io.File
import java.nio.file.Files
import java.security.KeyStore
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class NativeSmokeTest {
    @Test
    fun controlledInteractiveLifecycle() {
        Hub().use { hub ->
            NativeProcess().use { app ->
                app.command("connect ${hub.endpoint}")
                val a = hub.peer("a")
                val b = hub.peer("b")
                a.catalogue()
                b.catalogue()
                app.awaitOutput("host b (B): CatalogReady")
                app.command("select b $SESSION")
                b.subscribe()
                app.awaitOutput("host b (B): ChatReady")
                app.command("send   hello  ")
                val dispatch = b.dispatch()
                assertEquals(b.defaultChat, dispatch.channel)
                assertEquals("hello", (dispatch.action as StateActionChatTurnStarted).value.message.text)
                app.awaitOutput("queued (not acknowledged)")
                b.startTurn()
                b.delta("Hi")
                app.awaitOutput("assistant: Hi")
                assertFalse(app.output().contains("Hi there"))
                b.delta(" there")
                app.awaitOutput("assistant: Hi there")
                b.completeTurn()
                assertEquals(null, a.frames.poll(100, TimeUnit.MILLISECONDS))
                app.command("sessions")
                assertTrue(a.closed.await(5, TimeUnit.SECONDS))
                assertTrue(b.closed.await(5, TimeUnit.SECONDS))
                val nextA = hub.peer("a")
                val nextB = hub.peer("b")
                app.command("select a $SESSION")
                app.awaitOutput("error: unknown or stale session")
                nextA.catalogue()
                nextB.catalogue()
                app.awaitOutput("host a (A): CatalogReady", occurrence = 2)
                app.awaitOutput("host b (B): CatalogReady", occurrence = 2)
                app.command("select a $SESSION")
                nextA.subscribe()
                app.awaitOutput("host a (A): ChatReady")
                app.command("send next")
                assertEquals(nextA.defaultChat, nextA.dispatch().channel)
                assertEquals(null, nextB.frames.poll(100, TimeUnit.MILLISECONDS))
                app.recordPeakWorkingSet()
                app.command("quit")
                app.awaitExit()
                app.awaitOutput("closed")
                assertTrue(nextA.closed.await(5, TimeUnit.SECONDS))
                assertTrue(nextB.closed.await(5, TimeUnit.SECONDS))
            }
        }
    }

    @Test
    fun ctrlCClosesPendingConnection() {
        Hub(direct = true).use { hub ->
            val id = UUID.randomUUID()
            val reports = reports()
            val report = File(reports, "ctrl_c_$id.json")
            val signal = File(reports, "ctrl_c_$id.signal")
            val helperOutput = StringBuffer()
            val helper =
                ProcessBuilder(
                    "pwsh",
                    "-NoProfile",
                    "-File",
                    requireNotNull(System.getProperty("dot.chat.smokeHelper")),
                    "-Endpoint",
                    hub.endpoint,
                    "-Executable",
                    executable(),
                    "-OutputPath",
                    report.absolutePath,
                    "-SignalPath",
                    signal.absolutePath,
                ).redirectErrorStream(true).start()
            val drain = thread(isDaemon = true) { helperOutput.append(helper.inputStream.reader().readText()) }
            try {
                val peer = hub.peer("a")
                assertEquals("initialize", peer.request().method())
                signal.writeText("ready")
                assertTrue("Ctrl+C helper timed out:\n$helperOutput", helper.waitFor(30, TimeUnit.SECONDS))
                drain.join(1000)
                assertEquals("Ctrl+C helper failed:\n$helperOutput", 0, helper.exitValue())
                val result = Ahp.json.parseToJsonElement(report.readText()).jsonObject
                assertEquals("true", result.getValue("eventDelivered").jsonPrimitive.content)
                assertTrue(
                    result.toString(),
                    result
                        .getValue("exitCode")
                        .jsonPrimitive.content
                        .toLongOrNull() != null,
                )
                assertTrue(
                    result.toString(),
                    result
                        .getValue("output")
                        .jsonPrimitive.content
                        .contains("closed"),
                )
                assertTrue(peer.closed.await(5, TimeUnit.SECONDS))
            } finally {
                if (helper.isAlive) helper.destroyForcibly()
                signal.delete()
            }
        }
    }

    @Test
    fun trustedTlsSucceedsAndInvalidTlsFails() {
        val ca =
            HeldCertificate
                .Builder()
                .commonName("dot test CA")
                .certificateAuthority(1)
                .build()
        val trust = Files.createTempFile("dot_chat_${UUID.randomUUID()}", ".p12").toFile()
        try {
            val store = KeyStore.getInstance("PKCS12")
            store.load(null, null)
            store.setCertificateEntry("ca", ca.certificate)
            trust.outputStream().use { store.store(it, "test-only".toCharArray()) }
            val trustArgs =
                listOf(
                    "-Djavax.net.ssl.trustStore=${trust.absolutePath}",
                    "-Djavax.net.ssl.trustStorePassword=test-only",
                )
            for (mode in listOf("trusted", "untrusted", "wrong-host")) {
                val cert =
                    HeldCertificate
                        .Builder()
                        .commonName("dot peer")
                        .addSubjectAlternativeName(if (mode == "wrong-host") "wrong.invalid" else "localhost")
                        .signedBy(ca)
                        .build()
                Hub(HandshakeCertificates.Builder().heldCertificate(cert, ca.certificate).build()).use { hub ->
                    NativeProcess(if (mode == "untrusted") emptyList() else trustArgs).use { app ->
                        app.command("connect ${hub.endpoint}")
                        if (mode == "trusted") {
                            val a = hub.peer("a")
                            val b = hub.peer("b")
                            a.catalogue()
                            b.catalogue()
                            app.awaitOutput("host a (A): CatalogReady")
                            app.command("select a $SESSION")
                            a.subscribe()
                            app.awaitOutput("host a (A): ChatReady")
                            app.command("send tls")
                            assertEquals(a.defaultChat, a.dispatch().channel)
                        } else {
                            app.awaitOutput("Failed during Connecting")
                            assertFalse(app.output().contains("CatalogReady"))
                            assertTrue(hub.incomingA.isEmpty())
                            assertTrue(hub.incomingB.isEmpty())
                        }
                        app.command("quit")
                        app.awaitExit()
                    }
                }
            }
        } finally {
            assertTrue(trust.delete())
        }
    }

    @Test
    fun eofDuringInitializeClosesSocket() {
        Hub(direct = true).use { hub ->
            NativeProcess().use { app ->
                app.command("connect ${hub.endpoint}")
                val peer = hub.peer("a")
                assertEquals("initialize", peer.request().method())
                app.awaitOutput("DirectFallback")
                app.eof()
                app.awaitExit()
                assertTrue(peer.closed.await(5, TimeUnit.SECONDS))
            }
        }
    }

    private class NativeProcess(arguments: List<String> = emptyList()) : Closeable {
        private val captured = StringBuffer()
        private val process = ProcessBuilder(listOf(executable()) + arguments).redirectErrorStream(true).start()
        private val input = process.outputStream.bufferedWriter(Charsets.UTF_8)
        private val reader =
            thread(name = "dot-chat-output", isDaemon = true) {
                process.inputStream.reader(Charsets.UTF_8).use { source ->
                    val buffer = CharArray(1024)
                    while (true) {
                        val count = source.read(buffer)
                        if (count < 0) break
                        captured.append(buffer, 0, count)
                    }
                }
            }

        fun command(line: String) {
            input.write(line)
            input.newLine()
            input.flush()
        }

        fun eof() = input.close()

        fun recordPeakWorkingSet() {
            val query =
                ProcessBuilder(
                    "pwsh",
                    "-NoProfile",
                    "-Command",
                    "(Get-Process -Id ${process.pid()}).PeakWorkingSet64",
                ).redirectErrorStream(true).start()
            val text =
                query.inputStream
                    .reader()
                    .readText()
                    .trim()
            assertTrue("PeakWorkingSet64 query timed out", query.waitFor(30, TimeUnit.SECONDS))
            val bytes = requireNotNull(text.toLongOrNull()) { "PeakWorkingSet64 query failed: $text" }
            val record = buildJsonObject { put("peakWorkingSetBytes", bytes) }
            File(reports(), "native_smoke_${UUID.randomUUID()}.json").writeText(record.toString())
        }

        fun output(): String = captured.toString()

        fun awaitOutput(expected: String, occurrence: Int = 1) {
            fun found() = Regex.fromLiteral(expected).findAll(output()).count() >= occurrence
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
            while (!found() && process.isAlive && System.nanoTime() < deadline) Thread.sleep(10)
            assertTrue("Missing '$expected' x$occurrence in native output:\n${output()}", found())
        }

        fun awaitExit() {
            assertTrue("Native exit timed out:\n${output()}", process.waitFor(10, TimeUnit.SECONDS))
            reader.join(1000)
            assertEquals("Native failure:\n${output()}", 0, process.exitValue())
        }

        override fun close() {
            if (process.isAlive) {
                input.close()
                if (!process.waitFor(10, TimeUnit.SECONDS)) {
                    process.destroyForcibly().waitFor()
                    throw AssertionError("Native cleanup required termination:\n${output()}")
                }
            }
            reader.join(1000)
        }
    }

    private class Hub(tls: HandshakeCertificates? = null, direct: Boolean = false) : Closeable {
        val incomingA = LinkedBlockingQueue<Peer>()
        val incomingB = LinkedBlockingQueue<Peer>()
        private val server = MockWebServer()

        init {
            if (tls != null) server.useHttps(tls.sslSocketFactory(), false)
            server.dispatcher =
                object : Dispatcher() {
                    override fun dispatch(request: RecordedRequest): MockResponse =
                        when (request.path) {
                            "/hosts" ->
                                if (direct) {
                                    MockResponse().setResponseCode(404)
                                } else {
                                    MockResponse().setBody(
                                        """[{"id":"a","port":1001,"label":"A"},{"id":"b","port":1002,"label":"B"}]""",
                                    )
                                }
                            "/", "/ws/1001" -> MockResponse().withWebSocketUpgrade(Peer("a", incomingA))
                            "/ws/1002" -> MockResponse().withWebSocketUpgrade(Peer("b", incomingB))
                            else -> MockResponse().setResponseCode(404)
                        }
                }
            server.start()
        }

        val endpoint: String get() =
            server
                .url("/")
                .newBuilder()
                .host("localhost")
                .build()
                .toString()
                .trimEnd('/')

        fun peer(id: String): Peer =
            (if (id == "a") incomingA else incomingB).poll(10, TimeUnit.SECONDS)
                ?: throw AssertionError("Native process did not open host $id")

        override fun close() = server.shutdown()
    }

    private class Peer(private val host: String, private val incoming: LinkedBlockingQueue<Peer>) :
        WebSocketListener() {
        val closed = CountDownLatch(1)
        val defaultChat = "ahp-chat:/$host-default"
        val frames = LinkedBlockingQueue<JsonObject>()
        private var sequence = 0L
        private lateinit var socket: WebSocket

        override fun onOpen(webSocket: WebSocket, response: Response) {
            socket = webSocket
            incoming.add(this)
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            frames.add(Ahp.json.parseToJsonElement(text).jsonObject)
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            webSocket.close(code, null)
        }

        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            closed.countDown()
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            closed.countDown()
        }

        fun request(): JsonObject =
            frames.poll(10, TimeUnit.SECONDS) ?: throw AssertionError("No frame from native client for host $host")

        fun catalogue() {
            val initialize = request()
            assertEquals("initialize", initialize.method())
            respond(
                initialize,
                InitializeResult.serializer(),
                InitializeResult(
                    protocolVersion = SUPPORTED_PROTOCOL_VERSIONS.first(),
                    serverSeq = 0,
                    serverInfo = Implementation(name = "peer", version = "1"),
                    snapshots = emptyList(),
                ),
            )
            val list = request()
            assertEquals("listSessions", list.method())
            val summary =
                SessionSummary(
                    provider = "copilot",
                    title = "Shared session",
                    status = SessionStatus.IDLE,
                    resource = SESSION,
                    createdAt = TIME,
                    modifiedAt = TIME,
                )
            respond(list, ListSessionsResult.serializer(), ListSessionsResult(items = listOf(summary)))
        }

        fun subscribe() {
            val selection = request()
            assertEquals("subscribe", selection.method())
            assertEquals(
                SESSION,
                selection
                    .getValue("params")
                    .jsonObject
                    .getValue("channel")
                    .jsonPrimitive.content,
            )
            val chats = listOf("ahp-chat:/$host-other", defaultChat)
            val session =
                SessionState(
                    provider = "copilot",
                    title = "Shared session",
                    status = SessionStatus.IDLE,
                    lifecycle = SessionLifecycle.READY,
                    activeClients = emptyList(),
                    chats = chats.map { ChatSummary(it, it, SessionStatus.IDLE, modifiedAt = TIME) },
                    defaultChat = defaultChat,
                )
            respond(
                selection,
                SubscribeResult.serializer(),
                SubscribeResult(Snapshot(SESSION, SnapshotState.Session(session), 0)),
            )
            for (chat in chats) {
                val subscribe = request()
                assertEquals("subscribe", subscribe.method())
                assertEquals(
                    chat,
                    subscribe
                        .getValue("params")
                        .jsonObject
                        .getValue("channel")
                        .jsonPrimitive.content,
                )
                val state =
                    ChatState(
                        resource = chat,
                        title = chat,
                        status = SessionStatus.IDLE,
                        modifiedAt = TIME,
                        turns = emptyList(),
                    )
                respond(
                    subscribe,
                    SubscribeResult.serializer(),
                    SubscribeResult(Snapshot(chat, SnapshotState.Chat(state), 0)),
                )
            }
        }

        fun dispatch(): DispatchActionParams {
            val frame = request()
            assertEquals("dispatchAction", frame.method())
            return Ahp.json.decodeFromJsonElement(DispatchActionParams.serializer(), frame.getValue("params"))
        }

        fun startTurn() {
            action(
                StateActionChatTurnStarted(
                    ChatTurnStartedAction(
                        type = ActionType.CHAT_TURN_STARTED,
                        turnId = "t1",
                        startedAt = TIME,
                        message = Message(text = "hello", origin = MessageOrigin(kind = MessageKind.USER)),
                    ),
                ),
            )
        }

        fun delta(text: String) {
            action(
                StateActionChatDelta(ChatDeltaAction(ActionType.CHAT_DELTA, "t1", "p1", text)),
            )
        }

        fun completeTurn() {
            action(StateActionChatTurnComplete(ChatTurnCompleteAction(ActionType.CHAT_TURN_COMPLETE, "t1", 5L)))
        }

        private fun action(action: StateAction) {
            val envelope = ActionEnvelope(channel = defaultChat, action = action, serverSeq = ++sequence)
            socket.send(
                buildJsonObject {
                    put("jsonrpc", "2.0")
                    put("method", "action")
                    put("params", Ahp.json.encodeToJsonElement(ActionEnvelope.serializer(), envelope))
                }.toString(),
            )
        }

        private fun <T> respond(request: JsonObject, serializer: KSerializer<T>, value: T) {
            socket.send(
                buildJsonObject {
                    put("jsonrpc", "2.0")
                    put("id", request.getValue("id").jsonPrimitive.long)
                    put("result", Ahp.json.encodeToJsonElement(serializer, value))
                }.toString(),
            )
        }
    }

    private companion object {
        const val SESSION = "ahp-session:/shared"
        const val TIME = "2025-01-01T00:00:00.000Z"

        fun executable(): String =
            File(requireNotNull(System.getProperty("dot.chat.exe")) { "dot.chat.exe is required" }).absolutePath

        fun reports(): File =
            File(requireNotNull(System.getProperty("dot.chat.reports")) { "dot.chat.reports is required" })
                .apply { mkdirs() }

        fun JsonObject.method(): String = getValue("method").jsonPrimitive.content
    }
}
