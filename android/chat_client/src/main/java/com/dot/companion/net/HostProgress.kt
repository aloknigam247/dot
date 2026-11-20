package com.dot.companion.net

/**
 * Diagnostic lifecycle stage of one host connection. Independent of [ConnectionState], which keeps its
 * socket-level meaning: [CatalogReady] means the session list arrived (possibly empty) and [ChatReady] means
 * the selected session and its chats are subscribed with an outbound chat available.
 */
enum class ProgressStage {
    Idle,
    Connecting,
    Initializing,
    Listing,
    CatalogReady,
    Subscribing,
    ChatReady,
    Failed,
    Closed,
}

/** A credential-safe description of a problem: the affected operation and a sanitized summary. */
data class Diagnostic(val operation: String, val summary: String)

/** One host connection's [stage]; [failedDuring] records the stage that a [ProgressStage.Failed] interrupted. */
data class HostProgress(
    val stage: ProgressStage = ProgressStage.Idle,
    val failedDuring: ProgressStage? = null,
    val diagnostic: Diagnostic? = null,
)

/** A host's identity with its [progress], as published by [com.dot.companion.ChatClient]. */
data class HostStatus(val id: String, val label: String, val progress: HostProgress)

enum class DiscoveryStage { Idle, Fetching, Catalogue, Empty, DirectFallback }

/** How the endpoint was resolved: a cptower catalogue of [hostCount] hosts, none, or a direct connection. */
data class DiscoveryProgress(
    val stage: DiscoveryStage = DiscoveryStage.Idle,
    val hostCount: Int = 0,
    val diagnostic: Diagnostic? = null,
)

/** Strips credentials and query values before endpoints or failures reach a diagnostic. */
object Redaction {
    private val userInfo = Regex("^([a-zA-Z][a-zA-Z0-9+.-]*://)?[^/@?#\\s]*@")

    fun endpoint(value: String): String {
        val bare = value.trim().replaceFirst(userInfo, "$1").substringBefore('#')
        val query = bare.substringAfter('?', missingDelimiterValue = "")
        if (query.isEmpty()) return bare.substringBefore('?')
        val redacted = query.split('&').joinToString("&") { if ('=' in it) it.substringBefore('=') + "=***" else it }
        return bare.substringBefore('?') + "?" + redacted
    }

    // Even transport exception messages may contain response headers or secret-bearing payloads.
    fun failure(error: Throwable): String = error.javaClass.simpleName.ifEmpty { "Exception" }
}
