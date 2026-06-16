param(
    [string]$DllPath = (Join-Path $PSScriptRoot "..\lib\PCIe3640.dll"),
    [int]$DeviceNumber = 0,
    [int]$DaAmplitudeMv = 500,
    [int]$DaDelay = 0,
    [int]$RfDelay = 0,
    [int]$PwmDelay = 0,
    [int]$RfAgc = 1023
)

$ErrorActionPreference = "Stop"

$resolvedDll = Resolve-Path -LiteralPath $DllPath
$dllDir = Split-Path -Parent $resolvedDll
$oldPath = $env:PATH
$env:PATH = "$dllDir;$oldPath"

$source = @"
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)]
public struct PcieBuf
{
    public IntPtr pDABufA;
    public IntPtr pDABufB;
    public IntPtr pADBufA;
    public IntPtr pADBufB;
}

public static class Pcie3640
{
    [DllImport("PCIe3640.dll", CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr PCIe3640_Link(int devNum, ref PcieBuf pcieBuf);

    [DllImport("PCIe3640.dll", CallingConvention = CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PCIe3640_UnLink(IntPtr hdl);

    [DllImport("PCIe3640.dll", CallingConvention = CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PCIe3640_intDA(IntPtr hdl);

    [DllImport("PCIe3640.dll", CallingConvention = CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PCIe3640_SetDA(IntPtr hdl, int selDA, int cycCnt, UInt16[] buf, int delay, int agc);

    [DllImport("PCIe3640.dll", CallingConvention = CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PCIe3640_StartDA(IntPtr hdl, int stDA, int pwmDelay);
}
"@

if (-not ("Pcie3640" -as [type])) {
    Add-Type -TypeDefinition $source
}

function New-RfSineBuffer {
    $length = 64 * 1024
    $cycles = 1024
    $period = [int]($length / $cycles)
    $buffer = New-Object UInt16[] $length
    for ($i = 0; $i -lt $period; $i++) {
        $value = [int][Math]::Round(32767.5 * ([Math]::Sin(2.0 * [Math]::PI * $i / $period) + 1.0))
        if ($value -lt 0) { $value = 0 }
        if ($value -gt 65535) { $value = 65535 }
        $buffer[$i] = [UInt16]$value
    }
    for ($cycle = 1; $cycle -lt $cycles; $cycle++) {
        [Array]::Copy($buffer, 0, $buffer, $cycle * $period, $period)
    }
    return $buffer
}

function New-LowSpeedSquareBuffer {
    param([int]$amplitudeMv)
    $length = 32
    $amplitudeCode = [int][Math]::Round([Math]::Abs($amplitudeMv) * 65536.0 / 10000.0)
    if ($amplitudeCode -gt 32767) { $amplitudeCode = 32767 }
    $high = 32768 + $amplitudeCode
    $low = 32768 - $amplitudeCode
    $buffer = New-Object UInt16[] $length
    for ($i = 0; $i -lt $length; $i++) {
        if (($i % 2) -eq 0) {
            $buffer[$i] = [UInt16]$high
        }
        else {
            $buffer[$i] = [UInt16]$low
        }
    }
    return $buffer
}

$pcieBuf = New-Object PcieBuf
$handle = [Pcie3640]::PCIe3640_Link($DeviceNumber, [ref]$pcieBuf)
if ($handle -eq [IntPtr]::Zero -or $handle -eq [IntPtr](-1)) {
    throw "PCIe3640_Link failed for device $DeviceNumber."
}

try {
    $rf = New-RfSineBuffer
    $da = New-LowSpeedSquareBuffer -amplitudeMv $DaAmplitudeMv

    if (-not [Pcie3640]::PCIe3640_intDA($handle)) {
        throw "PCIe3640_intDA failed."
    }
    $okRf = [Pcie3640]::PCIe3640_SetDA($handle, 0, $rf.Length, $rf, $RfDelay, $RfAgc)
    $okDa1 = [Pcie3640]::PCIe3640_SetDA($handle, 1, $da.Length, $da, $DaDelay, 0)
    $okDa2 = [Pcie3640]::PCIe3640_SetDA($handle, 2, $da.Length, $da, $DaDelay, 0)
    $okDa3 = [Pcie3640]::PCIe3640_SetDA($handle, 3, $da.Length, $da, $DaDelay, 0)
    $okDa4 = [Pcie3640]::PCIe3640_SetDA($handle, 4, $da.Length, $da, $DaDelay, 0)
    if (-not ($okRf -and $okDa1 -and $okDa2 -and $okDa3 -and $okDa4)) {
        throw "PCIe3640_SetDA failed: RF=$okRf DA1=$okDa1 DA2=$okDa2 DA3=$okDa3 DA4=$okDa4"
    }
    if (-not [Pcie3640]::PCIe3640_StartDA($handle, 1, $PwmDelay)) {
        throw "PCIe3640_StartDA failed."
    }

    Write-Host "PCIe3640 DA self-test is running."
    Write-Host "RF: 64-sample sine repeated in 64K buffer, RF AGC=$RfAgc."
    Write-Host "LDA1-LDA4: 32-sample square around 0V, amplitude approx +/-$DaAmplitudeMv mV."
    Read-Host "Measure outputs now, then press Enter to reset DAC and exit"
}
finally {
    try { [Pcie3640]::PCIe3640_intDA($handle) | Out-Null } catch {}
    try { [Pcie3640]::PCIe3640_UnLink($handle) | Out-Null } catch {}
    $env:PATH = $oldPath
}
