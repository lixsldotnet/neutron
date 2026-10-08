// Maps every game in the library that has no Mac version to the tool. Used by
// dev/steam.sh --map-all, __TOOL__ is the tool name.
(async () => {
  const details = id => new Promise(resolve => {
    let done = false;
    const h = SteamClient.Apps.RegisterForAppDetails(id, d => { if (done) return; done = true; h.unregister(); resolve(d); });
    setTimeout(() => { if (!done) { done = true; h.unregister(); resolve(null); } }, 3000);
  });
  const games = () => (window.collectionStore?.allGamesCollection?.allApps || []).filter(a => a.app_type == 1);
  for (let i = 0; i < 120 && !games().length; i++) await new Promise(r => setTimeout(r, 500));
  let mapped = 0, mac = 0, unknown = 0;
  for (const app of games()) {
    const d = await details(app.appid);
    if (!d || !d.vecPlatforms || !d.vecPlatforms.length) { unknown++; continue; }
    if (d.strCompatToolName === '__TOOL__') continue;
    // A mapped compat tool makes Steam list osx too: games mapped to another
    // (older) tool name are Windows games, remap them.
    if (d.vecPlatforms.includes('osx') && !d.strCompatToolName) { mac++; continue; }
    SteamClient.Apps.SpecifyCompatTool(app.appid, '__TOOL__'); mapped++;
  }
  return 'mapped ' + mapped + ', Mac native ' + mac + ', unknown ' + unknown;
})()
