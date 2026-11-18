package com.dot.release

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class ReleaseGatesTest {
    @Test
    fun requiresRetainedSerializersInThePackagedDex() {
        val mapping = r8ClassMapping("Kept -> a:\nRemoved -> a:\nAbsent -> b:")
        val removed = r8RemovedClasses("Removed\nKept:\n    void unused()")

        assertEquals(
            listOf("Removed", "Absent"),
            missingClasses(listOf("Kept", "Removed", "Absent"), mapping, setOf("a"), removed),
        )
    }

    @Test
    fun requiresStableTagMatchingTheAppVersion() {
        requireReleaseTag("android-v1.2.0", "1.2.0")
        for (tag in listOf("android-v1.2.1", "android-v1.2.0-rc.1")) {
            assertThrows(IllegalArgumentException::class.java) { requireReleaseTag(tag, "1.2.0") }
        }
    }

    @Test
    fun requiresOneSignerWithThePinnedCertificate() {
        val expected = "ab".repeat(32)
        val signer = "Signer #1 certificate SHA-256 digest: $expected"

        requireSingleSigner(signer, expected)
        assertThrows(IllegalArgumentException::class.java) { requireSingleSigner(signer, "cd".repeat(32)) }
        assertThrows(IllegalArgumentException::class.java) {
            requireSingleSigner("$signer\n${signer.replace("#1", "#2")}", expected)
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
