plugins {
    application
    id("org.graalvm.buildtools.native")
    id("org.jetbrains.kotlin.jvm")
    id("org.jlleitschuh.gradle.ktlint")
    jacoco
}

group = "com.dot"

ktlint {
    version.set("1.3.1")
}

jacoco {
    toolVersion = "0.8.12"
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

application {
    mainClass.set("com.dot.companion.cli.MainKt")
}

dependencies {
    implementation(project(":chat_client"))

    testImplementation("com.squareup.okhttp3:mockwebserver:4.12.0")
    testImplementation("com.squareup.okhttp3:okhttp-tls:4.12.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.1")
    testImplementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.3")
}

val nativeHeap = providers.gradleProperty("dot.native.heap").orElse("6g")
val nativeThreads = providers.gradleProperty("dot.native.threads").orElse("2")

graalvmNative {
    // GraalVM comes from GRAALVM_HOME, set only for the build by scripts/build_chat_cli.ps1.
    toolchainDetection.set(false)
    testSupport.set(false)
    metadataRepository {
        val metadataVersion = "1.1.19"
        enabled.set(true)
        uri(
            "https://repo.maven.apache.org/maven2/org/graalvm/buildtools/graalvm-reachability-metadata/" +
                "$metadataVersion/graalvm-reachability-metadata-$metadataVersion-repository.zip",
        )
    }
    binaries {
        named("main") {
            imageName.set("dot_chat")
            mainClass.set(application.mainClass)
            verbose.set(true)
            jvmArgs.add(nativeHeap.map { "-Xmx$it" })
            buildArgs.add(nativeThreads.map { "--parallelism=$it" })
            buildArgs.addAll("-H:+AddAllCharsets", "-H:+ReportExceptionStackTraces")
        }
    }
}

val nativeExecutable = layout.buildDirectory.file("native/nativeCompile/dot_chat.exe")
val nativeReports = layout.buildDirectory.dir("reports/native")

tasks.withType<Test>().configureEach {
    testLogging {
        events("failed")
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }
}

tasks.named<Test>("test") {
    exclude("**/NativeSmokeTest*")
}

tasks.register<Test>("nativeSmokeTest") {
    description = "Runs the controlled-peer workflow against the native dot_chat executable."
    group = "verification"
    dependsOn("nativeCompile")
    testClassesDirs =
        sourceSets.test
            .get()
            .output.classesDirs
    classpath = sourceSets.test.get().runtimeClasspath
    include("**/NativeSmokeTest*")
    systemProperty("dot.chat.exe", nativeExecutable.get().asFile.absolutePath)
    systemProperty("dot.chat.reports", nativeReports.get().asFile.absolutePath)
    systemProperty("dot.chat.smokeHelper", rootProject.file("scripts/native_process_smoke.ps1").absolutePath)
    outputs.upToDateWhen { false }
}

val coverageExcludes = listOf("**/MainKt*")
val coverageClassDirs =
    sourceSets.main
        .get()
        .output.asFileTree
        .matching { exclude(coverageExcludes) }
val coverageSourceDirs = files("src/main/kotlin")
val coverageExecData = layout.buildDirectory.file("jacoco/test.exec")

tasks.named<JacocoReport>("jacocoTestReport") {
    dependsOn("test")
    reports {
        xml.required.set(true)
        xml.outputLocation.set(layout.buildDirectory.file("reports/jacoco/jacocoTestReport/jacocoTestReport.xml"))
        html.required.set(true)
        csv.required.set(false)
    }
    classDirectories.setFrom(coverageClassDirs)
    sourceDirectories.setFrom(coverageSourceDirs)
    executionData.setFrom(coverageExecData)
}

tasks.register<JacocoCoverageVerification>("jacocoCoverageFloor") {
    dependsOn("jacocoTestReport")
    classDirectories.setFrom(coverageClassDirs)
    sourceDirectories.setFrom(coverageSourceDirs)
    executionData.setFrom(coverageExecData)
    violationRules {
        rule {
            limit {
                counter = "LINE"
                value = "COVEREDRATIO"
                minimum = "0.98".toBigDecimal()
            }
        }
    }
}
