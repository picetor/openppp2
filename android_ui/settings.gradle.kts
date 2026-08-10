pluginManagement {
    repositories {
        if (System.getenv("GITHUB_ACTIONS") == "true") {
            // GitHub-hosted runners are in the US: use upstream sources directly.
            gradlePluginPortal()
            google()
            mavenCentral()
        } else {
            // Local builds (CN): prefer fast CN mirrors, keep upstream fallbacks.
            maven(url = "https://maven.aliyun.com/repository/gradle-plugin")
            maven(url = "https://maven.aliyun.com/repository/google")
            maven(url = "https://maven.aliyun.com/repository/central")
            maven(url = "https://maven.aliyun.com/repository/public")
            maven(url = "file:///D:/github/openppp2/.toolchains/maven-local")
            maven(url = "https://mirrors.cloud.tencent.com/nexus/repository/jitpack/")
            gradlePluginPortal()
            google()
            mavenCentral()
        }
    }
}

dependencyResolutionManagement {
    repositories {
        if (System.getenv("GITHUB_ACTIONS") == "true") {
            google()
            mavenCentral()
            gradlePluginPortal()
            maven(url = "https://jitpack.io")
        } else {
            maven(url = "https://maven.aliyun.com/repository/google")
            maven(url = "https://maven.aliyun.com/repository/central")
            maven(url = "https://maven.aliyun.com/repository/gradle-plugin")
            maven(url = "https://maven.aliyun.com/repository/public")
            maven(url = "file:///D:/github/openppp2/.toolchains/maven-local")
            maven(url = "https://mirrors.cloud.tencent.com/nexus/repository/jitpack/")
            google()
            mavenCentral()
            gradlePluginPortal()
        }
    }
}

include(":plugin:api")
include(":app")

rootProject.name = "openppp2"
