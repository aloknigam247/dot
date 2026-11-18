import com.dot.release.SigningInputs
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

val verifyReleaseTag =
    tasks.register<VerifyReleaseTagTask>("verifyReleaseTag") {
        tag.set(providers.gradleProperty("dot.release.tag"))
        versionName.set(android.defaultConfig.versionName)
    }

if (providers.gradleProperty("dot.release.tag").isPresent) {
    tasks.matching { it.name == "assembleRelease" }.configureEach { dependsOn(verifyReleaseTag) }
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
