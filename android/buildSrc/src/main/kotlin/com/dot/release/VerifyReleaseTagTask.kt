package com.dot.release

import org.gradle.api.DefaultTask
import org.gradle.api.provider.Property
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.TaskAction

abstract class VerifyReleaseTagTask : DefaultTask() {
    @get:Input
    abstract val tag: Property<String>

    @get:Input
    abstract val versionName: Property<String>

    @TaskAction
    fun verify() = requireReleaseTag(tag.get(), versionName.get())
}
