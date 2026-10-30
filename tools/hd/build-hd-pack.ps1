# Rogue Galaxy Recompiled: the launcher behind Build-HD-Pack.bat.
#
# Finds Python (the one Install.bat put in toolchain\python, or Python 3.10+ on this PC), makes sure numpy and
# Pillow are there (installed into toolchain\pylibs after asking; nothing is installed in Windows), finds your
# disc (extracted by Install.bat into disc\; otherwise asks for the .iso and extracts it), then runs
# tools\hd\build_hd_pack.py, which shows a menu: [1] textures [2] movies [3] both.
#
#   Build-HD-Pack.bat                               (menu)
#   Build-HD-Pack.bat --mode 3 --gentle             (everything, low priority)
#   Build-HD-Pack.bat --estimate                    (plan, disk and time; changes nothing)
#   Build-HD-Pack.bat "D:\Rogue Galaxy (USA).iso"   (an .iso dropped on it: used only if disc\ is not extracted)
#   Build-HD-Pack.bat --yes                         (no questions, downloads included)
#
# Every other argument goes to build_hd_pack.py unchanged (see its --help).
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$ToolDir = Join-Path $Root "toolchain"
$Libs = Join-Path $ToolDir "pylibs"

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
    return ((Invoke-Quiet $Exe @("-c", "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)")) -eq 0)
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

if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot "build_hd_pack.py"))) {
    Stop-Build "build_hd_pack.py is not beside this script. Extract the whole zip first."
}

$python = Find-Python
if (-not $python) {
    Stop-Build ("No Python 3.10 or later was found. Run Install.bat first (it puts Python into toolchain\python), " +
        "or install Python from python.org and run this again.")
}

# Arguments: an .iso path anywhere on the line (dropped on the .bat) becomes --iso; the rest is passed on.
$forward = @()
$assumeYes = $false
foreach ($a in $args) {
    if ($a -eq "--yes") { $assumeYes = $true }
    if ($a -match "\.iso$" -and (Test-Path -LiteralPath $a -PathType Leaf)) { $forward += @("--iso", $a) }
    else { $forward += $a }
}

# Python packages.
$env:PYTHONPATH = $Libs
$env:PYTHONNOUSERSITE = "1"
if ((Invoke-Quiet $python @("-c", "import numpy, PIL")) -ne 0) {
    Write-Host ""
    Write-Host "The builder needs two Python packages: numpy and Pillow (tools\hd\requirements.txt)." -ForegroundColor Yellow
    Write-Host "  They would be downloaded from pypi.org (about 40 MB) with pip into $Libs"
    Write-Host "  (nothing is installed in Windows or in your Python)."
    if (-not (Confirm-Step "Download and install them now?" $assumeYes)) {
        Stop-Build "numpy and Pillow are needed. Run this again and answer y."
    }
    $req = Join-Path $PSScriptRoot "requirements.txt"
    $ErrorActionPreference = "Continue"
    & $python -m pip install --disable-pip-version-check --target $Libs -r $req
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($code -ne 0) {
        Stop-Build "Installing numpy and Pillow failed (pip code $code). Check the internet connection and run this again."
    }
    if ((Invoke-Quiet $python @("-c", "import numpy, PIL")) -ne 0) { Stop-Build "numpy and Pillow still cannot be imported from $Libs." }
}

# The disc: Install.bat extracts it into disc\. Without it, ask for the image and extract it with the project's tool.
$discOk = (Test-Path -LiteralPath (Join-Path $Root "disc\DATA0_0.DAT")) -and (Test-Path -LiteralPath (Join-Path $Root "disc\DATA0_1.DAT"))
$isEstimate = $forward -contains "--estimate"
if (-not $discOk -and -not ($forward -contains "--iso") -and -not $isEstimate) {
    Write-Host ""
    Write-Host "disc\ does not hold the extracted disc yet." -ForegroundColor Yellow
    Write-Host "Choose the image of your Rogue Galaxy disc (USA, SCUS-97490) in the window that opens;"
    Write-Host "it will be extracted into disc\ first (about 7 GB, a few minutes)."
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Rogue Galaxy (USA) disc image"
    $dialog.Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*"
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { $forward += @("--iso", $dialog.FileName) }
    } finally { $owner.Dispose() }
    if (-not ($forward -contains "--iso")) { Stop-Build "No disc image chosen. Run Install.bat, or run this again with the image." }
}

if (-not $discOk -and ($forward -contains "--iso") -and -not $isEstimate) {
    $i = [array]::IndexOf($forward, "--iso")
    $iso = $forward[$i + 1]
    if (-not (Test-Path -LiteralPath $iso -PathType Leaf)) { Stop-Build "There is no file $iso." }
    Write-Host ""
    Write-Host "== Extracting your disc image into disc\ (setup.py extract)" -ForegroundColor Cyan
    $ErrorActionPreference = "Continue"
    Push-Location -LiteralPath $Root
    try { & $python -u (Join-Path $Root "setup.py") extract --iso $iso } finally { Pop-Location }
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($code -ne 0) {
        Stop-Build "Extracting the disc failed (code $code). Is it the USA disc image (Redump 'Rogue Galaxy (USA)', 6,967,721,984 bytes)? Run Install.bat for the full check."
    }
}

Push-Location -LiteralPath $Root
try {
    & $python -X utf8 (Join-Path $PSScriptRoot "build_hd_pack.py") @forward
    exit $LASTEXITCODE
} finally { Pop-Location }
