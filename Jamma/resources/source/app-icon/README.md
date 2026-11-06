# Jamma Windows icon

Selected by the user on 2026-10-25: option C's amber colors and tilted loop,
with a **rounded rectangle** border. `Jamma.svg` is the editable source.

`generate.cjs` renders 16/24/32/48/64/128/256px alpha PNGs and packs them into
`../../icons/Jamma.ico`. The app resource script embeds that file; no external
icon file is needed at runtime. Window loads shared large and small icons from
the application instance, falling back to the standard Windows application icon
if the resource is unavailable. Separate large/small icon group IDs refer to the
same ICO so the Windows shared resource-name cache cannot reuse the large frame
for the small request. Windows owns these shared handles.

Regenerate with Node.js and sharp 0.35.5. To keep dependencies outside the repo,
install them into a temporary directory and point `NODE_PATH` there:

```powershell
$iconDependencies = Join-Path ([IO.Path]::GetTempPath()) 'jamma-icon-dependencies'
npm install --prefix $iconDependencies --no-save --package-lock=false sharp@0.35.5
$env:NODE_PATH = Join-Path $iconDependencies 'node_modules'
node Jamma/resources/source/app-icon/generate.cjs
```

An optional first argument selects another output path, allowing a byte-for-byte
comparison without overwriting the committed ICO. Rasterization can differ with
another libvips/librsvg platform build; retain the reviewed ICO unless that change
has been inspected at the small sizes on both light and dark backgrounds.
