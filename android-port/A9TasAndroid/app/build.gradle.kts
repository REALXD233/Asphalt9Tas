plugins {
    id("com.android.application")
}

android {
    namespace = "dev.a9tas.android"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.a9tas.android"
        minSdk = 24
        targetSdk = 35
        versionCode = 32
        versionName = "0.8.0-profile-autogen"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        resources.excludes += setOf("META-INF/DEPENDENCIES", "META-INF/LICENSE*")
    }
}
