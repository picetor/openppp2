repositories {
    if (System.getenv("GITHUB_ACTIONS") == "true") {
        // GitHub-hosted runners are in the US: use upstream sources directly.
        google()
        mavenCentral()
        gradlePluginPortal()
        maven(url = "https://jitpack.io")
    } else {
        // Local builds (CN): prefer fast CN mirrors, keep upstream fallbacks.
        maven(url = "https://maven.aliyun.com/repository/google")
        maven(url = "https://maven.aliyun.com/repository/central")
        maven(url = "https://maven.aliyun.com/repository/gradle-plugin")
        maven(url = "https://maven.aliyun.com/repository/public")
        maven(url = "https://mirrors.cloud.tencent.com/nexus/repository/jitpack/")
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
