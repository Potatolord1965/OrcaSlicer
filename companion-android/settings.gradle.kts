pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "UnifiedSlicerCompanion"

// The printer protocols live in a plain Kotlin/JVM module so they can be unit tested on the JVM.
include(":core", ":app")
