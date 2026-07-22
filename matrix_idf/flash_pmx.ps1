param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ContentPath,
    [string]$Port = 'COM20'
)

$assetOffset = '0x110000'
$assetCapacity = 0x2F0000
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ContentPath))

if ($bytes.Length -lt 24) { throw 'The PMX file is too short.' }
if ([System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'PMX1') { throw 'The file is not PMX1 content.' }
if ($bytes[4] -ne 1 -or $bytes[5] -ne 1 -or $bytes[6] -ne 64 -or $bytes[7] -ne 64) {
    throw 'Unsupported PMX format. Expected PMX1 RGB332 64x64 content.'
}

$declaredSize = [System.BitConverter]::ToUInt32($bytes, 20)
if ($declaredSize -ne $bytes.Length) { throw 'PMX length does not match its header.' }
if ($bytes.Length -gt $assetCapacity) { throw 'PMX content exceeds the 3008KB assets partition.' }

$python = 'D:\espidf\v6.0.2\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe'
$esptool = 'D:\espidf\v6.0.2\esp-idf\components\esptool_py\esptool\esptool.py'
if (-not (Test-Path -LiteralPath $python) -or -not (Test-Path -LiteralPath $esptool)) {
    throw 'ESP-IDF Python or esptool was not found at the configured D: drive path.'
}

Write-Host "Writing $($bytes.Length) bytes to $Port at $assetOffset..."
& $python $esptool --chip esp32 -p $Port -b 115200 --before default-reset --after hard-reset write-flash $assetOffset $ContentPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host 'PMX content written. The ESP32 reset and starts playback automatically.'
