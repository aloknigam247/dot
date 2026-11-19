package com.dot.companion.net

fun interface ChatLogger {
    fun error(message: String, cause: Throwable?)
}
