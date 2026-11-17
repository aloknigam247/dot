import com.android.build.api.artifact.SingleArtifact
import com.dot.release.SigningInputs
import com.dot.release.VerifyApkClassesTask
import com.dot.release.VerifyApkSignerTask
import com.dot.release.VerifyReleaseTagTask

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
    id("org.jlleitschuh.gradle.ktlint")
    jacoco
}

ktlint {
    version.set("1.3.1")
}

jacoco {
    toolVersion = "0.8.12"
}

val releaseSigning =
    SigningInputs.resolve(
        lookup = { providers.environmentVariable(it).orNull },
        required = providers.gradleProperty("dot.signing.required").map(String::toBooleanStrict).getOrElse(false),
    )

android {
    namespace = "com.dot.companion"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.dot.companion"
        minSdk = 26
        targetSdk = 34
        versionCode = 5
        versionName = "1.2.0"
    }

    signingConfigs {
        releaseSigning?.let { inputs ->
            create("release") {
                keyAlias = inputs.keyAlias
                keyPassword = inputs.keyPassword
                storeFile = file(inputs.storeFile)
                storePassword = inputs.storePassword
                storeType = "PKCS12"
            }
        }
    }

    buildTypes {
        debug {
            enableUnitTestCoverage = true
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            if (releaseSigning != null) signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    sourceSets.named("main") {
        res.srcDir(rootProject.file("../common/icons/android"))
    }
}

val releaseSerializers =
    listOf(
        "com.dot.companion.net.CatalogHost\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.ActionEnvelope\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.ChatState\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.DispatchActionParams\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.InitializeParams\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.InitializeResult\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.JsonRpcNotification\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.JsonRpcRequest\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.ListSessionsParams\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.ListSessionsResult\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.ResponsePartSerializer",
        "com.microsoft.agenthostprotocol.generated.SessionState\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.Snapshot\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.SnapshotStateSerializer",
        "com.microsoft.agenthostprotocol.generated.StateActionSerializer",
        "com.microsoft.agenthostprotocol.generated.SubscribeParams\$\$serializer",
        "com.microsoft.agenthostprotocol.generated.SubscribeResult\$\$serializer",
    )

val verifyReleaseTag =
    tasks.register<VerifyReleaseTagTask>("verifyReleaseTag") {
        tag.set(providers.gradleProperty("dot.release.tag"))
        versionName.set(android.defaultConfig.versionName)
    }

androidComponents {
    onVariants(selector().withBuildType("release")) { variant ->
        val verifyDex =
            tasks.register<VerifyApkClassesTask>("verifyReleaseDexSerializers") {
                apkDirectory.set(variant.artifacts.get(SingleArtifact.APK))
                mappingFile.set(variant.artifacts.get(SingleArtifact.OBFUSCATION_MAPPING_FILE))
                report.set(layout.buildDirectory.file("reports/release/serializers.txt"))
                requiredClasses.set(releaseSerializers)
                usageFile.set(layout.file(mappingFile.map { it.asFile.resolveSibling("usage.txt") }))
            }
        tasks.matching { it.name == "assembleRelease" }.configureEach {
            dependsOn(verifyDex)
            if (providers.gradleProperty("dot.release.tag").isPresent) dependsOn(verifyReleaseTag)
        }
        releaseSigning?.let { inputs ->
            val signerName = if (System.getProperty("os.name").startsWith("Windows")) "apksigner.bat" else "apksigner"
            val signerFile =
                sdkComponents.sdkDirectory.map {
                    it.file(
                        "build-tools/${android.buildToolsVersion}/$signerName",
                    )
                }
            val verifySigner =
                tasks.register<VerifyApkSignerTask>("verifyReleaseSigner") {
                    apkDirectory.set(variant.artifacts.get(SingleArtifact.APK))
                    apksigner.set(signerFile)
                    expectedCertSha256.set(inputs.certSha256)
                    report.set(layout.buildDirectory.file("reports/release/signer.txt"))
                }
            tasks.matching { it.name == "assembleRelease" }.configureEach { dependsOn(verifySigner) }
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.06.00")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.2")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.2")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.2")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation("com.microsoft.agenthostprotocol:agent-host-protocol:0.9.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.1")
}

tasks.withType<Test>().configureEach {
    testLogging {
        events("failed")
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }
}

// Classes that JVM unit tests (no emulator in CI) structurally cannot reach: the
// Compose UI, the Activity bootstrap, the OkHttp WebSocket transport, and
// compiler-generated serializers. Excluded from both the report and the floor
// so the measured number reflects only the logic that tests can actually exercise.
val coverageExcludes =
    listOf(
        "**/MainActivity*",
        "**/ui/**",
        "**/net/**",
        "**/*\$\$serializer*",
        "**/ComposableSingletons*",
    )

val coverageClassDirs =
    fileTree(layout.buildDirectory.dir("tmp/kotlin-classes/debug")) { exclude(coverageExcludes) }
val coverageSourceDirs = files("src/main/java")
val coverageExecData =
    layout.buildDirectory.file("outputs/unit_test_code_coverage/debugUnitTest/testDebugUnitTest.exec")

tasks.register<JacocoReport>("jacocoTestReport") {
    dependsOn("testDebugUnitTest")
    reports {
        xml.required.set(true)
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
