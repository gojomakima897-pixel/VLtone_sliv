[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string] $Configuration = "Release",
    [string] $BuildDirectory = "",
    [string] $QtRoot = "",
    [string] $VcpkgRoot = "",
    [string] $ApiOrigin = "https://vltstudio.ru/api/v1",
    [string] $MsvcToolset = "14.44",
    [string] $WindowsSdkVersion = "10.0.26100.0",
    [string] $SignPfxPath = "",
    [switch] $RequireSignature,
    [switch] $SkipTests,
    [switch] $SkipInstaller
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDirectory = $PSScriptRoot
$repository = [IO.Path]::GetFullPath((Join-Path $scriptDirectory "..\.."))
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $repository "build-windows"
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$stageDirectory = Join-Path $BuildDirectory "stage"
$distributionDirectory = Join-Path $stageDirectory "bin"
$artifactDirectory = Join-Path $BuildDirectory "artifacts"
$expectedWindowsTestCount = 19

$cmakeSource = Get-Content -LiteralPath (Join-Path $repository "CMakeLists.txt") -Raw
$versionMatch = [Regex]::Match(
    $cmakeSource,
    '(?ms)project\s*\(\s*VLTStudioPro\b.*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) {
    throw "Could not read the project version from CMakeLists.txt."
}
$applicationVersion = $versionMatch.Groups[1].Value
$vcpkgManifest = Get-Content -LiteralPath (Join-Path $repository "vcpkg.json") `
    -Raw | ConvertFrom-Json
if ($vcpkgManifest.version -ne $applicationVersion) {
    throw "Version mismatch: CMake is $applicationVersion, vcpkg.json is $($vcpkgManifest.version)."
}
$vcpkgConfiguration = Get-Content -LiteralPath `
    (Join-Path $repository "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$vcpkgBaseline = $vcpkgConfiguration.'default-registry'.baseline
if ($vcpkgBaseline -notmatch '^[0-9a-f]{40}$') {
    throw "vcpkg-configuration.json must contain a 40-character baseline."
}

function Invoke-Checked {
    if ($args.Count -lt 1) { throw "Invoke-Checked requires a command." }
    $filePath = [string] $args[0]
    $arguments = @($args | Select-Object -Skip 1)
    & $filePath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE: $filePath $($arguments -join ' ')"
    }
}

function Import-MsvcEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe was not found. Install Visual Studio 2022 Build Tools with the C++ workload."
    }

    $installationPath = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1).Trim()
    if (-not $installationPath) {
        throw "Visual Studio 2022 with the C++ toolchain was not found."
    }

    $developerCommand = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
    $command = "call `"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 " +
        "-vcvars_ver=$MsvcToolset -winsdk=$WindowsSdkVersion >nul && set"
    $environmentLines = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Could not activate MSVC $MsvcToolset with Windows SDK $WindowsSdkVersion. Install both components in Visual Studio Installer."
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) { continue }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -Path "Env:$name" -Value $value
    }

    if (-not $env:VCToolsVersion.StartsWith($MsvcToolset,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Activated MSVC toolset '$($env:VCToolsVersion)', expected $MsvcToolset.x."
    }
    $actualSdk = $env:WindowsSDKVersion.TrimEnd('\')
    if ($actualSdk -ne $WindowsSdkVersion) {
        throw "Activated Windows SDK '$actualSdk', expected $WindowsSdkVersion."
    }
    return $installationPath
}

function Resolve-QtRoot {
    if (-not $QtRoot -and $env:QTDIR) { $script:QtRoot = $env:QTDIR }
    if (-not $QtRoot) {
        $script:QtRoot = "C:\Qt\6.8.3\msvc2022_64"
    }
    $script:QtRoot = [IO.Path]::GetFullPath($QtRoot)

    $qmake = Join-Path $QtRoot "bin\qmake.exe"
    if (-not (Test-Path -LiteralPath $qmake -PathType Leaf)) {
        throw "Qt 6.8.3 for msvc2022_64 was not found at '$QtRoot'. Pass -QtRoot or set QTDIR."
    }
    $actualVersion = (& $qmake -query QT_VERSION).Trim()
    if ($actualVersion -ne "6.8.3") {
        throw "Qt $actualVersion was found at '$QtRoot'; the release requires exactly Qt 6.8.3."
    }
    foreach ($module in "Qt6WebEngineWidgets", "Qt6SerialPort") {
        $config = Join-Path $QtRoot "lib\cmake\$module\${module}Config.cmake"
        if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
            throw "Required Qt module '$module' is missing from '$QtRoot'."
        }
    }
    $env:QTDIR = $QtRoot
}

function Resolve-VcpkgRoot {
    if ($VcpkgRoot) {
        $script:VcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot)
    } else {
        $script:VcpkgRoot = Join-Path $BuildDirectory "_deps\vcpkg"
        if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git"))) {
            New-Item -ItemType Directory -Force -Path (Split-Path $VcpkgRoot) | Out-Null
            Invoke-Checked git clone --filter=blob:none --no-checkout `
                https://github.com/microsoft/vcpkg.git $VcpkgRoot
        }
        Invoke-Checked git -C $VcpkgRoot fetch --depth 1 origin $vcpkgBaseline
        Invoke-Checked git -C $VcpkgRoot checkout --detach $vcpkgBaseline
    }

    $bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
    if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
        throw "vcpkg was not found at '$VcpkgRoot'."
    }
    $vcpkgExecutable = Join-Path $VcpkgRoot "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $vcpkgExecutable -PathType Leaf)) {
        Invoke-Checked $bootstrap -disableMetrics
    }
    $env:VCPKG_ROOT = $VcpkgRoot
    $env:VCPKG_FEATURE_FLAGS = "manifests,versions,binarycaching"
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) {
        $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $BuildDirectory "vcpkg-cache"
    }
    New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
}

function Find-VcRedist {
    param([Parameter(Mandatory = $true)] [string] $VisualStudioPath)
    $redistRoot = Join-Path $VisualStudioPath "VC\Redist\MSVC"
    $redist = Get-ChildItem -LiteralPath $redistRoot -Recurse `
        -Filter "vc_redist.x64.exe" -File -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $redist) {
        throw "A Visual C++ v14 x64 redistributable was not found under '$redistRoot'."
    }
    return $redist.FullName
}

function Find-SignTool {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $preferred = Join-Path $kitsRoot "$WindowsSdkVersion\x64\signtool.exe"
    if (Test-Path -LiteralPath $preferred -PathType Leaf) { return $preferred }
    $fallback = Get-ChildItem -LiteralPath $kitsRoot -Recurse `
        -Filter "signtool.exe" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq "x64" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $fallback) { throw "signtool.exe was not found in the Windows SDK." }
    return $fallback.FullName
}

function Sign-File {
    param([Parameter(Mandatory = $true)] [string] $Path)
    if (-not $SignPfxPath) { return }
    if (-not $env:WINDOWS_SIGN_CERT_PASSWORD) {
        throw "WINDOWS_SIGN_CERT_PASSWORD is required when -SignPfxPath is used."
    }
    $signTool = Find-SignTool
    Invoke-Checked $signTool sign /fd SHA256 /td SHA256 `
        /tr https://timestamp.digicert.com /f $SignPfxPath `
        /p $env:WINDOWS_SIGN_CERT_PASSWORD $Path
    Invoke-Checked $signTool verify /pa $Path
}

$visualStudioPath = Import-MsvcEnvironment
Resolve-QtRoot
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
Resolve-VcpkgRoot

Write-Host "Configuring VLT Studio Pro $applicationVersion ($Configuration)..."
Invoke-Checked cmake --preset windows-vcpkg -B $BuildDirectory `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_INSTALL_PREFIX:PATH=$stageDirectory" `
    "-DCMAKE_PREFIX_PATH:PATH=$QtRoot" `
    "-DVLT_DEFAULT_API_ORIGIN=$ApiOrigin" `
    "-DCMAKE_SYSTEM_VERSION=$WindowsSdkVersion"
Invoke-Checked cmake --build $BuildDirectory --config $Configuration --parallel

if (-not $SkipTests) {
    $testListing = & ctest --test-dir $BuildDirectory -C $Configuration -N
    if ($LASTEXITCODE -ne 0) { throw "Could not enumerate CTest tests." }
    $countLine = $testListing | Select-String -Pattern 'Total Tests:\s+(\d+)' |
        Select-Object -Last 1
    if (-not $countLine -or
        [int] $countLine.Matches[0].Groups[1].Value -ne $expectedWindowsTestCount) {
        throw "Expected $expectedWindowsTestCount Windows CTest tests. CTest reported:`n$($testListing -join [Environment]::NewLine)"
    }
    Invoke-Checked ctest --test-dir $BuildDirectory -C $Configuration `
        --output-on-failure
}

if (Test-Path -LiteralPath $stageDirectory) {
    Remove-Item -LiteralPath $stageDirectory -Recurse -Force
}
Invoke-Checked cmake --install $BuildDirectory --config $Configuration `
    --prefix $stageDirectory

$application = Join-Path $distributionDirectory "VLT Studio Pro.exe"
foreach ($requiredFile in $application, `
         (Join-Path $distributionDirectory "daw_scan.exe"), `
         (Join-Path $distributionDirectory "daw_guard.exe"), `
         (Join-Path $distributionDirectory "daw_reporter.exe")) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "The deployed application is incomplete; missing '$requiredFile'."
    }
}

if ($RequireSignature -and -not $SignPfxPath) {
    throw "A production release requires -SignPfxPath and WINDOWS_SIGN_CERT_PASSWORD."
}
if ($SignPfxPath) {
    $SignPfxPath = [IO.Path]::GetFullPath($SignPfxPath)
    if (-not (Test-Path -LiteralPath $SignPfxPath -PathType Leaf)) {
        throw "Signing certificate does not exist: $SignPfxPath"
    }
    foreach ($ownedExecutable in "VLT Studio Pro.exe", "daw_scan.exe", "daw_guard.exe", "daw_reporter.exe") {
        Sign-File (Join-Path $distributionDirectory $ownedExecutable)
    }
}

Invoke-Checked (Join-Path $scriptDirectory "check-runtime-deps.ps1") `
    -DistributionDirectory $distributionDirectory

if (-not $SkipTests) {
    $unicodeTestDirectory = Join-Path $BuildDirectory "Тест сборки\VLT Studio Pro"
    if (Test-Path -LiteralPath $unicodeTestDirectory) {
        Remove-Item -LiteralPath $unicodeTestDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $unicodeTestDirectory | Out-Null
    Copy-Item -Path (Join-Path $distributionDirectory "*") `
        -Destination $unicodeTestDirectory -Recurse -Force

    $oldQpaPlatform = $env:QT_QPA_PLATFORM
    $oldWebEngineSandbox = $env:QTWEBENGINE_DISABLE_SANDBOX
    try {
        $env:QT_QPA_PLATFORM = "offscreen"
        $env:QTWEBENGINE_DISABLE_SANDBOX = "1"
        Invoke-Checked (Join-Path $unicodeTestDirectory "VLT Studio Pro.exe") `
            --selftest
    } finally {
        if ($null -eq $oldQpaPlatform) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
        else { $env:QT_QPA_PLATFORM = $oldQpaPlatform }
        if ($null -eq $oldWebEngineSandbox) { Remove-Item Env:QTWEBENGINE_DISABLE_SANDBOX -ErrorAction SilentlyContinue }
        else { $env:QTWEBENGINE_DISABLE_SANDBOX = $oldWebEngineSandbox }
    }
}

New-Item -ItemType Directory -Force -Path $artifactDirectory | Out-Null
$zipPath = Join-Path $artifactDirectory `
    "VLT-Studio-Pro-$applicationVersion-windows-x64.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $distributionDirectory "*") `
    -DestinationPath $zipPath -CompressionLevel Optimal

$installerPath = ""
if (-not $SkipInstaller) {
    $commonIscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
    $isccPath = ""
    if (Test-Path -LiteralPath $commonIscc -PathType Leaf) {
        $isccPath = $commonIscc
    } else {
        $iscc = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
        if ($iscc) { $isccPath = $iscc.Source }
    }
    if (-not $isccPath) {
        throw "Inno Setup 6 was not found. Install it or pass -SkipInstaller for a developer-only build."
    }
    $isccVersion = (Get-Item -LiteralPath $isccPath).VersionInfo.ProductVersion
    if (-not $isccVersion.StartsWith("6.7.1", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Inno Setup '$isccVersion' was found; the release toolchain requires 6.7.1."
    }

    $vcRedist = Find-VcRedist -VisualStudioPath $visualStudioPath
    $icon = Join-Path $repository "app\resources\windows\daw.ico"
    Invoke-Checked $isccPath `
        "/DSourceDir=$distributionDirectory" `
        "/DOutputDir=$artifactDirectory" `
        "/DAppVersion=$applicationVersion" `
        "/DVcRedist=$vcRedist" `
        "/DIconFile=$icon" `
        (Join-Path $scriptDirectory "installer.iss")
    $installerPath = Join-Path $artifactDirectory `
        "VLT-Studio-Pro-$applicationVersion-x64-Setup.exe"
    if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
        throw "Inno Setup completed without producing '$installerPath'."
    }
    Sign-File $installerPath
}

Write-Host "Windows release artifacts:"
Write-Host "  ZIP:       $zipPath"
if ($installerPath) { Write-Host "  Installer: $installerPath" }
