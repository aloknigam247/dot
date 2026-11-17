package com.dot.release

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class DexClassesTest {
    @get:Rule
    val temp = TemporaryFolder()

    @Test
    fun readsDefinedClassesButNotReferencedTypes() {
        val dex =
            dex(
                classes = listOf("Lcom/dot/A;", "Lcom/dot/A\$\$serializer;"),
                referenced = listOf("Ljava/lang/Object;"),
            )

        assertEquals(setOf("com.dot.A", "com.dot.A\$\$serializer"), dexClassNames(dex))
    }

    @Test
    fun decodesMultiByteMutf8Descriptors() {
        val name = "com.dot.Caf\u00e9\u20ac\uD83D\uDE00"

        assertEquals(setOf(name), dexClassNames(dex(classes = listOf("L${name.replace('.', '/')};"))))
    }

    @Test
    fun readsLongDescriptorsWithMultiByteLengthPrefixes() {
        val name = "com.dot.${"A".repeat(160)}"

        assertEquals(setOf(name), dexClassNames(dex(classes = listOf("L${name.replace('.', '/')};"))))
    }

    @Test
    fun acceptsEmptyClassTables() {
        assertEquals(emptySet<String>(), dexClassNames(dex(classes = emptyList())))
    }

    @Test
    fun rejectsNonDexBytes() {
        val error = assertThrows(IllegalArgumentException::class.java) { dexClassNames(ByteArray(0x70)) }

        assertEquals("not a DEX file", error.message)
    }

    @Test
    fun rejectsClassDefsPastTheEndOfTheFile() {
        val dex = dex(classes = listOf("Lcom/dot/A;"))
        ByteBuffer.wrap(dex).order(ByteOrder.LITTLE_ENDIAN).putInt(0x60, 1000)

        val error = assertThrows(IllegalArgumentException::class.java) { dexClassNames(dex) }

        assertEquals("truncated DEX file", error.message)
    }

    @Test
    fun rejectsInvalidTypeAndStringIndices() {
        listOf(0x44, 0x64).forEach { tableOffset ->
            val dex = dex(classes = listOf("Lcom/dot/A;"))
            val buffer = ByteBuffer.wrap(dex).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(buffer.getInt(tableOffset), 1)

            assertThrows(IllegalArgumentException::class.java) { dexClassNames(dex) }
        }
    }

    @Test
    fun rejectsNegativeAndTruncatedTableOffsets() {
        listOf(-1, Int.MAX_VALUE).forEach { offset ->
            val dex = dex(classes = listOf("Lcom/dot/A;"))
            ByteBuffer.wrap(dex).order(ByteOrder.LITTLE_ENDIAN).putInt(0x64, offset)

            assertThrows(IllegalArgumentException::class.java) { dexClassNames(dex) }
        }
    }

    @Test
    fun rejectsNonClassDescriptorsAndUnterminatedStrings() {
        listOf(
            dex(classes = listOf("I")),
            dex(classes = listOf("Lcom/dot/A;")).dropLast(1).toByteArray(),
        ).forEach { invalid ->
            assertThrows(IllegalArgumentException::class.java) { dexClassNames(invalid) }
        }
    }

    @Test
    fun rejectsInvalidMutf8LeadAndContinuationBytes() {
        listOf(false, true).forEach { corruptContinuation ->
            val dex = dex(classes = listOf("Lcom/dot/Caf\u00e9;"))
            val index = dex.indexOf(0xC3.toByte())
            if (corruptContinuation) dex[index + 1] = 0x20 else dex[index] = 0xFF.toByte()

            assertThrows(IllegalArgumentException::class.java) { dexClassNames(dex) }
        }
    }

    @Test
    fun apkMergesTopLevelDexFilesOnly() {
        val apk =
            apk(
                "classes.dex" to dex(classes = listOf("Lcom/dot/A;")),
                "classes2.dex" to dex(classes = listOf("Lcom/dot/B;")),
                "assets/classes3.dex" to dex(classes = listOf("Lcom/dot/C;")),
            )

        assertEquals(setOf("com.dot.A", "com.dot.B"), apkDexClassNames(apk))
    }

    @Test
    fun apkWithoutDexIsRejected() {
        val apk = apk("AndroidManifest.xml" to ByteArray(1))

        assertThrows(IllegalArgumentException::class.java) { apkDexClassNames(apk) }
    }

    private fun apk(vararg entries: Pair<String, ByteArray>) =
        temp.newFile("app.apk").also { file ->
            ZipOutputStream(file.outputStream()).use { zip ->
                entries.forEach { (name, bytes) ->
                    zip.putNextEntry(ZipEntry(name))
                    zip.write(bytes)
                    zip.closeEntry()
                }
            }
        }

    /**
     * A minimal DEX holding only what the reader uses: header offsets, string_ids, type_ids, and class_defs. Referenced
     * types come first so a class_def's type index differs from its position.
     */
    private fun dex(classes: List<String>, referenced: List<String> = emptyList()): ByteArray {
        val stringIdsOff = 0x70
        val descriptors = referenced + classes
        val strings = descriptors.map(::dexString)
        val typeIdsOff = stringIdsOff + descriptors.size * 4
        val classDefsOff = typeIdsOff + descriptors.size * 4
        var dataOff = classDefsOff + classes.size * 32
        val buffer = ByteBuffer.allocate(dataOff + strings.sumOf { it.size }).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put("dex\n035\u0000".toByteArray(Charsets.US_ASCII))
        buffer.putInt(0x38, descriptors.size).putInt(0x3C, stringIdsOff)
        buffer.putInt(0x40, descriptors.size).putInt(0x44, typeIdsOff)
        buffer.putInt(0x60, classes.size).putInt(0x64, classDefsOff)
        strings.forEachIndexed { index, bytes ->
            buffer.putInt(stringIdsOff + index * 4, dataOff)
            buffer.putInt(typeIdsOff + index * 4, index)
            bytes.forEachIndexed { offset, byte -> buffer.put(dataOff + offset, byte) }
            dataOff += bytes.size
        }
        classes.indices.forEach { index -> buffer.putInt(classDefsOff + index * 32, referenced.size + index) }
        return buffer.array()
    }

    private fun dexString(descriptor: String): ByteArray {
        val length = ByteArrayOutputStream()
        val text = ByteArrayOutputStream()
        var remaining = descriptor.length
        do {
            val byte = remaining and 0x7F
            remaining = remaining ushr 7
            length.write(if (remaining == 0) byte else byte or 0x80)
        } while (remaining != 0)
        DataOutputStream(text).use { it.writeUTF(descriptor) }
        val encoded = text.toByteArray()
        return length.toByteArray() + encoded.copyOfRange(2, encoded.size) + 0.toByte()
    }
}
