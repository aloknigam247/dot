package com.dot.companion.net

import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
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
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.KSerializer
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.IOException
import java.util.concurrent.CompletableFuture
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/** Drives a real [AhpConnection] against a scripted AHP peer over a local WebSocket. */
class AhpConnectionTest {
    private val client = OkHttpClient()
    private val frames = LinkedBlockingQueue<String>()
    private val logs = LinkedBlockingQueue<Pair<String, Throwable?>>()
    private val peerSocket = CompletableFuture<WebSocket>()
    private val server = MockWebServer()
    private var serverSeq = 0L
    private lateinit var conn: AhpConnection

    @Before
    fun setUp() {
        server.enqueue(
            MockResponse().withWebSocketUpgrade(
                object : WebSocketListener() {
                    override fun onOpen(webSocket: WebSocket, response: Response) {
                        peerSocket.complete(webSocket)
                    }

                    override fun onMessage(webSocket: WebSocket, text: String) {
                        frames.add(text)
                    }
                },
            ),
        )
        server.start()
        conn = AhpConnection(server.url("/").toString(), ChatLogger { m, cause -> logs.add(m to cause) }, client)
    }

    @After
    fun tearDown() {
        conn.close()
        server.shutdown()
        client.dispatcher.executorService.shutdown()
        client.connectionPool.evictAll()
    }

    @Test
    fun `handshake routes sends to the default chat and streams delta-only markdown`() {
        handshake()

        assertTrue(conn.send("hello"))
        val dispatch = nextFrame()
        assertEquals("dispatchAction", dispatch.method())
        val params =
            Ahp.json.decodeFromJsonElement(DispatchActionParams.serializer(), dispatch.getValue("params"))
        assertEquals(CHAT_2, params.channel)
        assertEquals("hello", (params.action as StateActionChatTurnStarted).value.message.text)

        val prompt = UiMessage("$CHAT_2#t1:msg", "user", "hello", streaming = false, kind = "message")
        action(
            StateActionChatTurnStarted(
                ChatTurnStartedAction(
                    type = ActionType.CHAT_TURN_STARTED,
                    turnId = "t1",
                    startedAt = "2025-01-01T00:00:01.000Z",
                    message = Message(text = "hello", origin = MessageOrigin(kind = MessageKind.USER)),
                ),
            ),
        )
        awaitTranscript(prompt, UiMessage("$CHAT_2#t1:reply", "assistant", "", streaming = true, kind = "markdown"))

        action(delta("Hi"))
        awaitTranscript(prompt, reply("Hi", streaming = true))

        action(delta(" there"))
        awaitTranscript(prompt, reply("Hi there", streaming = true))

        action(
            StateActionChatTurnComplete(
                ChatTurnCompleteAction(type = ActionType.CHAT_TURN_COMPLETE, turnId = "t1", duration = 5L),
            ),
        )
        awaitTranscript(prompt, reply("Hi there", streaming = false))

        conn.close()
        assertFalse(conn.send("late"))
    }

    @Test
    fun `dropped frames and rpc errors reach the injected logger`() {
        handshake()

        peer().send("not json")
        val frame = nextLog()
        assertTrue(frame.first, frame.first.startsWith("dropped malformed frame:"))
        assertNotNull(frame.second)

        peer().send(notification("action", buildJsonObject { put("channel", 1) }))
        val action = nextLog()
        assertTrue(action.first, action.first.startsWith("dropped malformed action:"))
        assertNotNull(action.second)

        conn.selectSession(OTHER_SESSION)
        val subscribe = nextFrame()
        assertSubscribe(subscribe, OTHER_SESSION)
        val error =
            buildJsonObject {
                put("jsonrpc", "2.0")
                put("id", subscribe.id())
                put("error", buildJsonObject { put("code", -32000) })
            }
        peer().send(error.toString())
        val rpc = nextLog()
        assertTrue(rpc.first, rpc.first.startsWith("rpc error id=${subscribe.id()} kind=Subscribe"))
        assertNull(rpc.second)
        val failed = await(conn.progress) { it.stage == ProgressStage.Failed }
        assertEquals(ProgressStage.Subscribing, failed.failedDuring)
        assertEquals(Diagnostic("subscribe", "rpc error -32000"), failed.diagnostic)
    }

    @Test
    fun `empty session list is ready only after its reply`() {
        conn.start()
        val initialize = nextFrame()
        await(conn.progress) { it.stage == ProgressStage.Initializing }
        assertEquals(ConnectionState.Connected, conn.state.value)
        assertNull(frames.poll(100, TimeUnit.MILLISECONDS))
        respond(initialize, InitializeResult.serializer(), initialized())
        val list = nextFrame()
        assertEquals(ProgressStage.Listing, conn.progress.value.stage)
        respond(list, ListSessionsResult.serializer(), ListSessionsResult(items = emptyList()))
        await(conn.progress) { it.stage == ProgressStage.CatalogReady }
        assertTrue(conn.availableSessions.value.isEmpty())
        assertNull(frames.poll(100, TimeUnit.MILLISECONDS))
    }

    @Test
    fun `initialize error retains operation and interrupted stage after close`() {
        conn.start()
        val request = nextFrame()
        peer().send(
            buildJsonObject {
                put("id", request.id())
                put(
                    "error",
                    buildJsonObject {
                        put("code", -32001)
                        put("message", "secret-token")
                    },
                )
            }.toString(),
        )
        val failed = await(conn.progress) { it.stage == ProgressStage.Failed }
        assertEquals(ProgressStage.Initializing, failed.failedDuring)
        assertEquals(Diagnostic("initialize", "rpc error -32001"), failed.diagnostic)
        await(conn.state) { it == ConnectionState.Disconnected }
        conn.close()
        assertEquals(failed, conn.progress.value)
        assertNull(frames.poll(100, TimeUnit.MILLISECONDS))
    }

    @Test
    fun `peer close and malformed payload diagnostics do not expose secrets`() {
        handshake()
        peer().send("secret-token not JSON")
        val malformed = await(conn.progress) { it.diagnostic?.operation == "frame" }
        assertEquals(ProgressStage.ChatReady, malformed.stage)
        assertFalse(malformed.toString().contains("secret-token"))
        peer().send(notification("action", buildJsonObject { put("secret", "token") }))
        val action = await(conn.progress) { it.diagnostic?.operation == "action" }
        assertEquals(ProgressStage.ChatReady, action.stage)
        assertFalse(action.toString().contains("token"))
        peer().close(1000, "secret-token")
        val closed = await(conn.progress) { it.stage == ProgressStage.Failed }
        assertEquals(ProgressStage.ChatReady, closed.failedDuring)
        assertEquals(Diagnostic("websocket", "closed by peer (code 1000)"), closed.diagnostic)
        assertEquals("IOException", Redaction.failure(IOException("Authorization: secret-token")))
        assertEquals("wss://host/chat?token=***", Redaction.endpoint("wss://user:secret@host/chat?token=value#secret"))
    }

    @Test
    fun `session with no outbound chat never becomes chat ready`() {
        handshake(emptyChats = true)
        val progress = await(conn.progress) { it.diagnostic?.summary == "selected session has no chat" }
        assertEquals(ProgressStage.Subscribing, progress.stage)
        assertFalse(conn.send("not ready"))
    }

    /** Runs initialize, listSessions, session selection, and the session's chat subscriptions. */
    private fun handshake(emptyChats: Boolean = false) {
        conn.start()

        val initialize = nextFrame()
        assertEquals("initialize", initialize.method())
        await(conn.state) { it == ConnectionState.Connected }
        await(conn.progress) { it.stage == ProgressStage.Initializing }
        assertNull(frames.poll(100, TimeUnit.MILLISECONDS))
        respond(initialize, InitializeResult.serializer(), initialized())

        val list = nextFrame()
        assertEquals("listSessions", list.method())
        assertEquals(ProgressStage.Listing, conn.progress.value.stage)
        val summary =
            SessionSummary(
                resource = SESSION,
                provider = "copilot",
                title = "S1",
                status = SessionStatus.IDLE,
                createdAt = TIME,
                modifiedAt = TIME,
            )
        respond(list, ListSessionsResult.serializer(), ListSessionsResult(items = listOf(summary)))
        await(conn.availableSessions) { it == listOf(SessionInfo(SESSION, "S1")) }
        await(conn.progress) { it.stage == ProgressStage.CatalogReady }
        assertFalse(conn.send("early"))

        conn.selectSession(SESSION)
        val session = nextFrame()
        assertSubscribe(session, SESSION)
        assertEquals(ProgressStage.Subscribing, conn.progress.value.stage)
        val state =
            SessionState(
                provider = "copilot",
                title = "S1",
                status = SessionStatus.IDLE,
                lifecycle = SessionLifecycle.READY,
                activeClients = emptyList(),
                chats = if (emptyChats) emptyList() else listOf(CHAT_1, CHAT_2).map(::chatSummary),
                defaultChat = if (emptyChats) null else CHAT_2,
            )
        respond(
            session,
            SubscribeResult.serializer(),
            SubscribeResult(Snapshot(SESSION, SnapshotState.Session(state), 0)),
        )

        if (emptyChats) return
        for (chat in listOf(CHAT_1, CHAT_2)) {
            val subscribe = nextFrame()
            assertSubscribe(subscribe, chat)
            assertEquals(ProgressStage.Subscribing, conn.progress.value.stage)
            val snapshot = Snapshot(chat, SnapshotState.Chat(chatState(chat)), 0)
            respond(subscribe, SubscribeResult.serializer(), SubscribeResult(snapshot))
        }
        await(conn.progress) { it.stage == ProgressStage.ChatReady }
    }

    private fun initialized(): InitializeResult =
        InitializeResult(
            protocolVersion = SUPPORTED_PROTOCOL_VERSIONS.first(),
            serverSeq = 0,
            serverInfo = Implementation(name = "peer", version = "1"),
            snapshots = emptyList(),
        )

    private fun action(action: StateAction) {
        val envelope = ActionEnvelope(channel = CHAT_2, action = action, serverSeq = ++serverSeq)
        peer().send(notification("action", Ahp.json.encodeToJsonElement(ActionEnvelope.serializer(), envelope)))
    }

    private fun assertSubscribe(frame: JsonObject, channel: String) {
        assertEquals("subscribe", frame.method())
        assertEquals(
            channel,
            frame
                .getValue("params")
                .jsonObject
                .getValue("channel")
                .jsonPrimitive.content,
        )
    }

    private fun <T> await(flow: StateFlow<T>, predicate: (T) -> Boolean): T =
        runBlocking { withTimeoutOrNull(TIMEOUT_MS) { flow.first(predicate) } }
            ?: throw AssertionError("timed out; last value ${flow.value}")

    private fun awaitTranscript(vararg rows: UiMessage) {
        await(conn.transcript) { it == rows.toList() }
    }

    private fun chatState(resource: String): ChatState =
        ChatState(
            resource = resource,
            title = resource,
            status = SessionStatus.IDLE,
            modifiedAt = TIME,
            turns = emptyList(),
        )

    private fun chatSummary(resource: String): ChatSummary =
        ChatSummary(resource = resource, title = resource, status = SessionStatus.IDLE, modifiedAt = TIME)

    private fun delta(content: String): StateActionChatDelta =
        StateActionChatDelta(
            ChatDeltaAction(type = ActionType.CHAT_DELTA, turnId = "t1", partId = "p1", content = content),
        )

    private fun JsonObject.id(): Long = getValue("id").jsonPrimitive.long

    private fun JsonObject.method(): String = getValue("method").jsonPrimitive.content

    private fun nextFrame(): JsonObject {
        val text = frames.poll(TIMEOUT_MS, TimeUnit.MILLISECONDS) ?: throw AssertionError("peer received no frame")
        return Ahp.json.parseToJsonElement(text).jsonObject
    }

    private fun nextLog(): Pair<String, Throwable?> =
        logs.poll(TIMEOUT_MS, TimeUnit.MILLISECONDS) ?: throw AssertionError("logger received no error")

    private fun notification(method: String, params: JsonElement): String =
        buildJsonObject {
            put("jsonrpc", "2.0")
            put("method", method)
            put("params", params)
        }.toString()

    private fun peer(): WebSocket = peerSocket.get(TIMEOUT_MS, TimeUnit.MILLISECONDS)

    private fun reply(text: String, streaming: Boolean): UiMessage =
        UiMessage("$CHAT_2#t1:part0", "assistant", text, streaming, kind = "markdown")

    private fun <R> respond(request: JsonObject, serializer: KSerializer<R>, result: R) {
        val response =
            buildJsonObject {
                put("jsonrpc", "2.0")
                put("id", request.id())
                put("result", Ahp.json.encodeToJsonElement(serializer, result))
            }
        peer().send(response.toString())
    }

    private companion object {
        const val CHAT_1 = "ahp-chat:/c1"
        const val CHAT_2 = "ahp-chat:/c2"
        const val OTHER_SESSION = "ahp-session:/s2"
        const val SESSION = "ahp-session:/s1"
        const val TIME = "2025-01-01T00:00:00.000Z"
        const val TIMEOUT_MS = 5_000L
    }
}
