package com.dot.release

import org.gradle.api.DefaultTask
import org.gradle.api.GradleException
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.provider.ListProperty
import org.gradle.api.provider.Property
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.OutputFile
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.process.ExecOperations
import java.io.ByteArrayOutputStream
import java.io.File
import javax.inject.Inject

private fun singleApk(directory: File): File {
    val apks = directory.listFiles { file -> file.extension == "apk" }.orEmpty()
    require(apks.size == 1) { "expected one APK in $directory, found ${apks.size}" }
    return apks.single()
}

/** Fails the build when R8 removed a required class or the packaged DEX does not define its mapped name. */
abstract class VerifyApkClassesTask : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val apkDirectory: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val mappingFile: RegularFileProperty

    @get:Input
    abstract val requiredClasses: ListProperty<String>

    @get:OutputFile
    abstract val report: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val usageFile: RegularFileProperty

    @TaskAction
    fun verify() {
        val apk = singleApk(apkDirectory.get().asFile)
        val mapping = r8ClassMapping(mappingFile.get().asFile.readText())
        val removed = r8RemovedClasses(usageFile.get().asFile.readText())
        val required = requiredClasses.get()
        val missing = missingClasses(required, mapping, apkDexClassNames(apk), removed)
        if (missing.isNotEmpty()) {
            throw GradleException("${apk.name} is missing required classes:\n  ${missing.joinToString("\n  ")}")
        }
        report.get().asFile.writeText(required.joinToString("") { "$it -> ${mapping.getValue(it)}\n" })
    }
}

/** Fails the build unless apksigner verifies the APK with exactly the expected signing certificate. */
abstract class VerifyApkSignerTask : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val apkDirectory: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val apksigner: RegularFileProperty

    @get:Input
    abstract val expectedCertSha256: Property<String>

    @get:Inject
    abstract val execOperations: ExecOperations

    @get:OutputFile
    abstract val report: RegularFileProperty

    @TaskAction
    fun verify() {
        val apk = singleApk(apkDirectory.get().asFile)
        val output = ByteArrayOutputStream()
        execOperations.exec {
            commandLine(apksigner.get().asFile.absolutePath, "verify", "--print-certs", apk.absolutePath)
            standardOutput = output
        }
        val certificates = output.toString(Charsets.UTF_8)
        requireSingleSigner(certificates, expectedCertSha256.get())
        report.get().asFile.writeText(certificates)
    }
}

/** Fails unless the tag is a stable `android-v<MAJOR>.<MINOR>.<PATCH>` tag matching the app's versionName. */
abstract class VerifyReleaseTagTask : DefaultTask() {
    @get:Input
    abstract val tag: Property<String>

    @get:Input
    abstract val versionName: Property<String>

    @TaskAction
    fun verify() = requireReleaseTag(tag.get(), versionName.get())
}
