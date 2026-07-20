param(
    [Parameter(Mandatory = $true)][string]$Server,
    [Parameter(Mandatory = $true)][string]$Client
)

$port = Get-Random -Minimum 44000 -Maximum 49000
$relay = Start-Process -FilePath $Server -ArgumentList @('--port', $port) -PassThru -WindowStyle Hidden
$clientExit = 1
try {
    Start-Sleep -Milliseconds 350
    & $Client $port
    $clientExit = $LASTEXITCODE
}
finally {
    if (-not $relay.HasExited) {
        Stop-Process -Id $relay.Id -Force
    }
    $relay.WaitForExit()
    $relay.Dispose()
}
exit $clientExit
