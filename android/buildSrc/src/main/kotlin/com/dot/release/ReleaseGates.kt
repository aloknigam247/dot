package com.dot.release

private val releaseTagPattern = Regex("android-v((?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*))")

/** The MAJOR.MINOR.PATCH version of a stable `android-v<MAJOR>.<MINOR>.<PATCH>` tag. */
fun releaseTagVersion(tag: String): String =
    requireNotNull(releaseTagPattern.matchEntire(tag)) {
        "release tag '$tag' is not android-v<MAJOR>.<MINOR>.<PATCH>"
    }.groupValues[1]

fun requireReleaseTag(tag: String, versionName: String) {
    val version = releaseTagVersion(tag)
    require(version == versionName) { "release tag $tag does not match versionName $versionName" }
}

/** Release signing inputs. Passwords never appear in messages or [toString]. */
class SigningInputs private constructor(
    val keyAlias: String,
    val keyPassword: String,
    val storeFile: String,
    val storePassword: String,
) {
    override fun toString(): String = "SigningInputs(storeFile=$storeFile, keyAlias=$keyAlias)"

    companion object {
        const val KEY_ALIAS = "DOT_ANDROID_KEY_ALIAS"
        const val KEY_PASSWORD = "DOT_ANDROID_KEY_PASSWORD"
        const val STORE_FILE = "DOT_ANDROID_KEYSTORE"
        const val STORE_PASSWORD = "DOT_ANDROID_KEYSTORE_PASSWORD"
        private val names = listOf(KEY_ALIAS, KEY_PASSWORD, STORE_FILE, STORE_PASSWORD)

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
                keyAlias = values.getValue(KEY_ALIAS)!!,
                keyPassword = values.getValue(KEY_PASSWORD)!!,
                storeFile = values.getValue(STORE_FILE)!!,
                storePassword = values.getValue(STORE_PASSWORD)!!,
            )
        }
    }
}
