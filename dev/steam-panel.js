// neutron settings panel for Mac Steam, evaluated in Steam's SharedJSContext over
// the CEF debug port (dev/steam.sh). Adds a "Neutron" section below "Launch Options"
// in a game's properties (General page) for games that run with a neutron tool.
// The settings are stored as NEUTRON_<NAME>=<value> words in the game's launch
// options; tool/neutron takes them out of the command line and exports them.
// Lives in the running Steam UI only, no Steam file is changed.
(() => {
  if (window.__neutronPanel) return 'already installed';

  const OPTIONS = [
    { key: 'NEUTRON_HUD', label: 'Performance HUD',
      choices: [['', 'Off'], ['1', 'FPS and frame time'], ['2', 'Full (with DXMT statistics)']] },
    { key: 'NEUTRON_RENDER_SCALE', label: 'Render scale',
      choices: [['', 'Default (85%, MetalFX upscaling)'], ['1', '100% (native, no upscaling)'], ['0.75', '75%']] },
    { key: 'NEUTRON_FEX_TSO', label: 'x86 memory ordering',
      choices: [['', 'Default for this game'], ['fast', 'Fast'], ['strict', 'Strict (slower, for threading bugs)']] },
    { key: 'NEUTRON_NATIVE_FULLSCREEN', label: 'macOS fullscreen and Game Mode',
      choices: [['', 'On'], ['0', 'Off']] },
    { key: 'NEUTRON_FPS_LOG', label: 'FPS log',
      choices: [['', 'Off'], ['1', 'On (~/Library/Logs/neutron)']] },
  ];

  const details = appid => new Promise(resolve => {
    const h = SteamClient.Apps.RegisterForAppDetails(appid, d => { h.unregister(); resolve(d); });
  });
  const words = s => (s || '').split(' ').filter(Boolean);
  const valueOf = (launch, key) => {
    const w = words(launch).find(x => x.startsWith(key + '='));
    return w ? w.slice(key.length + 1) : '';
  };
  const withValue = (launch, key, value) => {
    const rest = words(launch).filter(x => !x.startsWith(key + '='));
    if (value) rest.unshift(`${key}=${value}`);
    return rest.join(' ');
  };

  // The app id of a properties dialog, from the React props of its content.
  const appidOf = doc => {
    for (const el of doc.querySelectorAll('.DialogBody, .DialogBody *')) {
      const fk = Object.keys(el).find(k => k.startsWith('__reactFiber'));
      for (let f = fk && el[fk], n = 0; f && n < 40; f = f.return, n++) {
        const p = f.memoizedProps;
        if (p && typeof p === 'object' && p.details && p.details.unAppID) return p.details.unAppID;
      }
    }
    return 0;
  };

  async function build(doc, section, appid) {
    const d = await details(appid);
    if (!/neutron/.test(d.strCompatToolName || '')) return;
    if (section.parentElement.querySelector('[data-neutron]')) return;

    const panel = doc.createElement('div');
    panel.dataset.neutron = String(appid);
    panel.className = 'DialogControlsSection DialogLabelledControlsSection DialogSettingsSection';
    const head = doc.createElement('div');
    head.className = 'SettingsDialogSubHeader';
    head.textContent = 'Neutron';
    panel.appendChild(head);

    for (const opt of OPTIONS) {
      const row = doc.createElement('label');
      row.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:16px;' +
        'padding:8px 0;color:#dcdedf;font-size:14px';
      const name = doc.createElement('span');
      name.textContent = opt.label;
      const select = doc.createElement('select');
      select.style.cssText = 'min-width:260px;padding:6px 8px;border:none;border-radius:2px;' +
        'background:#32353c;color:#dcdedf;font-size:13px;outline:none';
      for (const [value, text] of opt.choices) {
        const o = doc.createElement('option');
        o.value = value;
        o.textContent = text;
        select.appendChild(o);
      }
      select.value = valueOf(d.strLaunchOptions, opt.key);
      select.addEventListener('change', async () => {
        const cur = await details(appid);
        SteamClient.Apps.SetAppLaunchOptions(appid, withValue(cur.strLaunchOptions, opt.key, select.value));
      });
      row.append(name, select);
      panel.appendChild(row);
    }
    const note = doc.createElement('div');
    note.style.cssText = 'color:#8b929a;font-size:12px;padding-top:4px';
    note.textContent = 'Stored in the launch options above, applies on the next start.';
    panel.appendChild(note);
    section.after(panel);
  }

  function watch(popup) {
    const win = popup && popup.m_popup;
    if (!win || win.__neutronWatch || !win.document) return;
    win.__neutronWatch = true;
    let pending = false;
    const check = () => {
      pending = false;
      const doc = win.document;
      // The General page: the settings section that holds the launch options text field.
      const input = doc.querySelector('.DialogSettingsSection input.DialogTextInputBase');
      const section = input && input.closest('.DialogSettingsSection');
      if (!section || section.parentElement.querySelector('[data-neutron]')) return;
      const appid = appidOf(doc);
      if (appid) build(doc, section, appid);
    };
    new win.MutationObserver(() => { if (!pending) { pending = true; win.setTimeout(check, 50); } })
      .observe(win.document.body, { childList: true, subtree: true });
    check();
  }

  g_PopupManager.AddPopupCreatedCallback(popup => setTimeout(() => watch(popup), 0));
  for (const popup of g_PopupManager.m_mapPopups.values()) watch(popup);
  window.__neutronPanel = true;
  return 'installed';
})()
