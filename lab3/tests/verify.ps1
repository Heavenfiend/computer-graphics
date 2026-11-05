param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$labRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $labRoot 'build-debug'
$binaryRoot = Join-Path $buildRoot $Configuration
$executable = Join-Path $binaryRoot 'vulkan-starter-app.exe'
$gpuExecutable = Join-Path $binaryRoot 'lab3-render-tests.exe'
foreach ($program in @($executable, $gpuExecutable)) {
    if (-not (Test-Path -LiteralPath $program)) { throw "Build the requested configuration first: $program" }
}

$ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
if ($ctestCommand) {
    $ctestExe = $ctestCommand.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'CTest was not found. Add CMake to PATH.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $ctestExe = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
    if (-not (Test-Path -LiteralPath $ctestExe)) { throw 'CTest was not found in Visual Studio.' }
}

$sdkCandidates = @(
    (Join-Path $labRoot '..\VulkanSDK\1.4.321.1'),
    $env:VULKAN_SDK,
    [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'User'),
    [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine'),
    (Join-Path $env:LOCALAPPDATA 'VulkanSDK\1.4.321.1')
)
$layerDirectory = $null
foreach ($candidate in $sdkCandidates) {
    if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
    $candidate = [Environment]::ExpandEnvironmentVariables($candidate.Trim().Trim('"'))
    $candidateBin = Join-Path $candidate 'Bin'
    if (Test-Path -LiteralPath (Join-Path $candidateBin 'VkLayer_khronos_validation.json')) {
        $layerDirectory = $candidateBin
        break
    }
}
if (-not $layerDirectory) { throw 'Validation layer was not found. Check VULKAN_SDK or the adjacent VulkanSDK folder.' }

function Invoke-ValidatedRun([string]$Program, [string]$Label, [string[]]$Arguments, [string]$ExpectedOutput) {
    $outLog = Join-Path $buildRoot "validation-$Label-output.log"
    $errLog = Join-Path $buildRoot "validation-$Label-errors.log"
    $start = @{
        FilePath = $Program
        WorkingDirectory = $binaryRoot
        WindowStyle = 'Hidden'
        PassThru = $true
        Wait = $true
        RedirectStandardOutput = $outLog
        RedirectStandardError = $errLog
    }
    if ($Arguments.Count -gt 0) { $start.ArgumentList = $Arguments }
    $process = Start-Process @start
    $output = (Get-Content -LiteralPath $outLog,$errLog -Raw) -join "`n"
    if ($process.ExitCode -ne 0 -or $output -notmatch 'Validation layer: enabled' -or
        ($ExpectedOutput -and $output -notmatch [regex]::Escape($ExpectedOutput)) -or
        (Get-Item -LiteralPath $errLog).Length -ne 0 -or
        $output -match '\[(ERROR|WARNING): Validation\]|Validation (Error|Warning)|VUID-') {
        throw "Runtime/validation failed for $Label. See $outLog and $errLog."
    }
    Write-Host "$Label : passed, no validation warnings/errors"
}

$oldLayerPath = $env:VK_ADD_LAYER_PATH
$oldLayerEnables = $env:VK_LAYER_ENABLES
try {
    $env:VK_ADD_LAYER_PATH = $layerDirectory + ';' + $oldLayerPath
    $env:VK_LAYER_ENABLES = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
    # GPU-тест запускается отдельно: его diagnostics проверяются даже при ExitCode=0.
    & $ctestExe --test-dir $buildRoot -C $Configuration -LE gpu --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { throw 'CPU tests failed.' }
    Invoke-ValidatedRun $gpuExecutable "$Configuration-gpu" @() 'GPU regression passed:'
    foreach ($size in @(@(1280,720), @(960,540), @(900,900))) {
        $label = "$Configuration-$($size[0])x$($size[1])"
        Invoke-ValidatedRun $executable $label @('--hidden','--frames','120','--size',"$($size[0])","$($size[1])") 'Rendered 120 frames'
    }
} finally {
    $env:VK_ADD_LAYER_PATH = $oldLayerPath
    $env:VK_LAYER_ENABLES = $oldLayerEnables
}
