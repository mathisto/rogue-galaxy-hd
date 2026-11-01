# Rogue Galaxy Recompiled: the launcher behind Install.bat.
#
# The work is done by the setup program, tools\setup\rgsetup (python -m rgsetup; docs\INSTALLER.md). This script only
# finds Python (downloading the pinned one into toolchain\ when this PC has none), asks which disc image to use,
# starts the setup with the old Install.bat's choices (the game only), and offers a desktop shortcut afterwards.
# Install.bat always built just the game; the HD textures and movies come from Build-HD-Pack.bat, or from
# the setup itself:  python -m rgsetup --dest <this folder> --preset recommended
#
#   Install.bat                         (or drop the .iso on it; a .cue, .chd or a disc folder work too)
#   Install.bat -Iso "D:\Rogue Galaxy (USA).iso"
#   Install.bat -UseToolsOnPath         (developers: the toolchain already on PATH)
#   Install.bat -Yes                    (no questions: a short path is used if needed,
#                                        no shortcut is made and the game is not started)
#   Install.bat -Estimate               (checks this PC and the disc, shows the plan, disk and time; changes nothing)
#
# Windows PowerShell 5.1, so no newer syntax; plain ASCII, which 5.1 reads without a byte order mark.

param(
    [string]$Iso = "",
    [switch]$UseToolsOnPath,
    [switch]$Yes,
    [switch]$Estimate
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$ToolDir = Join-Path $Root "toolchain"
$System32 = Join-Path $env:SystemRoot "System32"
$SetupDir = Join-Path $Root "tools\setup"

# The deepest path the build writes is 185 characters below this folder
# (in build\tools\_deps), and Windows tools stop at 259 in all.
$MaxRootLength = 60

function Write-Step([string]$Text) {
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Stop-Install([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Red
    exit 1
}

function Confirm-Step([string]$Question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$Question [Y/n]"
    return ($answer -eq "" -or $answer -match "^(y|yes|s|sim)$")
}

# Why this folder cannot hold the build, or "" when it can.
function Get-PathProblem([string]$Path) {
    if ($Path.StartsWith("\\")) { return "it is on a network share" }
    foreach ($cloud in @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial)) {
        if ($cloud -and ($Path + "\").StartsWith($cloud.TrimEnd("\") + "\", [StringComparison]::OrdinalIgnoreCase)) {
            return "it is inside OneDrive, which would upload the 10 GB the build writes"
        }
    }
    if ($Path.Length -gt $MaxRootLength) {
        return "its path is too long for the build tools ($($Path.Length) characters; at most $MaxRootLength)"
    }
    if ($Path -match "[^A-Za-z0-9_.:\\-]") {
        return "its path has spaces, accented letters or symbols, which the build tools mishandle"
    }
    return ""
}

function Get-Drive([string]$Path) {
    if ($Path -match "^[A-Za-z]:") { return $Path.Substring(0, 2) }
    return ""
}

# Offers a copy of this folder at a short path and continues the install
# there; returns only when the player says no.
function Move-ToShortPath([string]$Problem) {
    $drive = Get-Drive $Root
    if (-not $drive -or $Problem -match "OneDrive|network") { $drive = $env:SystemDrive }
    $target = "$drive\RogueGalaxy"
    Write-Host ""
    Write-Host "This folder cannot hold the game's build: $Problem." -ForegroundColor Yellow
    Write-Host "  $Root"
    $script = Join-Path $target "tools\install\install.ps1"
    if (Test-Path -LiteralPath $script) {
        if (-not (Confirm-Step "An earlier copy is in $target. Continue the install there?")) { return }
    } else {
        if ((Test-Path -LiteralPath $target) -and @(Get-ChildItem -LiteralPath $target -Force).Count -gt 0) {
            Stop-Install "Move this folder to a short path without spaces, such as $target (which already holds other files), and run Install.bat there."
        }
        if (-not (Confirm-Step "Copy it to $target and install there?")) { return }
        Write-Step "Copying to $target"
        & (Join-Path $System32 "robocopy.exe") $Root $target /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Host
        if ($LASTEXITCODE -ge 8) { Stop-Install "Copying to $target failed (robocopy code $LASTEXITCODE)." }
        Write-Host "Copied. The game will live in $target; this folder can be deleted afterwards."
    }
    $forward = @()
    if ($Iso) { $forward += @("-Iso", $Iso) }
    if ($UseToolsOnPath) { $forward += "-UseToolsOnPath" }
    if ($Yes) { $forward += "-Yes" }
    & (Join-Path $System32 "WindowsPowerShell\v1.0\powershell.exe") -NoProfile -ExecutionPolicy Bypass -File $script @forward
    exit $LASTEXITCODE
}

function Invoke-Quiet([string]$Exe, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Exe @Arguments 2>&1 | Out-Null
        return $LASTEXITCODE
    } catch { return 1 } finally { $ErrorActionPreference = $saved }
}

function Test-Python([string]$Exe) {
    return ((Invoke-Quiet $Exe @("-c", "import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)")) -eq 0)
}

# The Python that runs the setup: the one in toolchain\python, else Python 3.11 or later on this PC.
function Find-Python {
    $own = Join-Path $ToolDir "python\python.exe"
    if ((Test-Path -LiteralPath $own) -and (Test-Python $own)) { return $own }
    foreach ($name in @("python", "py")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $exe = $cmd.Source
        # Windows' own python.exe may be the Store's stand-in, which fails the version check.
        if ($name -eq "py") {
            $ErrorActionPreference = "Continue"
            $real = & $exe -3 -c "import sys; print(sys.executable)" 2>$null
            $code = $LASTEXITCODE
            $ErrorActionPreference = "Stop"
            if ($code -eq 0 -and $real) { $exe = "$real".Trim() } else { continue }
        }
        if (Test-Python $exe) { return $exe }
    }
    return ""
}

# No Python on this PC: the pinned one is downloaded into toolchain\python (the same file the setup's toolchain
# step reads: tools\setup\rgsetup\data\toolchain.json), checked against its SHA-256.
function Install-Python {
    $pins = Get-Content -LiteralPath (Join-Path $SetupDir "rgsetup\data\toolchain.json") -Raw | ConvertFrom-Json
    $tool = $pins.tools | Where-Object { $_.name -eq "python" } | Select-Object -First 1
    Write-Host "This PC has no Python 3.11 or later. $($tool.label) ($($tool.mb) MB) will be downloaded into $ToolDir"
    Write-Host "  from $($tool.url)"
    Write-Host "  (it must have the SHA-256 $($tool.sha256)); nothing is installed in Windows."
    if (-not (Confirm-Step "Download it now?")) { Stop-Install "Python is needed. Run Install.bat again and answer y, or install Python 3.11 or later from python.org." }
    $downloads = Join-Path $ToolDir "downloads"
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $file = Join-Path $downloads ("python" + $tool.ext)
    $curl = Join-Path $System32 "curl.exe"
    if (Test-Path -LiteralPath $curl) {
        & $curl -L --fail --retry 3 --retry-delay 2 --progress-bar -o $file $tool.url
        if ($LASTEXITCODE -ne 0) { Stop-Install "Downloading Python failed (curl code $LASTEXITCODE). Check the internet connection and run Install.bat again." }
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $progress = $ProgressPreference
        $ProgressPreference = "SilentlyContinue"
        try { Invoke-WebRequest -Uri $tool.url -OutFile $file -UseBasicParsing }
        catch { Stop-Install "Downloading Python failed: $($_.Exception.Message)" }
        finally { $ProgressPreference = $progress }
    }
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $tool.sha256) {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        Stop-Install "The download of Python is not the expected file (its SHA-256 differs). Run Install.bat again; if this repeats, something on this network alters downloads."
    }
    $dir = Join-Path $ToolDir "python"
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Path $dir | Out-Null
    # The archive holds one folder (python\); its contents go straight into toolchain\python.
    & (Join-Path $System32 "tar.exe") -xf $file -C $dir --strip-components=1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $dir "python.exe"))) {
        Stop-Install "Unpacking Python failed. If an antivirus is running it may be holding the files; run Install.bat again."
    }
    Set-Content -LiteralPath (Join-Path $dir ".rg-sha256") -Value $tool.sha256 -Encoding Ascii
    Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    return (Join-Path $dir "python.exe")
}

function Select-DiscImage {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Rogue Galaxy (USA) disc image"
    $dialog.Filter = "Disc images (*.iso;*.cue;*.bin;*.chd)|*.iso;*.cue;*.bin;*.chd|All files (*.*)|*.*"
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.FileName }
    } finally {
        $owner.Dispose()
    }
    return ""
}

function Main {
    try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
    $version = "?"
    $project = Get-Content -LiteralPath (Join-Path $Root "CMakeLists.txt") -Raw -ErrorAction SilentlyContinue
    if ($project -match "VERSION\s+(\d+\.\d+\.\d+)") { $version = $Matches[1] }
    $Host.UI.RawUI.WindowTitle = "Rogue Galaxy Recompiled $version installer"
    Write-Host "Rogue Galaxy Recompiled $version installer" -ForegroundColor Green
    Write-Host "Builds the game on this PC from your own disc image. Everything goes into this folder:"
    Write-Host "  $Root"

    if (-not (Test-Path -LiteralPath (Join-Path $SetupDir "rgsetup\cli.py"))) {
        Stop-Install "tools\setup is not beside Install.bat. Extract the whole zip first, then run Install.bat from the extracted folder."
    }
    if (-not [Environment]::Is64BitOperatingSystem) { Stop-Install "The game needs 64-bit Windows 10 or later." }

    $problem = Get-PathProblem $Root
    if ($problem) {
        Move-ToShortPath $problem
        Stop-Install "Move this folder to a short path without spaces, such as C:\RogueGalaxy, and run Install.bat there."
    }

    # The disc is asked for only when disc\ does not hold it yet.
    $extracted = (Test-Path -LiteralPath (Join-Path $Root "disc\manifest.json")) -and
        (Test-Path -LiteralPath (Join-Path $Root "disc\SCUS_974.90"))
    if ($extracted -and $Iso) {
        Write-Host ""
        Write-Host "disc\ already holds the extracted disc, so $Iso is not used (delete disc\manifest.json to extract it instead)."
        $Iso = ""
    }
    if (-not $extracted) {
        Write-Step "Your disc image"
        if (-not $Iso) {
            Write-Host "Choose the image of your Rogue Galaxy disc (USA, SCUS-97490) in the window that opens."
            $Iso = Select-DiscImage
            if (-not $Iso) { Stop-Install "No disc image chosen. Run Install.bat again when you have it." }
        }
        if (-not (Test-Path -LiteralPath $Iso)) { Stop-Install "There is no file or folder $Iso." }
        Write-Host "  $Iso"
    }

    $python = Find-Python
    if (-not $python) { $python = Install-Python }

    # The setup, with the choices Install.bat always had: the game only, build tools downloaded into toolchain\.
    $arguments = @("-X", "utf8", "-m", "rgsetup", "--dest", $Root, "--preset", "faithful", "--yes", "--format", "text")
    if ($Iso) { $arguments += @("--source", $Iso) }
    if ($UseToolsOnPath) { $arguments += "--tools-on-path" }
    if ($Estimate) { $arguments += "--estimate" }
    Write-Step "Building the game"
    Write-Host "This takes about 20 minutes on a fast PC, up to an hour on a slower one; it can be left alone."
    Write-Host "Clicking inside this window pauses it (the title then starts with ""Select""); press Esc to go on."
    Write-Host "Everything shown here also goes to install.log. The same as: python -m rgsetup $($arguments[4..($arguments.Count - 1)] -join ' ')"
    # install.log keeps what the last install showed; an estimate changes nothing, so it leaves the file alone.
    $log = if ($Estimate) { Join-Path $env:TEMP "rg-install-estimate.log" } else { Join-Path $Root "install.log" }
    $writer = New-Object System.IO.StreamWriter($log, $false, (New-Object System.Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    # Native programs' error output arrives as error records; with "Stop" the first of them would end the script.
    $ErrorActionPreference = "Continue"
    $env:PYTHONPATH = $SetupDir
    $env:PYTHONIOENCODING = "utf-8"
    $env:PYTHONNOUSERSITE = "1"
    Push-Location -LiteralPath $Root
    try {
        & $python @arguments 2>&1 | ForEach-Object {
            $line = "$_"
            $writer.WriteLine($line)
            Write-Host $line
        }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $writer.Close()
    }
    $ErrorActionPreference = "Stop"
    $game = Join-Path $Root "Rogue Galaxy.exe"
    if ($Estimate) { exit $code }
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $game)) {
        $hint = ""
        $text = Get-Content -LiteralPath $log -Raw
        if ($text -match "No such file or directory|is not recognized|Access is denied") {
            $hint = " A build tool went missing or was blocked: an antivirus may have quarantined it (restore it, or allow $ToolDir)."
        }
        Stop-Install "The install stopped (code $code).$hint The full output is in $log. Run Install.bat again to continue from where it stopped."
    }

    Write-Step "Done"
    Write-Host "The game is $game"
    Write-Host "Optional: Build-HD-Pack.bat builds HD textures and HD movies from your disc (docs/HD-BUILD.md)."
    if (-not $Yes -and (Confirm-Step "Put a shortcut to it on the desktop?")) {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Desktop")) "Rogue Galaxy.lnk"))
        $link.TargetPath = $game
        $link.WorkingDirectory = $Root
        $link.IconLocation = "$game,0"
        $link.Save()
        Write-Host "  shortcut made"
    }
    if (-not $Yes -and (Confirm-Step "Start the game now?")) { Start-Process -FilePath $game -WorkingDirectory $Root }
}

# Dot-sourced (tests), only the functions.
if ($MyInvocation.InvocationName -ne ".") { Main }
