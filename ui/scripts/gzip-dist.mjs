// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
// dist/ → dist-gz/: every file gzipped (level 9) as <name>.gz, nothing else.
// The firmware serves <path>.gz with Content-Encoding: gzip; the LittleFS
// image is built from dist-gz/ (see the root CMakeLists.txt).
import { promises as fs } from "node:fs";
import path from "node:path";
import { gzipSync } from "node:zlib";

const src = new URL("../dist/", import.meta.url).pathname;
const dst = new URL("../dist-gz/", import.meta.url).pathname;

async function walk(dir) {
  const out = [];
  for (const e of await fs.readdir(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) out.push(...(await walk(p)));
    else out.push(p);
  }
  return out;
}

await fs.rm(dst, { recursive: true, force: true });
let plain = 0, packed = 0;
for (const file of await walk(src)) {
  const rel = path.relative(src, file);
  const data = await fs.readFile(file);
  const gz = gzipSync(data, { level: 9 });
  const target = path.join(dst, rel + ".gz");
  await fs.mkdir(path.dirname(target), { recursive: true });
  await fs.writeFile(target, gz);
  plain += data.length; packed += gz.length;
  console.log(`${rel}  ${data.length} → ${gz.length}`);
}
console.log(`total ${plain} → ${packed} bytes gzipped`);
