// After the platform switch Steam keeps "invalid platform" (display status 14) for
// games mapped to the tool until the mapping is set again. Used by dev/steam.sh,
// __TOOL__ is the tool name.
(async () => {
  const games = () => (window.collectionStore?.allGamesCollection?.allApps || []).filter(a => a.app_type == 1);
  for (let i = 0; i < 120 && !games().length; i++) await new Promise(r => setTimeout(r, 500));
  const details = id => new Promise(resolve => {
    let done = false;
    const h = SteamClient.Apps.RegisterForAppDetails(id, d => { if (done) return; done = true; h.unregister(); resolve(d); });
    setTimeout(() => { if (!done) { done = true; h.unregister(); resolve({}); } }, 3000);
  });
  let n = 0;
  for (const app of games()) {
    const d = await details(app.appid);
    if (d.strCompatToolName !== '__TOOL__' || d.eDisplayStatus !== 14) continue;
    SteamClient.Apps.SpecifyCompatTool(app.appid, '');
    await new Promise(r => setTimeout(r, 500));
    SteamClient.Apps.SpecifyCompatTool(app.appid, '__TOOL__');
    await new Promise(r => setTimeout(r, 100));
    n++;
  }
  return n;
})()
