param(
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot

function Stop-WithMessage([string]$Message) {
    Write-Host ''
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Find-Mosquitto {
    $candidates = @()
    $command = Get-Command 'mosquitto.exe' -ErrorAction SilentlyContinue
    if ($command) { $candidates += $command.Source }
    if (${env:ProgramFiles}) { $candidates += "${env:ProgramFiles}\Mosquitto\mosquitto.exe" }
    if (${env:ProgramFiles(x86)}) { $candidates += "${env:ProgramFiles(x86)}\Mosquitto\mosquitto.exe" }

    return $candidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
        Select-Object -First 1
}

function Test-PortFree([int]$Port, [string]$Protocol = 'TCP') {
    if ($Protocol -eq 'UDP') {
        return -not (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
    }
    return -not (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
}

function New-ChildProcess([string]$FileName, [string]$Arguments) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FileName
    $startInfo.Arguments = $Arguments
    $startInfo.WorkingDirectory = $projectRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    return [System.Diagnostics.Process]::Start($startInfo)
}

Write-Host '=========================================' -ForegroundColor Cyan
Write-Host '  Sistema de Seguridad ESP32' -ForegroundColor Cyan
Write-Host '=========================================' -ForegroundColor Cyan
Write-Host ''

$nodeCommand = Get-Command 'node.exe' -ErrorAction SilentlyContinue
$npmCommand = Get-Command 'npm.cmd' -ErrorAction SilentlyContinue
$mosquittoPath = Find-Mosquitto
$cloudflaredPath = Join-Path $projectRoot 'cloudflared.exe'

if (-not $nodeCommand) {
    Stop-WithMessage 'Node.js no esta instalado o no aparece en PATH. Instala Node.js 22.13 o posterior.'
}
if (-not $npmCommand) {
    Stop-WithMessage 'npm.cmd no esta disponible. Reinstala Node.js incluyendo npm.'
}
if (-not $mosquittoPath) {
    Stop-WithMessage 'Mosquitto no esta instalado. Instala Mosquitto para Windows y vuelve a intentarlo.'
}
if (-not (Test-Path -LiteralPath $cloudflaredPath -PathType Leaf)) {
    Stop-WithMessage 'Falta cloudflared.exe. Ejecuta Instalar-Sistema.cmd para descargarlo.'
}

$nodeVersion = & $nodeCommand.Source -p 'process.versions.node'
$nodeMajor = [int]($nodeVersion.Split('.')[0])
if ($nodeMajor -lt 22) {
    Stop-WithMessage "La version de Node.js es $nodeVersion. Se necesita Node.js 22.13 o posterior."
}

Write-Host "Node.js:    $nodeVersion" -ForegroundColor Green
Write-Host "Mosquitto:  $mosquittoPath" -ForegroundColor Green
Write-Host "Proyecto:   $projectRoot" -ForegroundColor Green

if ($CheckOnly) {
    Write-Host ''
    Write-Host 'Comprobacion terminada correctamente.' -ForegroundColor Green
    exit 0
}

if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'node_modules'))) {
    Write-Host ''
    Write-Host 'Instalando dependencias de Node.js por primera vez...' -ForegroundColor Yellow
    & $npmCommand.Source install
    if ($LASTEXITCODE -ne 0) {
        Stop-WithMessage 'npm install no pudo completar la instalacion.'
    }
}

$windowsServiceWasRunning = $false
$mosquittoService = Get-Service -Name 'mosquitto' -ErrorAction SilentlyContinue
if ($mosquittoService -and $mosquittoService.Status -eq 'Running') {
    $windowsServiceWasRunning = $true
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    $isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    if (-not $isAdministrator) {
        Write-Host ''
        Write-Host 'Windows pedira permiso para preparar el broker MQTT.' -ForegroundColor Yellow
        $elevatedArguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
        # La ventana elevada queda visible porque contiene los procesos y permite
        # que el usuario los detenga juntos con Ctrl+C.
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Normal -ArgumentList $elevatedArguments
        exit 0
    }

    Write-Host 'Deteniendo el servicio Mosquitto local que ocupa el puerto 1883...' -ForegroundColor Yellow
    Stop-Service -Name 'mosquitto' -Force
    Start-Sleep -Milliseconds 700
}

$busyPorts = @()
foreach ($port in 80, 1883, 2000) {
    if (-not (Test-PortFree -Port $port)) { $busyPorts += "TCP $port" }
}
if (-not (Test-PortFree -Port 3000 -Protocol 'UDP')) { $busyPorts += 'UDP 3000' }
if ($busyPorts.Count -gt 0) {
    if ($windowsServiceWasRunning) {
        try { Start-Service -Name 'mosquitto' } catch { }
    }
    Stop-WithMessage ("Estos puertos ya estan ocupados: " + ($busyPorts -join ', ') + '. Cierra la otra instancia y vuelve a intentarlo.')
}

$brokerProcess = $null
$serverProcess = $null

try {
    Write-Host ''
    Write-Host 'Iniciando broker MQTT...' -ForegroundColor Yellow
    $brokerConfig = Join-Path $projectRoot 'mosquitto.conf'
    $brokerProcess = New-ChildProcess -FileName $mosquittoPath -Arguments "-c `"$brokerConfig`" -v"
    Start-Sleep -Seconds 1
    if ($brokerProcess.HasExited) {
        Stop-WithMessage "Mosquitto termino con codigo $($brokerProcess.ExitCode)."
    }

    Write-Host 'Iniciando servidor web, WebSocket, camara y tunel publico...' -ForegroundColor Yellow
    $serverFile = Join-Path $projectRoot 'server\server.js'
    $tunnelUrlFile = Join-Path $projectRoot 'data\tunnel-url.txt'
    if (Test-Path -LiteralPath $tunnelUrlFile) {
        Remove-Item -LiteralPath $tunnelUrlFile -Force
    }
    $serverProcess = New-ChildProcess -FileName $nodeCommand.Source -Arguments "--disable-warning=ExperimentalWarning `"$serverFile`" --tunnel"
    Start-Sleep -Seconds 2
    if ($serverProcess.HasExited) {
        Stop-WithMessage "El servidor termino con codigo $($serverProcess.ExitCode)."
    }

    $lanAddress = Get-NetIPConfiguration -ErrorAction SilentlyContinue |
        Where-Object { $_.IPv4DefaultGateway -and $_.IPv4Address } |
        ForEach-Object { $_.IPv4Address.IPAddress } |
        Select-Object -First 1

    Write-Host ''
    Write-Host 'SISTEMA INICIADO CORRECTAMENTE' -ForegroundColor Green
    Write-Host 'Panel local:  http://localhost' -ForegroundColor Green
    if ($lanAddress) {
        Write-Host "Panel en LAN: http://$lanAddress" -ForegroundColor Green
        Write-Host "MQTT para ESP: $lanAddress puerto 1883" -ForegroundColor Green
    }

    Write-Host 'Esperando la URL publica de Cloudflare...' -ForegroundColor Yellow
    $publicUrl = $null
    for ($attempt = 0; $attempt -lt 90 -and -not $serverProcess.HasExited; $attempt++) {
        if (Test-Path -LiteralPath $tunnelUrlFile) {
            $candidateUrl = (Get-Content -LiteralPath $tunnelUrlFile -Raw).Trim()
            if ($candidateUrl -match '^https://[-a-z0-9]+\.trycloudflare\.com/?$') {
                $publicUrl = $candidateUrl
                break
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($publicUrl) {
        Write-Host "Panel publico: $publicUrl" -ForegroundColor Cyan
        Write-Host 'Abriendo el enlace publico en el navegador...' -ForegroundColor Cyan
        Start-Process -FilePath $publicUrl
    } else {
        Write-Host 'El tunel no entrego una URL en 45 segundos. Revisa los mensajes de Cloudflare.' -ForegroundColor DarkYellow
    }
    Write-Host ''
    Write-Host 'Deja esta ventana abierta. Presiona Ctrl+C para detener todo.' -ForegroundColor Cyan

    while (-not $brokerProcess.HasExited -and -not $serverProcess.HasExited) {
        Start-Sleep -Seconds 1
    }

    if ($brokerProcess.HasExited) {
        Stop-WithMessage "El broker MQTT se cerro con codigo $($brokerProcess.ExitCode)."
    }
    if ($serverProcess.HasExited) {
        Stop-WithMessage "El servidor se cerro con codigo $($serverProcess.ExitCode)."
    }
}
finally {
    Write-Host ''
    Write-Host 'Deteniendo componentes...' -ForegroundColor Yellow
    if ($serverProcess -and -not $serverProcess.HasExited) {
        $serverProcess.Kill()
        $serverProcess.WaitForExit(3000) | Out-Null
    }
    if ($brokerProcess -and -not $brokerProcess.HasExited) {
        $brokerProcess.Kill()
        $brokerProcess.WaitForExit(3000) | Out-Null
    }

    if ($windowsServiceWasRunning) {
        try {
            Start-Service -Name 'mosquitto'
        } catch {
            Write-Host 'No se pudo restaurar el servicio Mosquitto de Windows.' -ForegroundColor DarkYellow
        }
    }
}

