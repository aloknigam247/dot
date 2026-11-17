package com.dot.release

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ReleaseGatesTest {
    private val fingerprint = "ab".repeat(32)

    @Test
    fun readsClassMappingsWithoutMembersOrMetadata() {
        val mapping =
            "# compiler: R8\r\n" +
                "com.dot.A -> a.b:\r\n" +
                "    int count -> c\r\n" +
                "    1:2:void call():3:4 -> d\r\n" +
                "com.dot.A\$\$serializer -> e.f:\r\n"

        assertEquals(
            mapOf("com.dot.A" to "a.b", "com.dot.A\$\$serializer" to "e.f"),
            r8ClassMapping(mapping),
        )
    }

    @Test
    fun rejectsMalformedClassMapping() {
        listOf("com.dot.A", "com.dot.A -> a.b").forEach { mapping ->
            assertThrows(IllegalArgumentException::class.java) { r8ClassMapping(mapping) }
        }
    }

    @Test
    fun distinguishesRemovedClassesFromRemovedMembers() {
        val usage =
            "# R8 usage\r\n\r\n" +
                "com.dot.Removed\r\n" +
                "com.dot.Retained:\r\n" +
                "    public void unused()\r\n"

        assertEquals(setOf("com.dot.Removed"), r8RemovedClasses(usage))
    }

    @Test
    fun requiresBothAMappingAndADefinedDexClass() {
        val required = listOf("kept", "removed", "mappedButAbsent", "optimizedAway")
        val mapping = mapOf("kept" to "a", "mappedButAbsent" to "b", "optimizedAway" to "a")

        assertEquals(
            listOf("removed", "mappedButAbsent", "optimizedAway"),
            missingClasses(required, mapping, setOf("a", "removed"), setOf("optimizedAway")),
        )
        assertTrue(missingClasses(listOf("kept"), mapping, setOf("a"), emptySet()).isEmpty())
    }

    @Test
    fun acceptsStableTagsMatchingTheAppVersion() {
        listOf("0.0.0", "1.2.3", "10.20.300").forEach { version ->
            assertEquals(version, releaseTagVersion("android-v$version"))
            requireReleaseTag("android-v$version", version)
        }
    }

    @Test
    fun rejectsMalformedPrereleaseAndNonAndroidTags() {
        listOf(
            "",
            "android-v01.2.0",
            "android-v1.02.3",
            "android-v1.2",
            "android-v1.2.03",
            "android-v1.2.3\n",
            "android-v1.2.3+build",
            "android-v1.2.3-rc.1",
            "android-v1.2.3.4",
            "dot-v1.2.3",
        ).forEach { tag ->
            assertThrows("tag: $tag", IllegalArgumentException::class.java) { releaseTagVersion(tag) }
        }
    }

    @Test
    fun rejectsTagVersionMismatch() {
        assertThrows(IllegalArgumentException::class.java) { requireReleaseTag("android-v1.2.3", "1.2.4") }
    }

    @Test
    fun normalizesPlainAndKeytoolFingerprints() {
        assertEquals(fingerprint, normalizeSha256(fingerprint))
        assertEquals(fingerprint, normalizeSha256(" ${fingerprint.chunked(2).joinToString(":").uppercase()}\r\n"))
    }

    @Test
    fun rejectsMissingShortLongAndNonHexFingerprints() {
        listOf("", "ab", "ab".repeat(33), "gz".repeat(32)).forEach { value ->
            assertThrows(IllegalArgumentException::class.java) { normalizeSha256(value) }
        }
    }

    @Test
    fun matchesOneSignerWithWindowsLineEndingsAndIgnoresSourceStamps() {
        val output =
            "Signer #1 certificate SHA-256 digest: ${fingerprint.uppercase()}\r\n" +
                "Signer #1 certificate SHA-1 digest: ignored\r\n" +
                "Source Stamp Signer certificate SHA-256 digest: ${"cd".repeat(32)}\r\n"

        requireSingleSigner(output, fingerprint)
        requireSingleSigner("Signer #1 certificate SHA-256 digest: $fingerprint\n", fingerprint)
    }

    @Test
    fun rejectsWrongMissingAndMultipleSigners() {
        listOf(
            "",
            "Signer #1 certificate SHA-256 digest: ${"cd".repeat(32)}",
            "Signer #1 certificate SHA-256 digest: $fingerprint\n" +
                "Signer #2 certificate SHA-256 digest: $fingerprint\n",
        ).forEach { output ->
            assertThrows(IllegalArgumentException::class.java) { requireSingleSigner(output, fingerprint) }
        }
    }

    @Test
    fun noSigningInputsProducesUnsignedUnlessRequired() {
        assertNull(SigningInputs.resolve({ null }, required = false))
        assertThrows(IllegalArgumentException::class.java) { SigningInputs.resolve({ null }, required = true) }
    }

    @Test
    fun acceptsCompleteSigningInputsWithoutExposingPasswords() {
        val values = signingValues()
        val inputs = requireNotNull(SigningInputs.resolve(values::get, required = true))

        assertEquals(fingerprint, inputs.certSha256)
        assertEquals("release", inputs.keyAlias)
        assertEquals("key-secret", inputs.keyPassword)
        assertEquals("release.p12", inputs.storeFile)
        assertEquals("store-secret", inputs.storePassword)
        assertFalse(inputs.toString().contains("key-secret"))
        assertFalse(inputs.toString().contains("store-secret"))
        assertEquals(fingerprint, SigningInputs.resolve(values::get, required = false)?.certSha256)
    }

    @Test
    fun everyMissingOrBlankSigningInputFailsWithoutExposingValues() {
        val complete = signingValues()
        complete.keys.forEach { name ->
            listOf(null, "", " ").forEach { missing ->
                val values = complete + (name to missing)
                val error =
                    assertThrows(IllegalArgumentException::class.java) {
                        SigningInputs.resolve(values::get, required = false)
                    }
                val message = requireNotNull(error.message)
                assertTrue(message.contains(name))
                assertFalse(message.contains("key-secret"))
                assertFalse(message.contains("store-secret"))
            }
        }
    }

    @Test
    fun allBlankInputsAreUnsignedOnlyWhenOptional() {
        assertNull(SigningInputs.resolve({ " " }, required = false))
        assertThrows(IllegalArgumentException::class.java) { SigningInputs.resolve({ "" }, required = true) }
    }

    @Test
    fun rejectsInvalidExpectedFingerprintBeforeSigning() {
        val values = signingValues() + (SigningInputs.CERT_SHA256 to "invalid")

        assertThrows(IllegalArgumentException::class.java) { SigningInputs.resolve(values::get, required = true) }
    }

    private fun signingValues() =
        mapOf(
            SigningInputs.CERT_SHA256 to fingerprint,
            SigningInputs.KEY_ALIAS to "release",
            SigningInputs.KEY_PASSWORD to "key-secret",
            SigningInputs.STORE_FILE to "release.p12",
            SigningInputs.STORE_PASSWORD to "store-secret",
        )
}
