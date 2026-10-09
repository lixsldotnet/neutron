// Prints the app ids with a Mac version, comma separated, read from Steam's app info
// cache (appcache/appinfo.vdf, key appinfo/common/oslist). Steam's own app details
// are no help here: with a compat tool mapped they list osx for every game.
// Prints nothing for an unknown file version. Used by steam-hook.sh.
// Usage: node steam-native.mjs [appinfo.vdf]
import { readFileSync } from 'node:fs';
import { homedir } from 'node:os';

const file = process.argv[2] || `${homedir()}/Library/Application Support/Steam/appcache/appinfo.vdf`;
const b = readFileSync(file);
const version = b.readUInt32LE(0);
// 0x07564428: keys are inline strings. 0x07564429: keys index a string table at the end.
if (version !== 0x07564428 && version !== 0x07564429) process.exit(0);

let p, strs;
if (version === 0x07564429) {
  const off = Number(b.readBigInt64LE(8));
  strs = [];
  let q = off + 4;
  for (let i = b.readUInt32LE(off); i > 0; i--) {
    const e = b.indexOf(0, q);
    strs.push(b.toString('utf8', q, e));
    q = e + 1;
  }
  p = 16;
} else {
  p = 8;
}

const native = [];
for (;;) {
  const appid = b.readUInt32LE(p);
  if (!appid) break;
  const end = p + 8 + b.readUInt32LE(p + 4);
  // size, info state, last updated, PICS token, SHA-1, change number, binary SHA-1
  let q = p + 8 + 4 + 4 + 8 + 20 + 4 + 20;
  const path = [];
  while (q < end) {
    const type = b[q++];
    if (type === 8) { if (!path.pop()) break; continue; }
    let key;
    if (strs) { key = strs[b.readUInt32LE(q)]; q += 4; }
    else { const e = b.indexOf(0, q); key = b.toString('utf8', q, e); q = e + 1; }
    if (type === 0) { path.push(key); continue; }
    if (type === 1) {
      const e = b.indexOf(0, q);
      if (key === 'oslist' && path.join('/') === 'appinfo/common' && /\bmacos\b/.test(b.toString('utf8', q, e))) native.push(appid);
      q = e + 1;
    } else if (type === 2 || type === 3 || type === 4) q += 4;
    else if (type === 7 || type === 10) q += 8;
    else break;
  }
  p = end;
}
console.log(native.join(','));
