package com.dot.companion.cli

import com.dot.companion.ChatClient
import com.dot.companion.net.AhpConnection
import com.dot.companion.net.ChatLogger
import com.dot.companion.net.HostCatalog
import com.dot.companion.net.Redaction
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import java.io.IOException
import java.io.PrintWriter
import java.nio.charset.Charset
import java.util.concurrent.Executors

fun main() {
    var closed = false
    val lock = Any()
    val terminal = Terminal(PrintWriter(System.out, true))
    val dispatcher =
        Executors
            .newSingleThreadExecutor { task -> Thread(task, "dot-chat").apply { isDaemon = true } }
            .asCoroutineDispatcher()
    val errors = CoroutineExceptionHandler { _, error -> terminal.line("error: ${Redaction.failure(error)}") }
    val scope = CoroutineScope(dispatcher + SupervisorJob() + errors)
    val http = AhpConnection.defaultClient()
    val catalog = HostCatalog(http)
    val client = ChatClient(scope, { AhpConnection(it, ChatLogger { _, _ -> }, http) }, catalog::fetch)
    val cli = ChatCli(client, scope, terminal)

    fun shutdown() {
        synchronized(lock) {
            if (closed) return
            closed = true
            try {
                runBlocking(dispatcher) {
                    cli.close()
                    scope.cancel()
                }
            } finally {
                http.dispatcher.executorService.shutdown()
                http.connectionPool.evictAll()
                dispatcher.close()
            }
        }
    }

    val hook = Thread(::shutdown, "dot-chat-shutdown")
    Runtime.getRuntime().addShutdownHook(hook)
    try {
        val encoding = Charset.forName(System.getProperty("stdin.encoding", Charset.defaultCharset().name()))
        cli.run(System.`in`.bufferedReader(encoding), dispatcher)
    } catch (error: IOException) {
        terminal.line("error: stdin ${Redaction.failure(error)}")
    } finally {
        shutdown()
    }
}
