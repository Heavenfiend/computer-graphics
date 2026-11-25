param([ValidateSet('Build', 'Run')][string]$Action = 'Build')
$ErrorActionPreference = 'Stop'
try {
    Set-Location -LiteralPath $PSScriptRoot
    $sdkCandidates = @(
        (Join-Path $PSScriptRoot '..\VulkanSDK\1.4.321.1'),
        [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'User'),
        $env:VULKAN_SDK,
        [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine'),
        (Join-Path $env:LOCALAPPDATA 'VulkanSDK\1.4.321.1')
    )
    $sdkRoot = $null
    foreach ($candidate in $sdkCandidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $candidate = [Environment]::ExpandEnvironmentVariables($candidate.Trim().Trim('"'))
            if (Test-Path -LiteralPath (Join-Path $candidate 'Bin\glslc.exe')) {
                $sdkRoot = $candidate
                break
            }
        }
    }
    if (-not $sdkRoot -and $Action -eq 'Build') {
        throw 'Vulkan SDK was not found. Check VULKAN_SDK.'
    }
    if ($sdkRoot) {
        $env:VULKAN_SDK = $sdkRoot
        $env:Path = (Join-Path $sdkRoot 'Bin') + ';' + $env:Path
        # указываем слои проверки из нашей папки sdk, без изменений реестра
        $env:VK_ADD_LAYER_PATH = (Join-Path $sdkRoot 'Bin') + ';' + $env:VK_ADD_LAYER_PATH
    }

    if ($Action -eq 'Build') {
        $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
        if ($cmakeCommand) {
            $cmakeExe = $cmakeCommand.Source
        } else {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
            if (-not (Test-Path -LiteralPath $vswhere)) { throw 'CMake and Visual Studio Build Tools were not found.' }
            $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            $cmakeExe = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        }
        & $cmakeExe --preset msvc-debug -A x64
        if ($LASTEXITCODE -ne 0) { throw 'Project configuration failed.' }
        & $cmakeExe --build build-debug --config Debug --parallel 4
        if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
        Write-Host 'Build completed. Open RUN.cmd to launch the app.'
    } else {
        $program = Join-Path $PSScriptRoot 'build-debug\Debug\vulkan-starter-app.exe'
        if (-not (Test-Path -LiteralPath $program)) { throw 'Build the app with BUILD.cmd first.' }
        $runProcess = Start-Process -FilePath $program -WorkingDirectory $PSScriptRoot -PassThru -Wait -RedirectStandardOutput 'build-debug\runtime-output.log' -RedirectStandardError 'build-debug\runtime-errors.log'
        if ($runProcess.ExitCode -ne 0) { throw 'The app exited with an error. See build-debug\runtime-errors.log.' }
        if ((Get-Item 'build-debug\runtime-errors.log').Length -gt 0) {
            Get-Content 'build-debug\runtime-errors.log'
        }
    }
} catch {
    Write-Host $_ -ForegroundColor Red
    exit 1
}
