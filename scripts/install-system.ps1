param(
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot

function Stop-WithMessage([string]$Message) {
    Write-Host ''
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Update-ProcessPath {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machinePath;$userPath"
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

function Get-NodeStatus {
    $command = Get-Command 'node.exe' -ErrorAction SilentlyContinue
    if (-not $command) { return $null }
    $version = & $command.Source -p 'process.versions.node'
    return [pscustomobject]@{
        Path = $command.Source
        Version = $version
        Major = [int]($version.Split('.')[0])
    }
}

Write-Host '=========================================' -ForegroundColor Cyan
Write-Host '  Instalador - Sistema de Seguridad ESP32' -ForegroundColor Cyan
Write-Host '=========================================' -ForegroundColor Cyan
Write-Host ''

$nodeStatus = Get-NodeStatus
$mosquittoPath = Find-Mosquitto

if ($CheckOnly) {
    if ($nodeStatus) {
        Write-Host "Node.js detectado: $($nodeStatus.Version)" -ForegroundColor Green
    } else {
        Write-Host 'Node.js no esta instalado; el instalador lo descargaria.' -ForegroundColor Yellow
    }
    if ($mosquittoPath) {
        Write-Host "Mosquitto detectado: $mosquittoPath" -ForegroundColor Green
    } else {
        Write-Host 'Mosquitto no esta instalado; el instalador lo descargaria.' -ForegroundColor Yellow
    }
    Write-Host 'Modo de comprobacion: no se realizaron cambios.' -ForegroundColor Green
    exit 0
}

if (-not (Test-Administrator)) {
    Write-Host 'Windows pedira permiso de administrador para instalar y configurar el firewall.' -ForegroundColor Yellow
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Normal -ArgumentList $arguments
    exit 0
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$temporaryDirectory = Join-Path $env:TEMP ("sistema-seguridad-installer-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null

try {
    if (-not $nodeStatus -or $nodeStatus.Major -lt 22) {
        Write-Host 'Buscando la version LTS actual de Node.js...' -ForegroundColor Yellow
        $architecture = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
        $fileTag = "win-$architecture-msi"
        $releases = Invoke-RestMethod -Uri 'https://nodejs.org/dist/index.json'
        $release = $releases |
            Where-Object { $_.lts -and $_.files -contains $fileTag } |
            Select-Object -First 1
        if (-not $release) {
            Stop-WithMessage "No se encontro una version LTS de Node.js para $architecture."
        }

        $nodeFileName = "node-$($release.version)-$architecture.msi"
        $nodeUrl = "https://nodejs.org/dist/$($release.version)/$nodeFileName"
        $nodeInstaller = Join-Path $temporaryDirectory $nodeFileName
        Write-Host "Descargando Node.js $($release.version) desde nodejs.org..." -ForegroundColor Yellow
        Invoke-WebRequest -Uri $nodeUrl -OutFile $nodeInstaller -UseBasicParsing

        Write-Host 'Instalando Node.js...' -ForegroundColor Yellow
        $nodeInstall = Start-Process -FilePath 'msiexec.exe' -WindowStyle Hidden -Wait -PassThru `
            -ArgumentList @('/i', "`"$nodeInstaller`"", '/qn', '/norestart')
        if ($nodeInstall.ExitCode -notin 0, 3010) {
            Stop-WithMessage "El instalador de Node.js termino con codigo $($nodeInstall.ExitCode)."
        }
        Update-ProcessPath
        $nodeStatus = Get-NodeStatus
        if (-not $nodeStatus -or $nodeStatus.Major -lt 22) {
            Stop-WithMessage 'Node.js se instalo pero no pudo localizarse. Reinicia Windows y vuelve a ejecutar el instalador.'
        }
    } else {
        Write-Host "Node.js $($nodeStatus.Version) ya esta instalado." -ForegroundColor Green
    }

    $mosquittoPath = Find-Mosquitto
    if (-not $mosquittoPath) {
        Write-Host 'Buscando el instalador actual de Mosquitto...' -ForegroundColor Yellow
        $downloadPageUri = [uri]'https://mosquitto.org/download/'
        $downloadPage = Invoke-WebRequest -Uri $downloadPageUri -UseBasicParsing
        $match = [regex]::Match($downloadPage.Content, 'href=["'']([^"'']*mosquitto-[^"'']+-install-windows-x64\.exe)["'']', 'IgnoreCase')
        if (-not $match.Success) {
            Stop-WithMessage 'No se pudo encontrar el instalador Windows x64 en mosquitto.org.'
        }

        $mosquittoUrl = [uri]::new($downloadPageUri, $match.Groups[1].Value).AbsoluteUri
        $mosquittoInstaller = Join-Path $temporaryDirectory 'mosquitto-installer.exe'
        Write-Host 'Descargando Mosquitto desde mosquitto.org...' -ForegroundColor Yellow
        Invoke-WebRequest -Uri $mosquittoUrl -OutFile $mosquittoInstaller -UseBasicParsing

        Write-Host 'Instalando Mosquitto...' -ForegroundColor Yellow
        $mosquittoInstall = Start-Process -FilePath $mosquittoInstaller -WindowStyle Hidden -Wait -PassThru -ArgumentList '/S'
        if ($mosquittoInstall.ExitCode -ne 0) {
            Stop-WithMessage "El instalador de Mosquitto termino con codigo $($mosquittoInstall.ExitCode)."
        }
        Update-ProcessPath
        $mosquittoPath = Find-Mosquitto
        if (-not $mosquittoPath) {
            Stop-WithMessage 'Mosquitto se instalo pero no se encontro mosquitto.exe.'
        }
    } else {
        Write-Host "Mosquitto ya esta instalado en $mosquittoPath" -ForegroundColor Green
    }

    $cloudflaredPath = Join-Path $projectRoot 'cloudflared.exe'
    if (-not (Test-Path -LiteralPath $cloudflaredPath -PathType Leaf)) {
        $cloudflaredArchitecture = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
        $cloudflaredUrl = "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-$cloudflaredArchitecture.exe"
        Write-Host 'Descargando Cloudflare Tunnel desde el repositorio oficial...' -ForegroundColor Yellow
        Invoke-WebRequest -Uri $cloudflaredUrl -OutFile $cloudflaredPath -UseBasicParsing
    } else {
        Write-Host 'Cloudflare Tunnel ya esta disponible en el proyecto.' -ForegroundColor Green
    }

    $npmCommand = Get-Command 'npm.cmd' -ErrorAction SilentlyContinue
    if (-not $npmCommand) {
        Stop-WithMessage 'npm.cmd no esta disponible despues de instalar Node.js.'
    }

    Write-Host 'Instalando dependencias del proyecto...' -ForegroundColor Yellow
    & $npmCommand.Source install
    if ($LASTEXITCODE -ne 0) {
        Stop-WithMessage 'npm install no pudo completar la instalacion.'
    }

    Write-Host 'Configurando reglas de firewall para redes privadas...' -ForegroundColor Yellow
    $tcpRuleName = 'Sistema Seguridad ESP32 - TCP privado'
    $udpRuleName = 'Sistema Seguridad ESP32 - Camara UDP privada'
    if (-not (Get-NetFirewallRule -DisplayName $tcpRuleName -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $tcpRuleName -Direction Inbound -Action Allow `
            -Protocol TCP -LocalPort 80, 1883, 2000 -Profile Private | Out-Null
    }
    if (-not (Get-NetFirewallRule -DisplayName $udpRuleName -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $udpRuleName -Direction Inbound -Action Allow `
            -Protocol UDP -LocalPort 3000 -Profile Private | Out-Null
    }

    $desktop = [Environment]::GetFolderPath('Desktop')
    if ($desktop) {
        $shortcutPath = Join-Path $desktop 'Sistema de Seguridad ESP32.lnk'
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        $shortcut.TargetPath = Join-Path $projectRoot 'Iniciar-Sistema.cmd'
        $shortcut.WorkingDirectory = $projectRoot
        $shortcut.Description = 'Iniciar sistema de seguridad ESP32'
        $shortcut.Save()
        Write-Host "Acceso directo creado en: $shortcutPath" -ForegroundColor Green
    }

    Write-Host ''
    Write-Host 'INSTALACION COMPLETADA' -ForegroundColor Green
    Write-Host 'Usa Iniciar-Sistema.cmd o el acceso directo del escritorio.' -ForegroundColor Green
    Write-Host 'El acceso desde Internet permanece desactivado.' -ForegroundColor Green
    Write-Host ''
    Read-Host 'Presiona Enter para cerrar'
}
finally {
    $resolvedTemporary = [IO.Path]::GetFullPath($temporaryDirectory)
    $resolvedSystemTemp = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if ($resolvedTemporary.StartsWith($resolvedSystemTemp, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemporary)) {
        Remove-Item -LiteralPath $resolvedTemporary -Recurse -Force -ErrorAction SilentlyContinue
    }
}

