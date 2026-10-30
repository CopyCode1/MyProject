# Run this application with the installed NCS 3.4.1 SDK and its matching toolchain.
# The caller's terminal does not need an activated Python virtual environment.
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Rebuild', 'Flash', 'FlashInfo', 'Menuconfig', 'Check')]
    [string]$Action = 'Build',
    [string]$SdkPath = 'C:\ncs\v3.4.1',
    [string]$ToolchainPath = ''
)

$ErrorActionPreference = 'Stop'
$appRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath = Join-Path $appRoot 'build_ncs_341'
$SdkPath = [IO.Path]::GetFullPath($SdkPath)

if (-not (Test-Path -LiteralPath (Join-Path $SdkPath 'nrf\VERSION'))) {
    throw "NCS installation not found: $SdkPath"
}
$sdkVersion = Get-Content -LiteralPath (Join-Path $SdkPath 'nrf\VERSION') -Raw
if ($sdkVersion -notmatch '(?m)^VERSION_MAJOR\s*=\s*3\s*$' -or
    $sdkVersion -notmatch '(?m)^VERSION_MINOR\s*=\s*4\s*$' -or
    $sdkVersion -notmatch '(?m)^PATCHLEVEL\s*=\s*1\s*$') {
    throw "This build configuration requires NCS 3.4.1: $SdkPath"
}

# Resolve the matching toolchain from Nordic's local installation registry.
if (-not $ToolchainPath) {
    $sdkInstallRoot = Split-Path -Parent $SdkPath
    $registryPath = Join-Path $sdkInstallRoot 'toolchains\toolchains.json'
    $registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
    $matchingToolchains = @(
        foreach ($entry in $registry) {
            foreach ($candidate in $entry.toolchains) {
                if ($candidate.ncs_versions -contains 'v3.4.1') {
                    $candidate
                }
            }
        }
    )
    if ($matchingToolchains.Count -ne 1) {
        throw 'Specify -ToolchainPath: could not uniquely resolve the NCS 3.4.1 toolchain.'
    }
    $ToolchainPath = Join-Path $sdkInstallRoot (
        'toolchains\' + $matchingToolchains[0].identifier.bundle_id
    )
}
$ToolchainPath = [IO.Path]::GetFullPath($ToolchainPath)
$environmentPath = Join-Path $ToolchainPath 'environment.json'
$toolchainEnvironment = Get-Content -LiteralPath $environmentPath -Raw | ConvertFrom-Json

# These changes apply only to this PowerShell process and the tools it launches.
[Environment]::SetEnvironmentVariable('PYTHONHOME', $null, 'Process')
[Environment]::SetEnvironmentVariable('VIRTUAL_ENV', $null, 'Process')
foreach ($variable in $toolchainEnvironment.env_vars) {
    if ($variable.type -eq 'relative_paths') {
        $value = ($variable.values | ForEach-Object {
            [IO.Path]::GetFullPath((Join-Path $ToolchainPath $_))
        }) -join [IO.Path]::PathSeparator
        if ($variable.existing_value_treatment -eq 'prepend_to') {
            $previousValue = [Environment]::GetEnvironmentVariable($variable.key, 'Process')
            if ($previousValue) { $value += [IO.Path]::PathSeparator + $previousValue }
        }
    } elseif ($variable.type -eq 'string') {
        $value = $variable.value
    } else {
        throw "Unsupported toolchain environment entry: $($variable.type)"
    }
    [Environment]::SetEnvironmentVariable($variable.key, $value, 'Process')
}
$env:ZEPHYR_BASE = (Join-Path $SdkPath 'zephyr').Replace('\', '/')
$env:ZEPHYR_SDK_INSTALL_DIR = $env:ZEPHYR_SDK_INSTALL_DIR.Replace('\', '/')
$pythonPath = Join-Path $ToolchainPath 'opt\bin\python.exe'
if (-not (Test-Path -LiteralPath $pythonPath)) {
    throw "Toolchain Python not found: $pythonPath"
}

# The app is outside any other west workspace. West locates NCS via ZEPHYR_BASE.
# Build from the app's drive: west's source-path check cannot cross Windows drives.
Push-Location -LiteralPath $appRoot
try {
    Write-Host "NCS SDK:       $SdkPath"
    Write-Host "ZEPHYR_BASE:   $env:ZEPHYR_BASE"
    Write-Host "NCS toolchain: $ToolchainPath"
    Write-Host "Application:   $appRoot"
    Write-Host "Build output:  $buildPath"

    switch ($Action) {
        'Check' {
            & $pythonPath --version
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            & $pythonPath -m west topdir
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            & $pythonPath -m west --version
        }
        { $_ -in @('Build', 'Rebuild') } {
            $pristine = if ($Action -eq 'Rebuild') { 'always' } else { 'auto' }
            & $pythonPath -m west build --no-sysbuild -p $pristine `
                -b 'gt02/nrf52840' -d $buildPath $appRoot -- `
                "-DZEPHYR_BASE=$env:ZEPHYR_BASE" '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
        }
        'FlashInfo' {
            & $pythonPath -m west flash -d $buildPath --no-rebuild --context --runner jlink
        }
        'Flash' {
            if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'zephyr\zephyr.elf'))) {
                throw 'Build the NCS application before flashing.'
            }
            $jlinkDirectory = 'D:\Program Files\SEGGER\JLink_V964'
            if (Test-Path -LiteralPath $jlinkDirectory) {
                $env:PATH = $jlinkDirectory + [IO.Path]::PathSeparator + $env:PATH
            }
            & $pythonPath -m west flash -d $buildPath --runner jlink
        }
        'Menuconfig' {
            & $pythonPath -m west build -d $buildPath -t menuconfig
        }
    }
    $result = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $result
