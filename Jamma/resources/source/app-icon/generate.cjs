const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');

// The selected SVG is the source of truth. PNG-compressed ICO entries retain
// alpha at every native size; a 256px entry uses the ICO zero-size sentinel.
const sizes = [16, 24, 32, 48, 64, 128, 256];
const output = process.argv[2] || path.resolve(__dirname, '../../icons/Jamma.ico');

async function generate() {
  if (sharp.versions.sharp !== '0.35.5') {
    throw new Error(`Expected sharp 0.35.5; found ${sharp.versions.sharp}`);
  }
  const svg = fs.readFileSync(path.join(__dirname, 'Jamma.svg'));
  const pngs = [];
  for (const size of sizes) pngs.push(await sharp(svg).resize(size, size).png().toBuffer());
  const directory = Buffer.alloc(6 + sizes.length * 16);
  directory.writeUInt16LE(1, 2); // icon rather than cursor
  directory.writeUInt16LE(sizes.length, 4);
  let offset = directory.length;
  for (let i = 0; i < sizes.length; ++i) {
    const entry = 6 + i * 16;
    directory[entry] = sizes[i] === 256 ? 0 : sizes[i];
    directory[entry + 1] = directory[entry];
    directory.writeUInt16LE(1, entry + 4); // planes
    directory.writeUInt16LE(32, entry + 6); // RGBA
    directory.writeUInt32LE(pngs[i].length, entry + 8);
    directory.writeUInt32LE(offset, entry + 12);
    offset += pngs[i].length;
  }
  fs.writeFileSync(output, Buffer.concat([directory, ...pngs]));
  console.log(`Wrote ${output}: ${sizes.join('/')}px`);
}

generate().catch(error => { console.error(error); process.exitCode = 1; });
