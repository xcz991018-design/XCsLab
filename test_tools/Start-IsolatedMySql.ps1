param([string]$MySqlRoot = 'C:/Program Files/MySQL/MySQL Server 8.0')
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $repositoryRoot ('build/mysql-template-test-' + [guid]::NewGuid().ToString('N'))
$dataRoot = Join-Path $testRoot 'data'
$serverExecutable = Join-Path $MySqlRoot 'bin/mysqld.exe'
New-Item -ItemType Directory -Path $dataRoot -Force | Out-Null
$initializationLog = Join-Path $testRoot 'initialization.log'
& $serverExecutable --no-defaults --initialize-insecure "--basedir=$MySqlRoot" "--datadir=$dataRoot" "--log-error=$initializationLog"
if ($LASTEXITCODE -ne 0) { throw "Isolated MySQL initialization failed; see $initializationLog" }
$serverLog = Join-Path $testRoot 'server.log'
$arguments = @('--no-defaults', ('--basedir="{0}"' -f $MySqlRoot), ('--datadir="{0}"' -f $dataRoot),
    '--port=33379', '--bind-address=127.0.0.1', '--mysqlx=0', '--innodb-buffer-pool-size=64M',
    ('--log-error="{0}"' -f $serverLog), ('--pid-file="{0}"' -f (Join-Path $testRoot 'server.pid')))
$server = Start-Process -FilePath $serverExecutable -ArgumentList $arguments -WindowStyle Hidden -PassThru
$startedAt = $server.StartTime.ToUniversalTime().ToString('o')
$ready = $false
for ($attempt = 0; $attempt -lt 100; $attempt++) {
    Start-Sleep -Milliseconds 100
    $server.Refresh()
    if ($server.HasExited) { throw "Isolated MySQL exited; see $serverLog" }
    if ((Test-Path -LiteralPath $serverLog) -and ((Get-Content -LiteralPath $serverLog -Raw) -match 'ready for connections')) { $ready = $true; break }
}
if (-not $ready) { throw "Isolated MySQL startup timed out; see $serverLog" }
$metadata = @{ processId = $server.Id; startedAt = $startedAt; dataRoot = $dataRoot }
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'owner.json')
Write-Output "Test instance ready on 127.0.0.1:33379; root has no password only in this isolated instance."
Write-Output $testRoot
