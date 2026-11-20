package com.dot.companion.net

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import okhttp3.Call
import okhttp3.EventListener
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.SocketPolicy
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class HostCatalogTest {
    private val cancelled = CountDownLatch(1)
    private val client =
        OkHttpClient
            .Builder()
            .eventListener(
                object : EventListener() {
                    override fun canceled(call: Call) {
                        cancelled.countDown()
                    }
                },
            ).build()
    private val server = MockWebServer()

    @After
    fun tearDown() {
        server.shutdown()
        client.dispatcher.executorService.shutdown()
        client.connectionPool.evictAll()
    }

    @Test
    fun `catalogue uses exact Android destination and preserves empty result`() =
        runBlocking {
            val base = server.url("/base").toString()
            for (endpoint in listOf(base, base.replace("http:", "ws:"), base.removePrefix("http://"))) {
                server.enqueue(MockResponse().setBody("[]"))
                assertTrue(HostCatalog(client).fetch(" $endpoint/ ").isEmpty())
                assertEquals("/base/hosts", server.takeRequest(5, TimeUnit.SECONDS)?.path)
            }
            server.enqueue(MockResponse().setBody("""[{"id":"h1","port":1234,"label":"Host","ignored":true}]"""))
            assertEquals(listOf(CatalogHost("h1", 1234, "Host")), HostCatalog(client).fetch(base))
        }

    @Test
    fun `HTTP and malformed catalogues have credential-safe diagnostics`() =
        runBlocking {
            val base =
                server
                    .url("/")
                    .newBuilder()
                    .username("user")
                    .password("secret")
                    .build()
                    .toString()
            val cases =
                listOf(
                    MockResponse().setResponseCode(401).setBody("secret") to "HTTP 401",
                    MockResponse().setBody("") to "empty response",
                    MockResponse().setBody("secret") to "response is not a host catalogue",
                )
            for ((response, summary) in cases) {
                server.enqueue(response)
                val failure = runCatching { HostCatalog(client).fetch(base) }.exceptionOrNull()
                assertTrue(failure is CatalogException)
                assertEquals("GET /hosts", (failure as CatalogException).operation)
                assertEquals(summary, failure.summary)
                assertFalse(failure.message.orEmpty().contains("secret"))
            }
        }

    @Test
    fun `cancelling discovery cancels its actual HTTP call`() =
        runBlocking {
            server.enqueue(MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE))
            val base = server.url("/").toString()
            val pending = async(Dispatchers.IO) { HostCatalog(client).fetch(base) }
            assertEquals("/hosts", server.takeRequest(5, TimeUnit.SECONDS)?.path)
            pending.cancelAndJoin()
            assertTrue(cancelled.await(5, TimeUnit.SECONDS))
        }
}
