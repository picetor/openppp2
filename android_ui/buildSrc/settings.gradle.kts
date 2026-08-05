val isGithubActions = System.getenv("GITHUB_ACTIONS") == "true"

pluginManagement {
    repositories {
        if (isGithubActions) {
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
            gradlePluginPortal()
            google()
            mavenCentral()
        }
    }
}

dependencyResolutionManagement {
    versionCatalogs {
        create("libs") {
            from(files("../gradle/libs.versions.toml"))
        }
    }
}
