// Offline maintenance tool; not part of the host build.
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require(process.argv[2] || 'sharp');
const root = path.resolve(__dirname, '../assets/menu/icons');
const sha256 = data => crypto.createHash('sha256').update(data).digest('hex');

(async () => {
    const manifest = JSON.parse(await fs.readFile(path.join(root, 'sources.json'), 'utf8'));
    for (const icon of manifest.icons) {
        const svg = await fs.readFile(path.join(root, `${icon.file}.svg`));
        const png = await sharp(svg, { density: 768 })
            .resize(manifest.rasterSize, manifest.rasterSize).png().toBuffer();
        await fs.writeFile(path.join(root, `${icon.file}.png`), png);
        icon.svgSha256 = sha256(svg);
        icon.pngSha256 = sha256(png);
    }
    await fs.writeFile(path.join(root, 'sources.json'), JSON.stringify(manifest, null, 2) + '\n');
    console.log(`Rendered ${manifest.icons.length} context-menu icons.`);
})().catch(error => { console.error(error); process.exitCode = 1; });
