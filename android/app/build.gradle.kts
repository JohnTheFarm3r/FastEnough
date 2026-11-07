plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// The root helper is the same server binary that the Windows client pushes over ADB.
val serverBinary = rootProject.file("../server/afm-server")

// Copied into the default jniLibs folder, which is generated and not committed.
val copyRootHelper by tasks.registering(Copy::class) {
    from(serverBinary)
    into(layout.projectDirectory.dir("src/main/jniLibs/arm64-v8a"))
    rename { "libafmserver.so" }
}

android {
    namespace = "com.fastenough.companion"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.fastenough.companion"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            // The root helper runs as an executable, so it must exist on disk.
            useLegacyPackaging = true
        }
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }
}

tasks.named("preBuild") {
    dependsOn(copyRootHelper)
}

dependencies {
    implementation("androidx.core:core-ktx:1.10.1")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.10.0")
    val composeBom = platform("androidx.compose:compose-bom:2026.06.00")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
}
