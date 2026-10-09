// Keeps the games mapped to neutron, evaluated in Steam's SharedJSContext at every
// Steam start (dev/steam-hook.sh). __TOOL__ is the tool name, __MAPPED__ the app ids
// mapped to it in Steam's config.vdf (the cache Steam keeps itself), __NATIVE__ the app
// ids with a Mac version (steam-native.mjs).
//  1. Remap from the cache: after the platform switch Steam shows "invalid platform"
//     for mapped games until the mapping changes. Setting the same tool again does
//     nothing, so all games go to the second tool name (__TOOL___remap, same files)
//     and back. No per-game details.
//     The platform switch also drops the Windows depot of installed games and
//     marks them "Update Required" (Steam delays that update for games not played
//     recently). A user-started update after the remap is a 0-byte check ("has no
//     changes") that puts the depot back.
//     Installed neutron games are set to "only update when launched": at the platform
//     switch Steam starts the automatic update of recently played games while their
//     mapping is briefly invalid, finds no Mac depots, deletes the installed files
//     and downloads everything again. The 0-byte check above is a user-started
//     update, it still brings real game updates at every Steam start.
//  2. Native games stay native: a game with a Mac version that is mapped but not
//     installed loses the mapping (nothing to delete). An installed one keeps it
//     (clearing it would swap the Windows files for the Mac depots), it is only
//     counted in the result.
//  3. Map new games: every library game without a Mac version that neutron has not
//     seen yet gets the tool, at start and while Steam runs. Seen games (mapped, Mac
//     native, or set to none by the user) are kept in localStorage, so a game set
//     back to none stays none.
(async () => {
  const TOOL = '__TOOL__', KEY = 'neutron.seen';
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  // Never clear the tool on the way: without a tool Steam computes the Mac depots,
  // which a Windows-only game does not have, deletes the installed files and
  // downloads everything again when the tool is back.
  const mapped = [__MAPPED__];
  for (const id of mapped) SteamClient.Apps.SpecifyCompatTool(id, TOOL + '_remap');
  await sleep(500);
  for (const id of mapped) SteamClient.Apps.SpecifyCompatTool(id, TOOL);

  const games = () => (window.collectionStore?.allGamesCollection?.allApps || []).filter(a => a.app_type == 1);
  for (let i = 0; i < 120 && !games().length; i++) await sleep(500);
  await sleep(1000);
  const isMapped = new Set(mapped);
  const installed = games().filter(a => a.installed && isMapped.has(a.appid)).map(a => a.appid);
  for (const id of installed) {
    SteamClient.Downloads.QueueAppUpdate(id, 0);
    SteamClient.Downloads.ResumeAppUpdate(id);
  }
  const native = new Set([__NATIVE__]);
  const nativeKept = installed.filter(id => native.has(id)).length;
  let unmapped = 0;
  for (const id of mapped) {
    if (!native.has(id) || installed.includes(id)) continue;
    SteamClient.Apps.SpecifyCompatTool(id, '');
    unmapped++;
  }
  const nativeNote = `native unmapped ${unmapped}` + (nativeKept ? `, native installed kept ${nativeKept}` : '');

  const details = id => new Promise(resolve => {
    let done = false;
    const h = SteamClient.Apps.RegisterForAppDetails(id, d => { if (done) return; done = true; h.unregister(); resolve(d); });
    setTimeout(() => { if (!done) { done = true; h.unregister(); resolve(null); } }, 3000);
  });
  // eAutoUpdateValue 1: "only update this game when I launch it"
  const holdUpdates = async () => {
    let n = 0;
    for (const app of games()) {
      if (!app.installed) continue;
      const d = await details(app.appid);
      if (!d || !(d.strCompatToolName || '').startsWith(TOOL) || d.eAutoUpdateValue === 1) continue;
      SteamClient.Apps.SetAppAutoUpdateBehavior(app.appid, 1);
      n++;
    }
    return n;
  };
  const held = await holdUpdates();

  if (window.__neutronSync) return `remapped ${mapped.length}, checked ${installed.length} installed, updates held for ${held}, ${nativeNote}, watch already running`;

  let seen;
  try { seen = new Set(JSON.parse(localStorage.getItem(KEY) || '[]')); } catch { seen = new Set(); }
  for (const id of mapped) seen.add(id);
  const failed = new Map();   // app id -> details without platforms, retried 3 times per Steam run
  let busy = false;

  const check = async () => {
    if (busy) return 0;
    busy = true;
    let n = 0;
    try {
      for (const app of games()) {
        const id = app.appid;
        if (seen.has(id) || (failed.get(id) || 0) >= 3) continue;
        const d = await details(id);
        if (!d || !d.vecPlatforms || !d.vecPlatforms.length) { failed.set(id, (failed.get(id) || 0) + 1); continue; }
        // A mapped tool makes Steam list osx too, so check the tool first.
        if (!d.strCompatToolName && !d.vecPlatforms.includes('osx') && !native.has(id)) {
          SteamClient.Apps.SpecifyCompatTool(id, TOOL);
          n++;
        }
        seen.add(id);
      }
      localStorage.setItem(KEY, JSON.stringify([...seen]));
      await holdUpdates();
    } finally {
      busy = false;
    }
    return n;
  };

  const added = await check();
  window.__neutronSync = setInterval(check, 30000);
  return `remapped ${mapped.length}, checked ${installed.length} installed, updates held for ${held}, ${nativeNote}, mapped ${added} new, seen ${seen.size}`;
})()
