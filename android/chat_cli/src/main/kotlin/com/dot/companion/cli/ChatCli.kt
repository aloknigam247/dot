package com.dot.companion.cli

import com.dot.companion.ChatClient
import com.dot.companion.model.SessionInfo
import com.dot.companion.model.UiMessage
import com.dot.companion.net.Diagnostic
import com.dot.companion.net.DiscoveryProgress
import com.dot.companion.net.HostStatus
import com.dot.companion.net.ProgressStage
import com.dot.companion.net.Redaction
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import java.io.BufferedReader
import java.io.PrintWriter

class ChatCli(private val client: ChatClient, scope: CoroutineScope, private val terminal: Terminal) {
    private var closed = false
    private var listed: List<SessionInfo> = emptyList()
    private val observers = mutableListOf<Job>()

    init {
        observers += scope.launch { client.discovery.collect { terminal.line(discoveryText(it)) } }
        observers +=
            scope.launch {
                var previous = emptyList<HostStatus>()
                client.hostProgress.collect { hosts ->
                    hosts.filter { it !in previous }.forEach { terminal.line(hostText(it)) }
                    previous = hosts
                }
            }
        observers +=
            scope.launch {
                client.sessions.collect { if (client.selectedSession.value == null) listSessions() }
            }
        observers += scope.launch { client.messages.collect(terminal::render) }
    }

    fun run(input: BufferedReader, dispatcher: CoroutineDispatcher) {
        terminal.line("dot_chat: type help for commands")
        try {
            while (true) {
                val line = input.readLine() ?: break
                if (!runBlocking(dispatcher) { handleCommand(line) }) break
            }
        } finally {
            runBlocking(dispatcher) { close() }
        }
    }

    fun handleCommand(line: String): Boolean {
        if (closed) return false
        val parts = line.trim().split(Regex("\\s+"), limit = 2)
        val args = parts.getOrElse(1) { "" }
        when (parts[0]) {
            "connect" -> {
                val url = args.replaceFirst(Regex("^ws:"), "http:").replaceFirst(Regex("^wss:"), "https:")
                val candidate = if ("://" in url) url else "http://$url"
                if (args.isEmpty() || args.any(Char::isWhitespace) || candidate.toHttpUrlOrNull() == null) {
                    terminal.line("error: connect requires a valid endpoint")
                } else {
                    listed = emptyList()
                    client.setEndpoint(args)
                    client.connect()
                }
            }
            "disconnect" ->
                noArgs(args) {
                    client.disconnect()
                    terminal.line("disconnected")
                }
            "help" -> noArgs(args) { HELP.forEach(terminal::line) }
            "quit" -> if (noArgs(args) { close() }) return false
            "select" -> select(args)
            "send" -> terminal.line(if (client.enqueue(args)) "queued (not acknowledged)" else "not queued")
            "sessions" ->
                noArgs(args) {
                    if (client.endpoint.value.isEmpty()) {
                        terminal.line("error: connect first")
                    } else {
                        listed = emptyList()
                        client.switchSession()
                    }
                }
            "show" ->
                noArgs(args) {
                    terminal.finish()
                    client.messages.value.forEach { terminal.line("${it.role}: ${it.text}") }
                }
            "status" ->
                noArgs(args) {
                    terminal.line("endpoint: ${Redaction.endpoint(client.endpoint.value)}")
                    terminal.line(discoveryText(client.discovery.value))
                    client.hostProgress.value.forEach { terminal.line(hostText(it)) }
                }
            else -> terminal.line("error: unknown command")
        }
        return true
    }

    fun close() {
        if (closed) return
        closed = true
        observers.forEach(Job::cancel)
        client.close()
        terminal.line("closed")
    }

    private fun noArgs(args: String, command: () -> Unit): Boolean {
        if (args.isNotEmpty()) {
            terminal.line("error: unexpected arguments")
            return false
        }
        command()
        return true
    }

    private fun select(args: String) {
        val current = client.sessions.value
        val pair = args.split(Regex("\\s+"), limit = 2)
        val session =
            if (pair.size == 1) {
                if (listed != current) null else pair[0].toIntOrNull()?.let { listed.getOrNull(it - 1) }
            } else {
                current.singleOrNull { it.hostId == pair[0] && it.resource == pair[1] }
            }
        when {
            client.selectedSession.value != null -> terminal.line("error: use sessions before selecting again")
            session == null -> terminal.line("error: unknown or stale session; use the current session list")
            client.hostProgress.value.none {
                it.id == session.hostId && it.progress.stage == ProgressStage.CatalogReady
            } -> terminal.line("error: host catalogue is not ready")
            else -> {
                client.selectSession(session)
                terminal.line("selected ${Redaction.endpoint(session.hostId)} ${Redaction.endpoint(session.resource)}")
            }
        }
    }

    private fun listSessions() {
        listed = client.sessions.value
        terminal.line("sessions: ${listed.size}")
        listed.forEachIndexed { index, session ->
            terminal.line(
                "${index + 1}: ${Redaction.endpoint(session.hostId)} ${Redaction.endpoint(session.resource)} " +
                    "${session.hostLabel} | ${session.title}",
            )
        }
    }

    private fun discoveryText(discovery: DiscoveryProgress): String =
        "discovery: ${discovery.stage} hosts=${discovery.hostCount}${diagnosticText(discovery.diagnostic)}"

    private fun hostText(host: HostStatus): String =
        "host ${Redaction.endpoint(host.id)} (${host.label}): ${host.progress.stage}" +
            (host.progress.failedDuring?.let { " during $it" } ?: "") + diagnosticText(host.progress.diagnostic)

    private fun diagnosticText(diagnostic: Diagnostic?): String =
        diagnostic?.let { " | ${it.operation}: ${it.summary}" } ?: ""

    private companion object {
        private val HELP =
            listOf(
                "connect <endpoint>        discover hosts and connect",
                "disconnect                close every host connection",
                "help                      show this list",
                "quit                      close connections and exit (also EOF or Ctrl+C)",
                "select <n>                select a session from the current list",
                "select <host> <resource>  select a session by host id and resource",
                "send <text>               queue a turn on the selected session's default chat",
                "sessions                  reconnect and list sessions again",
                "show                      print the current transcript",
                "status                    show discovery and per-host progress",
            )
    }
}

class Terminal(private val output: PrintWriter) {
    private var openRow: String? = null
    private var previous: Map<String, UiMessage> = emptyMap()

    @Synchronized
    fun line(text: String) {
        finish()
        output.println(printable(text))
        output.flush()
    }

    @Synchronized
    fun finish() {
        if (openRow != null) {
            output.println()
            output.flush()
            openRow = null
        }
    }

    @Synchronized
    fun render(rows: List<UiMessage>) {
        if (rows.isEmpty()) finish()
        for (row in rows) {
            val old = previous[row.id]
            if (row == old) continue
            val suffix =
                if (old != null &&
                    row.text.startsWith(old.text)
                ) {
                    row.text.substring(old.text.length)
                } else {
                    row.text
                }
            if (old == null || suffix.isNotEmpty()) {
                if (openRow != row.id) {
                    finish()
                    output.print("${printable(row.role)}: ")
                }
                output.print(printable(suffix))
                output.flush()
                openRow = row.id
            }
            if (!row.streaming) finish()
        }
        previous = rows.associateBy { it.id }
    }

    private fun printable(text: String): String = text.filter { !it.isISOControl() || it == '\n' || it == '\t' }
}
