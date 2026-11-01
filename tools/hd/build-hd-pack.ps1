# Rogue Galaxy Recompiled: the launcher behind Build-HD-Pack.bat.
#
# The work is done by the setup program, tools\setup\rgsetup (docs\INSTALLER.md), which drives tools\hd\build_hd_pack.py.
# This script finds Python (the one Install.bat put in toolchain\python, or Python 3.11+ on this PC), shows the old
# menu (or reads the old options), and starts the setup for just the HD parts:
#
#   Build-HD-Pack.bat                               (menu: [1] textures [2] movies [3] both)
#   Build-HD-Pack.bat --mode 3 --gentle             (everything, low priority)
#   Build-HD-Pack.bat --estimate                    (plan, disk and time; changes nothing)
#   Build-HD-Pack.bat "D:\Rogue Galaxy (USA).iso"   (an .iso dropped on it: used only if disc\ is not extracted)
#   Build-HD-Pack.bat --yes                         (no questions, downloads included)
#
# These options become the setup's:  --mode 1|2|3 -> --components textures | movies | textures,movies;
# --estimate --gentle --yes --limit N --limit-movies N --clean-work --gpu N are the same; --iso PATH -> --source PATH.
# The line it runs is printed first. Options only the HD builder itself has (--redo, --work, --out, --disc, --sample-log,
# --stall-timeout, --retry-failed, --skip-space-check) run tools\hd\build_hd_pack.py directly, as before.
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$ToolDir = Join-Path $Root "toolchain"
$Libs = Join-Path $ToolDir "pylibs"
$SetupDir = Join-Path $Root "tools\setup"

function Stop-Build([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Red
    exit 1
}

# Runs a native program and returns its exit code. Windows PowerShell 5.1 turns a native program's stderr output
# into a terminating error under "Stop", so the preference is relaxed around the call.
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

function Confirm-Step([string]$Question, [bool]$Assume) {
    if ($Assume) { return $true }
    $answer = Read-Host "$Question [y/N]"
    return ($answer -match "^(y|yes|s|sim)$")
}

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
$Host.UI.RawUI.WindowTitle = "Rogue Galaxy HD pack builder"

if (-not (Test-Path -LiteralPath (Join-Path $SetupDir "rgsetup\cli.py"))) {
    Stop-Build "tools\setup is not here. Extract the whole zip first."
}

$python = Find-Python
if (-not $python) {
    Stop-Build ("No Python 3.11 or later was found. Run Install.bat first (it puts Python into toolchain\python), " +
        "or install Python from python.org and run this again.")
}

# Arguments: an .iso path anywhere on the line (dropped on the .bat) becomes --iso; the rest is read below.
$forward = @()
foreach ($a in $args) {
    if ($a -match "\.(iso|cue|chd)$" -and (Test-Path -LiteralPath $a -PathType Leaf)) { $forward += @("--iso", $a) }
    else { $forward += $a }
}
$assumeYes = $forward -contains "--yes"
$isEstimate = $forward -contains "--estimate"

# Which options does the setup know?
$mode = 0
$iso = ""
$passthrough = @()
$advanced = $false
for ($i = 0; $i -lt $forward.Count; $i++) {
    switch ($forward[$i]) {
        "--mode" { $i++; $mode = [int]$forward[$i] }
        "--iso" { $i++; $iso = $forward[$i] }
        { $_ -in @("--estimate", "--gentle", "--yes", "--clean-work") } { $passthrough += $_ }
        { $_ -in @("--limit", "--limit-movies", "--gpu") } { $passthrough += @($_, $forward[$i + 1]); $i++ }
        default { $advanced = $true }
    }
}

$env:PYTHONPATH = $Libs
$env:PYTHONNOUSERSITE = "1"

if ($advanced) {
    # Options of the HD builder itself: it runs directly, as it always has (see tools\hd\build_hd_pack.py --help).
    Write-Host "These options belong to the HD builder itself; running tools\hd\build_hd_pack.py directly." -ForegroundColor Yellow
    if ((-not $isEstimate) -and ((Invoke-Quiet $python @("-c", "import numpy, PIL")) -ne 0)) {
        Write-Host ""
        Write-Host "The builder needs two Python packages: numpy and Pillow (tools\hd\requirements.txt)." -ForegroundColor Yellow
        Write-Host "  They would be downloaded from pypi.org (about 40 MB) with pip into $Libs"
        Write-Host "  (nothing is installed in Windows or in your Python)."
        if (-not (Confirm-Step "Download and install them now?" $assumeYes)) { Stop-Build "numpy and Pillow are needed. Run this again and answer y." }
        $ErrorActionPreference = "Continue"
        & $python -m pip install --disable-pip-version-check --target $Libs -r (Join-Path $PSScriptRoot "requirements.txt")
        $code = $LASTEXITCODE
        $ErrorActionPreference = "Stop"
        if ($code -ne 0) { Stop-Build "Installing numpy and Pillow failed (pip code $code). Check the internet connection and run this again." }
    }
    Push-Location -LiteralPath $Root
    try {
        & $python -X utf8 (Join-Path $PSScriptRoot "build_hd_pack.py") @forward
        exit $LASTEXITCODE
    } finally { Pop-Location }
}

# The old menu.
if ($mode -eq 0 -and -not $isEstimate) {
    Write-Host ""
    Write-Host "What do you want to build?"
    Write-Host "  [1] HD textures only   (about 20 to 30 minutes: about 5 of preparation, then 15 to 25 of upscaling)"
    Write-Host "  [2] HD movies only     (about 1.5 hours)"
    Write-Host "  [3] Both"
    Write-Host "  [E] Show the plan and the estimates only"
    Write-Host "  [Q] Quit"
    Write-Host "  (times are for the author's RTX 5070 Ti; the graphics card decides)"
    while ($true) {
        $c = (Read-Host "Choose 1, 2, 3, E or Q").Trim().ToLower()
        if ($c -in @("1", "2", "3")) { $mode = [int]$c; break }
        if ($c -eq "e") { $isEstimate = $true; $passthrough += "--estimate"; $mode = 3; break }
        if ($c -in @("q", "")) { exit 0 }
    }
}
if ($mode -eq 0) { $mode = 3 }
$components = @{ 1 = "textures"; 2 = "movies"; 3 = "textures,movies" }[$mode]
$hdSteps = @{ 1 = "hd_textures"; 2 = "hd_movies"; 3 = "hd_textures,hd_movies" }[$mode]

# The disc: Install.bat extracts it into disc\. Without it, ask for the image.
$discOk = (Test-Path -LiteralPath (Join-Path $Root "disc\manifest.json")) -and (Test-Path -LiteralPath (Join-Path $Root "disc\SCUS_974.90"))
if (-not $discOk -and -not $iso -and -not $isEstimate) {
    Write-Host ""
    Write-Host "disc\ does not hold the extracted disc yet." -ForegroundColor Yellow
    Write-Host "Choose the image of your Rogue Galaxy disc (USA, SCUS-97490) in the window that opens;"
    Write-Host "it will be extracted into disc\ first (about 7 GB, a few minutes)."
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Rogue Galaxy (USA) disc image"
    $dialog.Filter = "Disc images (*.iso;*.cue;*.bin;*.chd)|*.iso;*.cue;*.bin;*.chd|All files (*.*)|*.*"
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { $iso = $dialog.FileName }
    } finally { $owner.Dispose() }
    if (-not $iso) { Stop-Build "No disc image chosen. Run Install.bat, or run this again with the image." }
}

# Only the disc and the HD steps of the chosen mode: the game itself (Install.bat) is not touched, so a game that is running stays.
$arguments = @("-X", "utf8", "-m", "rgsetup", "--dest", $Root, "--components", $components, "--only", "check,source,extract,$hdSteps,finish", "--format", "text") + $passthrough
if ($iso -and -not $discOk) { $arguments += @("--source", $iso) }
Write-Host ""
Write-Host "Running: python -m rgsetup $($arguments[4..($arguments.Count - 1)] -join ' ')" -ForegroundColor Cyan
Push-Location -LiteralPath $Root
try {
    $env:PYTHONPATH = $SetupDir
    $env:PYTHONIOENCODING = "utf-8"
    & $python @arguments
    exit $LASTEXITCODE
} finally { Pop-Location }
