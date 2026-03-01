param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

if (-not $SkipBuild) {
    mvn -q -DskipTests compile dependency:build-classpath "-Dmdep.outputFile=target/classpath.txt" "-Dmdep.pathSeparator=;"
}

if (-not (Test-Path "target/classes")) {
    throw "Missing target/classes. Run without -SkipBuild first."
}

if (-not (Test-Path "target/classpath.txt")) {
    throw "Missing target/classpath.txt. Run without -SkipBuild first."
}

$depCp = (Get-Content "target/classpath.txt" -Raw).Trim()
$cp = "target/classes;$depCp"

$javaArgs = @(
    "-Dsun.misc.Unsafe.SCAN_FOR_DEPRECATED_METHODS=false"
    "--add-opens=java.base/java.nio=ALL-UNNAMED"
    "--add-opens=java.base/jdk.internal.misc=ALL-UNNAMED"
    "--add-opens=java.base/sun.nio.ch=ALL-UNNAMED"
    "--add-opens=java.base/java.lang=ALL-UNNAMED"
    "--enable-native-access=ALL-UNNAMED"
    "-classpath"
    $cp
    "rps.client.RpsClientMain"
)

& java @javaArgs
