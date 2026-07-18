param([long]$Hwnd,[string]$Out)
Add-Type -AssemblyName System.Drawing
$sig = @"
using System;
using System.Runtime.InteropServices;
public class U {
  [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr a, IntPtr b, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
}
"@
Add-Type $sig
$h=[IntPtr]$Hwnd
[U]::ShowWindow($h,9)  | Out-Null        # SW_RESTORE
[U]::SetForegroundWindow($h) | Out-Null
[U]::RedrawWindow($h,[IntPtr]::Zero,[IntPtr]::Zero,0x101) | Out-Null  # RDW_INVALIDATE|RDW_UPDATENOW
Start-Sleep -Milliseconds 500
$r=New-Object U+RECT
[U]::GetWindowRect($h,[ref]$r) | Out-Null
$w=$r.R-$r.L; $ht=$r.B-$r.T
$bmp=New-Object System.Drawing.Bitmap($w,$ht)
$g=[System.Drawing.Graphics]::FromImage($bmp)
try { $g.CopyFromScreen($r.L,$r.T,0,0,(New-Object System.Drawing.Size($w,$ht))) } catch {}
$bmp.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png)
# also try PrintWindow into a second file (works even if occluded)
$bmp2=New-Object System.Drawing.Bitmap($w,$ht)
$g2=[System.Drawing.Graphics]::FromImage($bmp2)
$hdc=$g2.GetHdc(); [U]::PrintWindow($h,$hdc,2) | Out-Null; $g2.ReleaseHdc($hdc)
$bmp2.Save(($Out -replace '\.png$','_pw.png'),[System.Drawing.Imaging.ImageFormat]::Png)
"$w x $ht saved"
