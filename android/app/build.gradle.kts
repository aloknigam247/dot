import com.android.build.gradle.api.ApkVariantOutput

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jlleitschuh.gradle.ktlint")
    jacoco
}

ktlint {
    version.set("1.3.1")
}

jacoco {
    toolVersion = "0.8.12"
}

android {
    namespace = "com.dot.companion"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.dot.companion"
        minSdk = 26
        targetSdk = 34
        versionCode = 6
        versionName = "1.3.0"
    }

    signingConfigs {
        val names = listOf("KEY_ALIAS", "KEY_PASSWORD", "KEYSTORE", "KEYSTORE_PASSWORD")
        val inputs = names.associateWith { providers.environmentVariable("DOT_ANDROID_$it").orNull }
        val missing = names.filter { inputs[it].isNullOrBlank() }
        val required = providers.gradleProperty("dot.signing.required").map(String::toBooleanStrict).getOrElse(false)
        if (required || missing.size != names.size) {
            require(missing.isEmpty()) { "Release signing needs: ${missing.joinToString { "DOT_ANDROID_$it" }}" }
            create("release") {
                keyAlias = inputs.getValue("KEY_ALIAS")
                keyPassword = inputs.getValue("KEY_PASSWORD")
                storeFile = file(requireNotNull(inputs.getValue("KEYSTORE")))
                storePassword = inputs.getValue("KEYSTORE_PASSWORD")
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
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        checkReleaseBuilds = false
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

    applicationVariants.all {
        outputs.withType<ApkVariantOutput>().all {
            if (outputFileName == "app-release.apk") outputFileName = "dot.apk"
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.09.00")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.3")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation(project(":chat_client"))
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

// Platform calls and Compose require an Android runtime.
val coverageExcludes =
    listOf(
        "**/MainActivity*",
        "**/ui/**",
        "**/ComposableSingletons*",
        "**/AndroidChatLogger*",
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
