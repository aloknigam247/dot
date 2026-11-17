package com.dot.release

private val releaseTagPattern = Regex("android-v((?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*))")
private val sha256Hex = Regex("[0-9a-f]{64}")
private val signerDigest = Regex("^Signer #[0-9]+ certificate SHA-256 digest: ([0-9A-Fa-f]+)$", RegexOption.MULTILINE)

/** Original-to-obfuscated class names from an R8 `mapping.txt`. */
fun r8ClassMapping(mapping: String): Map<String, String> =
    mapping
        .lineSequence()
        .filter { it.isNotEmpty() && !it[0].isWhitespace() && !it.startsWith("#") }
        .associate { line ->
            val arrow = line.indexOf(" -> ")
            require(arrow > 0 && line.endsWith(":")) { "unrecognized R8 mapping line: $line" }
            line.substring(0, arrow) to line.substring(arrow + 4, line.length - 1)
        }

fun r8RemovedClasses(usage: String): Set<String> =
    usage
        .lineSequence()
        .filter { it.isNotEmpty() && !it[0].isWhitespace() && !it.startsWith("#") && !it.endsWith(":") }
        .toSet()

/** Required classes that R8 removed, or whose mapped name is not defined in the packaged DEX. */
fun missingClasses(
    required: Collection<String>,
    mapping: Map<String, String>,
    dexClasses: Set<String>,
    removedClasses: Set<String>,
): List<String> = required.filter { it in removedClasses || mapping[it]?.let(dexClasses::contains) != true }

/** The MAJOR.MINOR.PATCH version of a stable `android-v<MAJOR>.<MINOR>.<PATCH>` tag. */
fun releaseTagVersion(tag: String): String =
    requireNotNull(releaseTagPattern.matchEntire(tag)) {
        "release tag '$tag' is not android-v<MAJOR>.<MINOR>.<PATCH>"
    }.groupValues[1]

fun requireReleaseTag(tag: String, versionName: String) {
    val version = releaseTagVersion(tag)
    require(version == versionName) { "release tag $tag does not match versionName $versionName" }
}

/** Lower-case hex SHA-256 from a fingerprint written as plain hex or keytool's colon-separated form. */
fun normalizeSha256(fingerprint: String): String {
    val hex = fingerprint.filterNot { it == ':' || it.isWhitespace() }.lowercase()
    require(sha256Hex.matches(hex)) { "expected a SHA-256 certificate fingerprint of 64 hex digits" }
    return hex
}

/** Fails unless `apksigner verify --print-certs` reports exactly one signer with the expected certificate. */
fun requireSingleSigner(apksignerOutput: String, expectedSha256: String) {
    val expected = normalizeSha256(expectedSha256)
    val digests = signerDigest.findAll(apksignerOutput).map { it.groupValues[1].lowercase() }.toList()
    require(digests.size == 1) { "expected exactly one APK signer, apksigner reported ${digests.size}" }
    require(digests.single() == expected) {
        "APK signer certificate SHA-256 ${digests.single()} does not match the expected $expected"
    }
}

/** Release signing inputs. Passwords never appear in messages or [toString]. */
class SigningInputs private constructor(
    val certSha256: String,
    val keyAlias: String,
    val keyPassword: String,
    val storeFile: String,
    val storePassword: String,
) {
    override fun toString(): String = "SigningInputs(storeFile=$storeFile, keyAlias=$keyAlias, certSha256=$certSha256)"

    companion object {
        const val CERT_SHA256 = "DOT_ANDROID_CERT_SHA256"
        const val KEY_ALIAS = "DOT_ANDROID_KEY_ALIAS"
        const val KEY_PASSWORD = "DOT_ANDROID_KEY_PASSWORD"
        const val STORE_FILE = "DOT_ANDROID_KEYSTORE"
        const val STORE_PASSWORD = "DOT_ANDROID_KEYSTORE_PASSWORD"
        private val names = listOf(CERT_SHA256, KEY_ALIAS, KEY_PASSWORD, STORE_FILE, STORE_PASSWORD)

        /**
         * All inputs set: signed release. None set: unsigned release, unless [required]. Blank values count as
         * unset, so an empty CI secret cannot silently produce an unsigned build when signing is required.
         */
        fun resolve(lookup: (String) -> String?, required: Boolean): SigningInputs? {
            val values = names.associateWith { name -> lookup(name)?.takeIf(String::isNotBlank) }
            val missing = values.filterValues { it == null }.keys
            if (missing.size == names.size && !required) return null
            require(missing.isEmpty()) { "release signing needs ${missing.joinToString()}" }
            return SigningInputs(
                certSha256 = normalizeSha256(values.getValue(CERT_SHA256)!!),
                keyAlias = values.getValue(KEY_ALIAS)!!,
                keyPassword = values.getValue(KEY_PASSWORD)!!,
                storeFile = values.getValue(STORE_FILE)!!,
                storePassword = values.getValue(STORE_PASSWORD)!!,
            )
        }
    }
}
