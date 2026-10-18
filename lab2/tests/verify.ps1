param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$labRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $labRoot 'build-debug'
$executable = Join-Path $buildRoot "$Configuration\vulkan-starter-app.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the requested configuration first.' }
$oldLayerPath = $env:VK_ADD_LAYER_PATH
$oldLayerEnables = $env:VK_LAYER_ENABLES
try {
    $localSdk = Join-Path $labRoot '..\VulkanSDK\1.4.321.1\Bin'
    if (Test-Path -LiteralPath $localSdk) { $env:VK_ADD_LAYER_PATH = $localSdk }
    $env:VK_LAYER_ENABLES = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
    & ctest --test-dir $buildRoot -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Math tests failed.' }
    foreach ($size in @(@(1280,720), @(960,540), @(900,900))) {
        $label = "$Configuration-$($size[0])x$($size[1])"
        $outLog = Join-Path $buildRoot "validation-$label-output.log"
        $errLog = Join-Path $buildRoot "validation-$label-errors.log"
        $process = Start-Process -FilePath $executable -WorkingDirectory $labRoot -WindowStyle Hidden -PassThru -Wait `
            -ArgumentList '--hidden','--frames','120','--size',"$($size[0])","$($size[1])" `
            -RedirectStandardOutput $outLog -RedirectStandardError $errLog
        $output = (Get-Content -LiteralPath $outLog,$errLog -Raw) -join "`n"
        if ($process.ExitCode -ne 0 -or $output -notmatch 'Rendered 120 frames' -or
            $output -notmatch 'Validation layer: enabled' -or
            $output -match '\[(ERROR|WARNING): Validation\]') {
            throw "Runtime/validation failed for $label. See $outLog and $errLog."
        }
        Write-Host "$label : 120 frames, no validation warnings/errors"
    }
} finally {
    $env:VK_ADD_LAYER_PATH = $oldLayerPath
    $env:VK_LAYER_ENABLES = $oldLayerEnables
}
