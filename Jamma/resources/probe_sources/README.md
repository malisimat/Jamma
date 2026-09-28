# Mirror-ball probe sources

`DefMat_Sphere2b.bmp` is the supplied source art. From the repository root,
regenerate the two custom probes with ImageMagick:

```powershell
magick Jamma/resources/probe_sources/DefMat_Sphere2b.bmp -flip -alpha on -type TrueColorAlpha -depth 8 -compress none Jamma/resources/textures/probe_pearl.tga
magick Jamma/resources/probe_sources/DefMat_Sphere2b.bmp -fill "#e7a85e" -tint 35 -level 0%,80% -flip -alpha on -type TrueColorAlpha -depth 8 -compress none Jamma/resources/textures/probe_amber.tga
```

The pearl probe preserves the BMP's colour and reflections. The amber probe
keeps the same highlights but shifts them toward warm metal. `-flip` puts the
BMP's bottom row first in the TGA pixel data because `ImageUtils::LoadTga`
uploads rows directly to OpenGL without applying the TGA origin flag.
Both outputs are uncompressed, 32-bit true-colour TGAs for that loader.
`probe_chrome.tga` remains the original synthetic probe.
