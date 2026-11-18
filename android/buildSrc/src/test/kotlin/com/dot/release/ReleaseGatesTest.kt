package com.dot.release

import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class ReleaseGatesTest {
    @Test
    fun requiresStableTagMatchingTheAppVersion() {
        requireReleaseTag("android-v1.2.0", "1.2.0")
        for (tag in listOf("android-v1.2.1", "android-v1.2.0-rc.1")) {
            assertThrows(IllegalArgumentException::class.java) { requireReleaseTag(tag, "1.2.0") }
        }
    }

    @Test
    fun allowsUnsignedBuildOnlyWhenSigningIsOptionalAndNoInputsWereSet() {
        assertNull(SigningInputs.resolve({ null }, required = false))
        assertThrows(IllegalArgumentException::class.java) { SigningInputs.resolve({ null }, required = true) }
        assertThrows(IllegalArgumentException::class.java) {
            SigningInputs.resolve({ if (it == SigningInputs.KEY_ALIAS) "release" else null }, required = false)
        }
    }
}
