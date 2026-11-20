package com.dot.companion

import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
import com.dot.companion.net.AhpConnection
import com.dot.companion.net.CatalogException
import com.dot.companion.net.CatalogHost
import com.dot.companion.net.ConnectionState
import com.dot.companion.net.Diagnostic
import com.dot.companion.net.DiscoveryProgress
import com.dot.companion.net.DiscoveryStage
import com.dot.companion.net.HostConnection
import com.dot.companion.net.HostProgress
import com.dot.companion.net.HostStatus
import com.dot.companion.net.Redaction
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Owns the live [AhpConnection]s and mirrors their transcripts and connection state
 * into StateFlows for the UI.
 *
 * The endpoint is a cptower multiplexer base (`host:port`). On [connect] the model
 * fetches cptower's `/hosts` catalog and opens one connection per advertised host at
 * `/ws/<port>`, merging every host's sessions into a single picker tagged by host. If
 * the endpoint is not a cptower instance (no `/hosts`), it falls back to a single
 * direct connection to the endpoint itself. Each [connect] discards any prior
 * connections (there is no in-place reconnect), so an explicit reconnect can never
 * duplicate history. Only the selected session's connection feeds the transcript.
 *
 * Public methods and [scope] must share a single serial dispatcher. [close] cancels
 * this client's jobs without cancelling the caller-owned scope. [discovery] and
 * [hostProgress] expose diagnostics without changing [connectionState]'s meaning.
 */
class ChatClient(
    private val scope: CoroutineScope,
    private val connectionFactory: (String) -> HostConnection,
    private val fetchHosts: suspend (String) -> List<CatalogHost>,
) {
    private val _messages = MutableStateFlow<List<UiMessage>>(emptyList())
    val messages: StateFlow<List<UiMessage>> = _messages.asStateFlow()

    private val _connectionState = MutableStateFlow(ConnectionState.Disconnected)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _endpoint = MutableStateFlow("")
    val endpoint: StateFlow<String> = _endpoint.asStateFlow()

    private val _sessions = MutableStateFlow<List<SessionInfo>>(emptyList())
    val sessions: StateFlow<List<SessionInfo>> = _sessions.asStateFlow()

    private val _selectedSession = MutableStateFlow<String?>(null)
    val selectedSession: StateFlow<String?> = _selectedSession.asStateFlow()

    private val _discovery = MutableStateFlow(DiscoveryProgress())
    val discovery: StateFlow<DiscoveryProgress> = _discovery.asStateFlow()

    private val _hostProgress = MutableStateFlow<List<HostStatus>>(emptyList())
    val hostProgress: StateFlow<List<HostStatus>> = _hostProgress.asStateFlow()

    private val connections = LinkedHashMap<String, HostConnection>()
    private val hostLabels = LinkedHashMap<String, String>()
    private val hostProgressById = LinkedHashMap<String, HostProgress>()
    private val hostSessions = LinkedHashMap<String, List<SessionInfo>>()
    private val hostStates = LinkedHashMap<String, ConnectionState>()
    private val jobs = mutableListOf<Job>()
    private var connectJob: Job? = null
    private var transcriptJob: Job? = null
    private var active: HostConnection? = null
    private var generation = 0

    /** Sets the cptower endpoint (`host:port`, or a full `ws://`/`wss://` URL). */
    fun setEndpoint(value: String) {
        _endpoint.value = value.trim()
    }

    /** (Re)connect to the endpoint. A blank endpoint is a no-op. */
    fun connect() {
        val target = _endpoint.value.trim()
        if (target.isEmpty()) return

        teardown()
        _messages.value = emptyList()
        _sessions.value = emptyList()
        _selectedSession.value = null
        _connectionState.value = ConnectionState.Connecting
        _discovery.value = DiscoveryProgress(DiscoveryStage.Fetching)
        _hostProgress.value = emptyList()
        val gen = ++generation

        connectJob =
            scope.launch {
                val hosts =
                    try {
                        fetchHosts(target)
                    } catch (cancel: CancellationException) {
                        throw cancel
                    } catch (failure: Exception) {
                        if (gen == generation) {
                            _discovery.value =
                                DiscoveryProgress(
                                    DiscoveryStage.DirectFallback,
                                    diagnostic = catalogDiagnostic(failure),
                                )
                            openConnection(id = target, label = "", endpoint = target)
                        }
                        return@launch
                    }
                if (gen != generation) return@launch
                if (hosts.isEmpty()) {
                    _discovery.value = DiscoveryProgress(DiscoveryStage.Empty)
                    _connectionState.value = ConnectionState.Disconnected
                } else {
                    _discovery.value = DiscoveryProgress(DiscoveryStage.Catalogue, hostCount = hosts.size)
                    hosts.forEach { host ->
                        openConnection(
                            id = host.id,
                            label = host.label,
                            endpoint = wsEndpoint(target, host.port),
                        )
                    }
                }
            }
    }

    /** Chooses which host session to observe and send to; scopes the transcript to it. */
    fun selectSession(session: SessionInfo) {
        val conn = connections[session.hostId] ?: connections.values.firstOrNull() ?: return
        active = conn
        transcriptJob?.cancel()
        _messages.value = emptyList()
        _selectedSession.value = session.resource
        transcriptJob = scope.launch { conn.transcript.collect { _messages.value = it } }
        conn.selectSession(session.resource)
    }

    /**
     * Returns to the session selector. Reconnects from scratch (the folded chat state
     * is owned by each connection's reader thread, so re-scoping in place would race);
     * the fresh handshakes re-list sessions and the picker reappears.
     */
    fun switchSession() {
        connect()
    }

    fun send(text: String) {
        enqueue(text)
    }

    /**
     * Queues [text] as a new turn on the selected session's default chat. Returns true only when the
     * socket accepted the frame for sending; it does not mean the host received or accepted the turn.
     */
    fun enqueue(text: String): Boolean {
        if (text.isBlank()) return false
        return active?.send(text.trim()) ?: false
    }

    /** Drops every connection and resets the published state; the endpoint is kept for a later [connect]. */
    fun disconnect() {
        teardown()
        _messages.value = emptyList()
        _sessions.value = emptyList()
        _selectedSession.value = null
        _connectionState.value = ConnectionState.Disconnected
        _discovery.value = DiscoveryProgress()
        _hostProgress.value = emptyList()
    }

    fun close() {
        teardown()
    }

    private fun openConnection(id: String, label: String, endpoint: String) {
        val conn = connectionFactory(endpoint)
        connections[id] = conn
        hostLabels[id] = label
        hostProgressById[id] = conn.progress.value
        hostStates[id] = ConnectionState.Connecting
        jobs +=
            scope.launch {
                conn.availableSessions.collect { list ->
                    hostSessions[id] = list.map { it.copy(hostId = id, hostLabel = label) }
                    recomputeSessions()
                }
            }
        jobs +=
            scope.launch {
                conn.state.collect { state ->
                    hostStates[id] = state
                    recomputeState()
                }
            }
        jobs +=
            scope.launch {
                conn.progress.collect { progress ->
                    hostProgressById[id] = progress
                    recomputeHostProgress()
                }
            }
        conn.start()
    }

    /** Reflects the strongest host state: Connected if any host is up, else Connecting if any is dialling. */
    private fun recomputeState() {
        val states = hostStates.values
        _connectionState.value =
            when {
                states.any { it == ConnectionState.Connected } -> ConnectionState.Connected
                states.any { it == ConnectionState.Connecting } -> ConnectionState.Connecting
                else -> ConnectionState.Disconnected
            }
    }

    private fun recomputeSessions() {
        _sessions.value = connections.keys.flatMap { hostSessions[it].orEmpty() }
    }

    private fun recomputeHostProgress() {
        _hostProgress.value =
            connections.keys.map { HostStatus(it, hostLabels[it].orEmpty(), hostProgressById[it] ?: HostProgress()) }
    }

    private fun catalogDiagnostic(failure: Exception): Diagnostic =
        if (failure is CatalogException) {
            Diagnostic(failure.operation, failure.summary)
        } else {
            Diagnostic("GET /hosts", Redaction.failure(failure))
        }

    /** Cancels owned work and closes every connection; the generation bump makes in-flight discovery stale. */
    private fun teardown() {
        generation++
        connectJob?.cancel()
        connectJob = null
        transcriptJob?.cancel()
        transcriptJob = null
        jobs.forEach { it.cancel() }
        jobs.clear()
        connections.values.forEach { it.close() }
        connections.clear()
        hostLabels.clear()
        hostProgressById.clear()
        hostSessions.clear()
        hostStates.clear()
        active = null
    }

    private fun wsEndpoint(base: String, port: Int): String = "${base.trim().trimEnd('/')}/ws/$port"
}
