package com.dot.companion.net

import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import okhttp3.Call
import okhttp3.Callback
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import java.io.IOException
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** One AHP host advertised by cptower's `/hosts` discovery endpoint. */
@Serializable
data class CatalogHost(
    val id: String,
    val port: Int,
    val label: String = "",
    val protocol: String? = null,
    val sessions: Int = 0,
)

/** A catalogue request failure; [summary] is credential-safe and never contains the endpoint. */
class CatalogException(val operation: String, val summary: String, cause: Throwable? = null) :
    IOException("$operation: $summary", cause)

/**
 * Reads the cptower multiplexer's host catalog from `GET http://<base>/hosts`.
 *
 * cptower discovers every live Copilot AHP host on the machine and exposes each one
 * behind a `/ws/<port>` route on a single external port. The app fetches this catalog
 * to learn which hosts exist, then opens one [AhpConnection] per host. A failed or
 * non-JSON response means the endpoint is not a cptower instance (e.g. a bare AHP
 * host), which the caller treats as "connect directly" rather than an error.
 * Cancelling the calling coroutine cancels the HTTP call.
 */
class HostCatalog(private val client: OkHttpClient = OkHttpClient()) {
    suspend fun fetch(base: String): List<CatalogHost> {
        val body = get(Request.Builder().url(hostsUrl(base)).build())
        return try {
            json.decodeFromString<List<CatalogHost>>(body)
        } catch (e: IllegalArgumentException) {
            throw CatalogException(OPERATION, "response is not a host catalogue", e)
        }
    }

    private suspend fun get(request: Request): String =
        suspendCancellableCoroutine { continuation ->
            val call = client.newCall(request)
            continuation.invokeOnCancellation { call.cancel() }
            call.enqueue(
                object : Callback {
                    override fun onFailure(call: Call, e: IOException) {
                        continuation.resumeWithException(CatalogException(OPERATION, Redaction.failure(e), e))
                    }

                    override fun onResponse(call: Call, response: Response) {
                        try {
                            continuation.resume(response.use(::read))
                        } catch (e: IOException) {
                            val failure = e as? CatalogException ?: CatalogException(OPERATION, Redaction.failure(e), e)
                            continuation.resumeWithException(failure)
                        }
                    }
                },
            )
        }

    private fun read(response: Response): String {
        if (!response.isSuccessful) throw CatalogException(OPERATION, "HTTP ${response.code}")
        val body = response.body?.string()
        if (body.isNullOrBlank()) throw CatalogException(OPERATION, "empty response")
        return body
    }

    private fun hostsUrl(base: String): String {
        val trimmed = base.trim().trimEnd('/')
        val withScheme =
            when {
                trimmed.startsWith("ws://") -> "http://" + trimmed.removePrefix("ws://")
                trimmed.startsWith("wss://") -> "https://" + trimmed.removePrefix("wss://")
                trimmed.startsWith("http://") || trimmed.startsWith("https://") -> trimmed
                else -> "http://$trimmed"
            }
        return "$withScheme/hosts"
    }

    private companion object {
        const val OPERATION = "GET /hosts"
        val json = Json { ignoreUnknownKeys = true }
    }
}
