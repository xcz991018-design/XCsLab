param([string]$OutputDirectory = (Join-Path $env:TEMP 'GaugeReader-offline-validation'), [switch]$SandboxGuest)
$ErrorActionPreference = 'Stop'
$packageRoot = $PSScriptRoot
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$names = @('PATH','PYTHONHOME','PYTHONPATH','PYTHONUSERBASE','VIRTUAL_ENV','USERPROFILE','HOME','APPDATA','LOCALAPPDATA','TEMP','TMP','GAUGE_OCR_PYTHON','GAUGE_OCR_SCRIPT','GAUGE_OCR_DETECTION_MODEL_DIR','GAUGE_OCR_RECOGNITION_MODEL_DIR')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
function Invoke-ValidationProcess([string]$Program, [string[]]$Arguments, [string]$LogPath) {
    # Windows PowerShell 5.1 turns ordinary native stderr warnings into ErrorRecords.
    $ErrorActionPreference = 'Continue'
    & $Program @Arguments *> $LogPath
    if ($LASTEXITCODE -ne 0) { throw "Validation process failed ($LASTEXITCODE): $Program. See $LogPath" }
}
try {
    $osNetworkDisabled = $false
    if ($SandboxGuest) {
        if ($env:USERNAME -ne 'WDAGUtilityAccount') { throw 'SandboxGuest verification requires a Windows Sandbox guest.' }
        if (@(Get-NetAdapter | Where-Object Status -eq 'Up').Count -ne 0) { throw 'An active network adapter exists in the sandbox.' }
        $osNetworkDisabled = $true
    }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:PYTHONHOME = 'C:\NoInstalledPython'
    $env:PYTHONPATH = 'C:\NoInstalledPython\site-packages'
    $env:PYTHONUSERBASE = 'C:\NoInstalledPython\user'
    $env:VIRTUAL_ENV = 'C:\NoInstalledPython\venv'
    $env:GAUGE_OCR_PYTHON = 'C:\NoInstalledPython\python.exe'
    $env:GAUGE_OCR_SCRIPT = 'C:\NoInstalledPython\missing.py'
    $env:GAUGE_OCR_DETECTION_MODEL_DIR = 'C:\NoCachedModels\det'
    $env:GAUGE_OCR_RECOGNITION_MODEL_DIR = 'C:\NoCachedModels\rec'
    # A fresh profile per invocation proves models aren't coming from user caches.
    $profileRoot = Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
    $env:USERPROFILE = Join-Path $profileRoot 'profile'
    $env:HOME = $env:USERPROFILE
    $env:LOCALAPPDATA = Join-Path $env:USERPROFILE 'AppData\Local'
    $env:APPDATA = Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TEMP = Join-Path $profileRoot 'temp'
    $env:TMP = $env:TEMP
    foreach ($path in @($env:USERPROFILE,$env:LOCALAPPDATA,$env:APPDATA,$env:TEMP)) { New-Item -ItemType Directory -Path $path -Force | Out-Null }
    Invoke-ValidationProcess -Program (Join-Path $packageRoot 'ocr\runtime\python.exe') -Arguments @('-B',(Join-Path $packageRoot 'ocr\validation\verify_python.py'),'--output',(Join-Path $OutputDirectory 'python-report.json')) -LogPath (Join-Path $OutputDirectory 'python.log')
    Invoke-ValidationProcess -Program (Join-Path $packageRoot 'ocr_verify.exe') -Arguments @('--output',(Join-Path $OutputDirectory 'cpp-report.json')) -LogPath (Join-Path $OutputDirectory 'cpp.log')
    @{ passed=$true; tested_at=[DateTime]::UtcNow.ToString('o'); package_root=$packageRoot; os_network_disabled=$osNetworkDisabled; validation_environment=$(if($SandboxGuest){'windows_sandbox'}else{'host_isolation'}); note='Only windows_sandbox is a clean Windows guest. Host isolation does not prove OS-level network isolation.' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
    Write-Output "PASS: offline bundle validation. Reports: $OutputDirectory"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
