package com.dot.release

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipFile

private const val CLASS_DEF_SIZE = 32
private const val HEADER_SIZE = 0x70
private val dexEntry = Regex("classes[0-9]*\\.dex")
private val dexMagic = "dex\n".toByteArray(Charsets.US_ASCII)

/** Binary names (`a.b.C$D`) of every class defined in the APK's top-level `classes*.dex` files. */
fun apkDexClassNames(apk: File): Set<String> =
    ZipFile(apk).use { zip ->
        val entries =
            zip
                .entries()
                .asSequence()
                .filter { dexEntry.matches(it.name) }
                .toList()
        require(entries.isNotEmpty()) { "${apk.name} contains no classes*.dex" }
        entries.flatMapTo(HashSet()) { entry -> dexClassNames(zip.getInputStream(entry).use { it.readBytes() }) }
    }

/** Binary names of the classes a DEX file defines (its class_defs), excluding types it only references. */
fun dexClassNames(dex: ByteArray): Set<String> {
    require(dex.size >= HEADER_SIZE && dex.copyOfRange(0, 4).contentEquals(dexMagic) && dex[7] == 0.toByte()) {
        "not a DEX file"
    }
    val buffer = ByteBuffer.wrap(dex).order(ByteOrder.LITTLE_ENDIAN)
    val classDefsOff = buffer.u32(0x64)
    val classDefsSize = buffer.u32(0x60)
    val stringIdsOff = buffer.u32(0x3C)
    val stringIdsSize = buffer.u32(0x38)
    val typeIdsOff = buffer.u32(0x44)
    val typeIdsSize = buffer.u32(0x40)
    buffer.requireTable(classDefsOff, classDefsSize, CLASS_DEF_SIZE)
    buffer.requireTable(stringIdsOff, stringIdsSize, 4)
    buffer.requireTable(typeIdsOff, typeIdsSize, 4)
    return (0 until classDefsSize).mapTo(HashSet(classDefsSize)) { index ->
        val typeIndex = buffer.u32(classDefsOff + index * CLASS_DEF_SIZE)
        require(typeIndex < typeIdsSize) { "DEX class_def $index has an invalid type index" }
        val stringIndex = buffer.u32(typeIdsOff + typeIndex * 4)
        require(stringIndex < stringIdsSize) { "DEX type $typeIndex has an invalid string index" }
        binaryName(readMutf8(dex, buffer.u32(stringIdsOff + stringIndex * 4)))
    }
}

private fun ByteBuffer.requireTable(offset: Int, size: Int, stride: Int) {
    require(offset <= limit() && size <= (limit() - offset) / stride) { "truncated DEX file" }
}

private fun ByteBuffer.u32(offset: Int): Int {
    require(offset >= 0 && offset <= limit() - 4) { "truncated DEX file" }
    val value = getInt(offset)
    require(value >= 0) { "DEX offset or size out of range" }
    return value
}

private fun binaryName(descriptor: String): String {
    require(descriptor.length > 2 && descriptor.startsWith("L") && descriptor.endsWith(";")) {
        "DEX class descriptor '$descriptor' is not a class type"
    }
    return descriptor.substring(1, descriptor.length - 1).replace('/', '.')
}

private fun readMutf8(dex: ByteArray, offset: Int): String {
    var position = offset

    fun next(): Int {
        require(position in dex.indices) { "truncated DEX string" }
        return dex[position++].toInt() and 0xFF
    }

    fun continuation(): Int {
        val byte = next()
        require(byte and 0xC0 == 0x80) { "invalid MUTF-8 in DEX string" }
        return byte and 0x3F
    }

    // Skips the ULEB128 UTF-16 length prefix; the NUL terminator delimits the string data.
    do {
        val lengthByte = next()
    } while (lengthByte and 0x80 != 0)
    val text = StringBuilder()
    while (true) {
        val lead = next()
        when {
            lead == 0 -> return text.toString()
            lead < 0x80 -> text.append(lead.toChar())
            lead and 0xE0 == 0xC0 -> text.append((((lead and 0x1F) shl 6) or continuation()).toChar())
            lead and 0xF0 == 0xE0 -> {
                val middle = continuation()
                text.append((((lead and 0x0F) shl 12) or (middle shl 6) or continuation()).toChar())
            }
            else -> throw IllegalArgumentException("invalid MUTF-8 in DEX string")
        }
    }
}
