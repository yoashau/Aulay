# Rebuild the existing speaker/Bluetooth silhouette on a 16-DIP grid.
# A 1-DIP stroke and #202020 foreground match the Windows/Portkey line weight.
# Requires only the Windows WPF assemblies; no imaging packages or fonts.
param([string]$Output = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Aulay.ico'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore,WindowsBase,PresentationFramework
$sizes = @(16,20,24,28,32,40,48,64,96,128,256)
$speaker = [Windows.Media.Geometry]::Parse('M1.5,5.5 H4.5 L8.5,2.5 V13.5 L4.5,10.5 H1.5 Z')
# Widen the Bluetooth lobes; keep its left edge, stem and speaker gap fixed.
$bluetooth = [Windows.Media.Geometry]::Parse('M10.5,5.5 L15,10.5 L12.5,12.5 V3.5 L15,5.5 L10.5,10.5')
$brush = [Windows.Media.SolidColorBrush]::new([Windows.Media.Color]::FromRgb(32,32,32))
$pen = [Windows.Media.Pen]::new($brush,1)
$pen.LineJoin = [Windows.Media.PenLineJoin]::Round
$pen.StartLineCap = $pen.EndLineCap = [Windows.Media.PenLineCap]::Round
$frames = @()
foreach ($n in $sizes) {
    $visual = [Windows.Media.DrawingVisual]::new()
    $drawing = $visual.RenderOpen()
    $drawing.PushTransform([Windows.Media.ScaleTransform]::new($n/16.0,$n/16.0))
    $drawing.DrawGeometry($null,$pen,$speaker)
    $drawing.DrawGeometry($null,$pen,$bluetooth)
    $drawing.Close()
    $bitmap = [Windows.Media.Imaging.RenderTargetBitmap]::new($n,$n,96,96,[Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $encoder = [Windows.Media.Imaging.PngBitmapEncoder]::new()
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = [IO.MemoryStream]::new()
    try { $encoder.Save($stream); $frames += ,$stream.ToArray() } finally { $stream.Dispose() }
}
$stream = [IO.File]::Create($Output)
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i=0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) {0} else {$sizes[$i]}
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
} finally { $writer.Dispose() }
Write-Output "Generated 11 native icon sizes: $Output"
