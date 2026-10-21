# Takes a screenshot of the game window (out\jitter\shots\markN.png) for every "Jitter mark N (F6" in the newest log.
$out = Join-Path $PSScriptRoot '..\..\out'
$logs = Join-Path $out 'logs'
$shots = New-Item -ItemType Directory -Force (Join-Path $out 'jitter\shots')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class WC { [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public struct R { public int L,T,Ri,B; } public struct P { public int X,Y; } }
'@
[WC]::SetProcessDPIAware() | Out-Null
function Shot($name) {
    $p = Get-Process tlg-probe -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return }
    $r = New-Object WC+R; [WC]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $o = New-Object WC+P; [WC]::ClientToScreen($p.MainWindowHandle, [ref]$o) | Out-Null
    # Vulkan content is not in PrintWindow: copy the screen pixels under the client area.
    $bmp = New-Object System.Drawing.Bitmap ($r.Ri), ($r.B); $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
    $bmp.Save((Join-Path $shots "$name.png")); $g.Dispose(); $bmp.Dispose()
}
Start-Sleep 5
$start = Get-Date
$log = $null
while (-not $log) {
    $log = Get-ChildItem $logs -File | Where-Object { $_.LastWriteTime -ge $start.AddSeconds(-30) } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Start-Sleep 1
}
$seen = 0
while ($true) {
    $running = Get-Process tlg-probe -ErrorAction SilentlyContinue
    $text = Get-Content $log.FullName -Raw -ErrorAction SilentlyContinue
    if ($text) {
        foreach ($m in [regex]::Matches($text, 'Jitter mark (\d+) \(F6')) {
            $n = [int]$m.Groups[1].Value
            if ($n -gt $seen) { $seen = $n; Shot "mark$n" }
        }
    }
    if (-not $running -and ((Get-Date) - $start).TotalSeconds -gt 120) { break }
    Start-Sleep -Milliseconds 300
}
