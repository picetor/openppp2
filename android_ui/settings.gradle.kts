pluginManagement {
    repositories {
        maven(url = "https://maven.aliyun.com/repository/gradle-plugin")
        maven(url = "https://maven.aliyun.com/repository/google")
        maven(url = "https://maven.aliyun.com/repository/central")
        maven(url = "https://maven.aliyun.com/repository/public")
        maven(url = "https://mirrors.cloud.tencent.com/nexus/repository/jitpack/")
        // Official fallbacks: aliyun/tencent mirrors return 502 on GitHub
        // runners (US), so keep upstream repositories as a reliable fallback.
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
        maven(url = "https://maven.aliyun.com/repository/google")
        maven(url = "https://maven.aliyun.com/repository/central")
        maven(url = "https://maven.aliyun.com/repository/gradle-plugin")
        maven(url = "https://maven.aliyun.com/repository/public")
        maven(url = "https://mirrors.cloud.tencent.com/nexus/repository/jitpack/")
        // Official fallbacks: aliyun/tencent mirrors return 502 on GitHub
        // runners (US), so keep upstream repositories as a reliable fallback.
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

include(":plugin:api")
include(":app")

rootProject.name = "openppp2"
