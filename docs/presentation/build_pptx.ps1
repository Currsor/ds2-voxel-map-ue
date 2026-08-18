$ErrorActionPreference = 'Stop'
$root = 'F:/ds2-voxel-map-ue/docs/presentation'
$images = Join-Path $root 'final-slides'
$output = Join-Path $root 'ds2-voxel-map-complete.pptx'
$titles = @(
'Cover','Design Context','Requirements','Target Experience','Options','Height Map','Export Geometry','Bake Point Cloud','Capture Process','Hemisphere Pass','XYZ Slice Pass','GBuffer To Points','Point Grid','Rendering Chapter','Rendering Constraints','Drawing Evolution','Voxel Block','Block LOD','Data Hierarchy','Memory Model','Buffer Overview','Word0 Layout','Fine Occupancy','Render Start Index','Voxel Attribute','Sparse Addressing','CPU Builder','GPU Upload','Instanced Billboard','Ray Box Intersection','LOD And DDA','Hit And Attribute','Billboard Screen Mapping','Slab Intersection','Camera Inside Box','Miss And Discard','Detailed 3D DDA','Multi Axis Crossing','HitT And Reverse Z','Normal And Attribute','Complete Pipeline'
)
$powerPoint = New-Object -ComObject PowerPoint.Application
$powerPoint.Visible = -1
$presentation = $powerPoint.Presentations.Add()
$presentation.PageSetup.SlideSize = 15
for ($i = 1; $i -le 41; $i++) {
    $slide = $presentation.Slides.Add($i, 12)
    $path = Join-Path $images ('slide-{0:D2}.png' -f $i)
    $shape = $slide.Shapes.AddPicture($path, 0, -1, 0, 0, 720, 405)
    $shape.AlternativeText = ('{0:D2} / 41 - {1}' -f $i, $titles[$i - 1])
}
$presentation.SaveAs($output, 24)
$presentation.Close()
$powerPoint.Quit()
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($presentation) | Out-Null
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($powerPoint) | Out-Null
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
Write-Output ('PPTX_CREATED path={0}' -f $output)
