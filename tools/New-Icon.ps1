# Gera assets\icon.ico: Erdtree dourada em pixel art (32x32 escalado, sem imagens externas).
Add-Type -AssemblyName System.Drawing
$root = (Split-Path $PSScriptRoot)
$N = 32
$g = New-Object 'System.Drawing.Color[,]' $N,$N
function C($r,$gg,$b,$a=255){ [System.Drawing.Color]::FromArgb($a,$r,$gg,$b) }
$bgTop=C 14 10 6; $bgBot=C 40 26 8
for($y=0;$y -lt $N;$y++){ for($x=0;$x -lt $N;$x++){
  $t=$y/($N-1); $g[$x,$y]=C ([int](14+(40-14)*$t)) ([int](10+(26-10)*$t)) ([int](6+(8-6)*$t)) } }
# brilho atras da copa
for($y=0;$y -lt $N;$y++){ for($x=0;$x -lt $N;$x++){
  $d=[math]::Sqrt(([math]::Pow($x-15.5,2))+([math]::Pow(($y-12)*1.25,2)))
  if($d -lt 13){ $k=1-$d/13; $c=$g[$x,$y]
    $g[$x,$y]=C ([math]::Min(255,[int]($c.R+120*$k*$k))) ([math]::Min(255,[int]($c.G+80*$k*$k))) ([math]::Min(255,[int]($c.B+14*$k*$k))) } } }
# copa: elipse com ruido deterministico
$seed=7
for($y=1;$y -lt 20;$y++){ for($x=2;$x -lt 30;$x++){
  $d=[math]::Pow(($x-15.5)/13.5,2)+[math]::Pow(($y-9.5)/8.8,2)
  $seed=($seed*1103515245+12345) % 2147483647; $nz=($seed % 1000)/1000.0
  if($d -lt (0.82+0.28*$nz) -and $d -lt 1.05){
    $k=1-[math]::Min(1,$d); $v=[math]::Min(1,0.45+0.55*$k+0.25*($nz-0.5))
    $g[$x,$y]=C ([int](150+105*$v)) ([int](100+110*$v)) ([int](20+90*$v*$v)) } } }
# tronco
for($y=17;$y -lt 28;$y++){ $w=[int](1+($y-17)*0.22); for($x=[int](15.5-$w);$x -le [int](16+$w-1);$x++){
  $shade=if($x -lt 16){1.0}else{0.78}; $g[$x,$y]=C ([int](235*$shade)) ([int](190*$shade)) ([int](80*$shade)) } }
# colina escura na base
for($y=26;$y -lt $N;$y++){ for($x=0;$x -lt $N;$x++){ $h=26+[int](1.6*[math]::Sin($x/4.0)); if($y -ge $h){ $g[$x,$y]=C 8 6 4 } } }
# moldura dourada
for($i=0;$i -lt $N;$i++){ foreach($p in @(@($i,0),@($i,($N-1)),@(0,$i),@(($N-1),$i))){ $px=$p[0]; $py=$p[1]; $g[$px,$py]=C 214 168 58 } }
function Render($size){
  $bmp=New-Object System.Drawing.Bitmap $size,$size,([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  for($y=0;$y -lt $size;$y++){ for($x=0;$x -lt $size;$x++){ $sx=[int][math]::Floor($x*$N/$size); $sy=[int][math]::Floor($y*$N/$size); $col=$script:g[$sx,$sy]; $bmp.SetPixel($x,$y,$col) } }
  $ms=New-Object System.IO.MemoryStream; $bmp.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); ,$ms.ToArray()
}
$sizes=@(256,64,48,32,16); $imgs=@(); foreach($s in $sizes){ $imgs += ,(Render $s) }
$out=New-Object System.IO.MemoryStream; $bw=New-Object System.IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$off=6+16*$sizes.Count
for($i=0;$i -lt $sizes.Count;$i++){ $s=$sizes[$i]; $b=[byte]$(if($s -ge 256){0}else{$s})
  $bw.Write($b);$bw.Write($b);$bw.Write([byte]0);$bw.Write([byte]0);$bw.Write([uint16]1);$bw.Write([uint16]32)
  $bw.Write([uint32]$imgs[$i].Length);$bw.Write([uint32]$off);$off+=$imgs[$i].Length }
foreach($im in $imgs){ $bw.Write($im) }
[IO.File]::WriteAllBytes((Join-Path $root 'assets\icon.ico'),$out.ToArray())
# previa 256 para conferir
[IO.File]::WriteAllBytes((Join-Path $env:TEMP 'icon-preview.png'),$imgs[0])
Write-Host 'OK assets\icon.ico'
