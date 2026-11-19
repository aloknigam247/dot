package com.dot.companion

import android.util.Log
import com.dot.companion.net.ChatLogger

object AndroidChatLogger : ChatLogger {
    override fun error(message: String, cause: Throwable?) {
        Log.e("AhpConn", message, cause)
    }
}
