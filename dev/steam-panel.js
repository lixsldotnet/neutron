// neutron settings in Mac Steam, evaluated in Steam's SharedJSContext over the CEF
// debug port (injected by dev/steam-hook.sh at every Steam start and UI reload).
// Mac Steam hides the Compatibility page of a game's properties, so this adds its
// own "Compatibility" tab there: the compat tool for the game (Neutron or none)
// and the neutron settings. When Steam shows its own
// Compatibility page, the settings go into that page instead.
// The settings are stored as NEUTRON_<NAME>=<value> words in the game's launch
// options; tool/neutron takes them out of the command line and exports them.
// On and Off are written as values (=1, =0): no word means the value from
// neutron.env, which would win over an Off.
// Lives in the running Steam UI only, it changes no Steam file.
(() => {
  if (window.__neutronPanel) return 'already installed';

  const TAB = 'Compatibility';
  const OPTIONS = [
    { key: 'NEUTRON_HUD', label: 'Performance HUD',
      choices: [['0', 'Off'], ['1', 'FPS and frame time'], ['2', 'Full (with DXMT statistics)']] },
    { key: 'NEUTRON_RENDER_SCALE', label: 'Render scale',
      choices: [['', 'Default (85%, MetalFX upscaling)'], ['1', '100% (native, no upscaling)'], ['0.75', '75%']] },
    { key: 'NEUTRON_FEX_TSO', label: 'x86 memory ordering',
      choices: [['', 'Default for this game'], ['fast', 'Fast'], ['strict', 'Strict (slower, for threading bugs)']] },
    { key: 'NEUTRON_NATIVE_FULLSCREEN', label: 'macOS fullscreen and Game Mode',
      choices: [['1', 'On'], ['0', 'Off']] },
    { key: 'NEUTRON_DLSS', label: 'DLSS on MetalFX (game sees an NVIDIA GPU)',
      choices: [['0', 'Off'], ['1', 'On (experimental)']] },
    { key: 'NEUTRON_D3D12_SM6', label: 'DirectX 12 shader model 6 (Unreal 5)',
      choices: [['0', 'Off'], ['1', 'On (experimental)']] },
    { key: 'NEUTRON_FPS_LOG', label: 'FPS log',
      choices: [['0', 'Off'], ['1', 'On (~/Library/Logs/neutron)']] },
    { key: 'NEUTRON_SANDBOX', label: 'Sandbox (game cannot read your files or start programs)',
      choices: [['0', 'Off'], ['1', 'On (experimental)']] },
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
  const isNeutron = name => /neutron/.test(name || '');

  // The app id of a properties dialog, from the React props of its content.
  const appidOf = doc => {
    for (const el of doc.querySelectorAll('.DialogContentTransition, .DialogContentTransition *')) {
      const fk = Object.keys(el).find(k => k.startsWith('__reactFiber'));
      for (let f = fk && el[fk], n = 0; f && n < 40; f = f.return, n++) {
        const p = f.memoizedProps;
        if (p && typeof p === 'object' && p.details && p.details.unAppID) return p.details.unAppID;
      }
    }
    return 0;
  };

  const select = (doc, choices, value, onChange) => {
    const s = doc.createElement('select');
    s.style.cssText = 'min-width:260px;padding:6px 8px;border:none;border-radius:2px;' +
      'background:#32353c;color:#dcdedf;font-size:13px;outline:none';
    for (const [v, text] of choices) {
      const o = doc.createElement('option');
      o.value = v;
      o.textContent = text;
      s.appendChild(o);
    }
    s.value = value;
    if (s.selectedIndex < 0) s.selectedIndex = 0;
    s.addEventListener('change', () => onChange(s.value));
    return s;
  };
  const row = (doc, label, control) => {
    const r = doc.createElement('label');
    r.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:16px;' +
      'padding:8px 0;color:#dcdedf;font-size:14px';
    const name = doc.createElement('span');
    name.textContent = label;
    r.append(name, control);
    return r;
  };
  const section = (doc, title) => {
    const s = doc.createElement('div');
    s.className = 'DialogControlsSection DialogLabelledControlsSection DialogSettingsSection';
    const head = doc.createElement('div');
    head.className = 'SettingsDialogSubHeader';
    head.textContent = title;
    s.appendChild(head);
    return s;
  };

  // The neutron settings as a settings section.
  function settings(doc, appid, d) {
    const s = section(doc, 'Neutron');
    s.dataset.neutron = String(appid);
    for (const opt of OPTIONS) {
      s.appendChild(row(doc, opt.label, select(doc, opt.choices, valueOf(d.strLaunchOptions, opt.key), async v => {
        const cur = await details(appid);
        SteamClient.Apps.SetAppLaunchOptions(appid, withValue(cur.strLaunchOptions, opt.key, v));
      })));
    }
    const note = doc.createElement('div');
    note.style.cssText = 'color:#8b929a;font-size:12px;padding-top:4px';
    note.textContent = 'Stored in the launch options (General page), applies on the next start.';
    s.appendChild(note);
    return s;
  }

  // Our Compatibility page: the compat tool for the game, then the settings.
  async function page(doc, appid) {
    const d = await details(appid);
    const tools = (await SteamClient.Apps.GetAvailableCompatTools(appid))
      .filter(t => isNeutron(t.strToolName) && !t.strToolName.endsWith('_remap'));
    if (isNeutron(d.strCompatToolName) && !tools.some(t => t.strToolName === d.strCompatToolName))
      tools.unshift({ strToolName: d.strCompatToolName, strDisplayName: 'Neutron' });

    const p = doc.createElement('div');
    p.className = 'DialogContent _DialogLayout';
    p.dataset.neutronPage = String(appid);
    const inner = doc.createElement('div');
    inner.className = 'DialogContent_InnerWidth';
    const header = doc.createElement('div');
    header.className = 'DialogHeader';
    header.textContent = TAB;
    const body = doc.createElement('div');
    body.className = 'DialogBody';

    const tool = section(doc, 'Steam Play');
    const choices = [['', 'None (Mac version)']].concat(tools.map(t => [t.strToolName, t.strDisplayName || t.strToolName]));
    tool.appendChild(row(doc, 'Run this game with', select(doc, choices, isNeutron(d.strCompatToolName) ? d.strCompatToolName : '',
      async v => {
        SteamClient.Apps.SpecifyCompatTool(appid, v);
        const fresh = await details(appid);
        const old = body.querySelector('[data-neutron]');
        if (old) old.remove();
        if (isNeutron(v)) body.appendChild(settings(doc, appid, fresh));
      })));
    body.appendChild(tool);
    if (isNeutron(d.strCompatToolName)) body.appendChild(settings(doc, appid, d));

    inner.append(header, body);
    p.appendChild(inner);
    return p;
  }

  // Only game properties dialogs: Steam's main window and menus change all the
  // time, watching them keeps Steam's UI thread busy (menus closed by themselves).
  function watch(popup, tries = 0) {
    const win = popup && popup.m_popup;
    if (!win || win.__neutronWatch || !win.document || !/^PopupWindow/.test(popup.m_strName || '')) return;
    if (!win.document.querySelector('.PageListColumn')) {
      if (tries < 20) setTimeout(() => watch(popup, tries + 1), 100);
      return;
    }
    win.__neutronWatch = true;
    const doc = win.document;
    let pending = false, ours = null, hidden = [], prevActive = [], activeClass = '';

    const leave = () => {
      if (!ours) return;
      ours.page.remove();
      for (const el of hidden) el.style.display = '';
      ours.nav.classList.remove(activeClass);
      for (const el of prevActive) el.classList.add(activeClass);
      hidden = []; prevActive = []; ours = null;
    };

    const check = async () => {
      pending = false;
      const content = doc.querySelector('.DialogContentTransition');
      const column = doc.querySelector('.PageListColumn');
      if (!content || !column) return;
      const items = [...column.querySelectorAll('div')].filter(e => e.childElementCount === 1 &&
        e.firstElementChild.childElementCount === 0 && e.parentElement.childElementCount > 3);
      if (!items.length) return;
      const appid = appidOf(doc);
      if (!appid) return;

      // Steam's own Compatibility page is there: put the settings into it.
      const steamTab = items.find(e => e.textContent.trim() === TAB && !e.dataset.neutronNav);
      if (steamTab) {
        const header = content.querySelector('.DialogHeader');
        const body = content.querySelector('.DialogBody');
        if (header && body && header.textContent.trim() === TAB && !body.querySelector('[data-neutron]')) {
          const d = await details(appid);
          if (isNeutron(d.strCompatToolName) && !body.querySelector('[data-neutron]'))
            body.appendChild(settings(doc, appid, d));
        }
        return;
      }
      if (column.querySelector('[data-neutron-nav]')) return;

      // Our own tab after "General", built from one of Steam's tab entries.
      const counts = {};
      for (const it of items) for (const c of it.classList) counts[c] = (counts[c] || 0) + 1;
      activeClass = Object.keys(counts).find(c => counts[c] === 1) || '';
      const nav = items[0].cloneNode(true);
      nav.dataset.neutronNav = '1';
      if (activeClass) nav.classList.remove(activeClass);
      nav.firstElementChild.textContent = TAB;
      items[0].after(nav);

      nav.addEventListener('click', async ev => {
        ev.stopPropagation();
        if (ours) return;
        const p = await page(doc, appid);
        const box = doc.querySelector('.DialogContentTransition');
        hidden = [...box.children];
        for (const el of hidden) el.style.display = 'none';
        box.appendChild(p);
        prevActive = items.filter(e => activeClass && e.classList.contains(activeClass));
        for (const el of prevActive) el.classList.remove(activeClass);
        if (activeClass) nav.classList.add(activeClass);
        ours = { nav, page: p };
      });
      // Any of Steam's tabs brings Steam's page back.
      for (const it of items) it.addEventListener('click', leave, true);
    };

    new win.MutationObserver(() => { if (!pending) { pending = true; win.setTimeout(check, 150); } })
      .observe(doc.body, { childList: true, subtree: true });
    check();
  }

  g_PopupManager.AddPopupCreatedCallback(popup => setTimeout(() => watch(popup), 0));
  for (const popup of g_PopupManager.m_mapPopups.values()) watch(popup);

  // In the library list, a small icon right of each game name: the neutron logo when
  // the game runs through neutron, the Apple logo for a native Mac game. The list is
  // virtualized (Steam reuses rows when scrolling), so a tick once a second checks the
  // visible rows; a MutationObserver on Steam's main window kept its UI thread busy
  // before. Kinds come from the app details, cached for 30 s.
  // The neutron app icon drawn for row height (16 px grid): its tile, the orbit with the
  // blue arc and electron, the white core. The Apple badge uses the same 16 px tile.
  // Colors as inline !important styles: Steam's library CSS paints every circle and
  // path in the list semi-transparent white, which beats presentation attributes.
  const paint = (fill, stroke) => `style="fill:${fill} !important;stroke:${stroke} !important"`;
  const MARK = '<svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style="display:block">' +
    `<rect width="16" height="16" rx="4" ${paint('#2a3752', 'none')}/>` +
    `<circle cx="8" cy="8" r="5.3" stroke-width="1.3" ${paint('none', '#6a7ca0')}/>` +
    `<path d="M8 2.7 A5.3 5.3 0 0 1 12.9 5.8" stroke-width="1.3" stroke-linecap="round" ${paint('none', '#9fb4ff')}/>` +
    `<circle cx="11.75" cy="4.25" r="1.45" ${paint('#9fb4ff', 'none')}/>` +
    `<circle cx="8" cy="8" r="3" ${paint('#f5f7fb', 'none')}/></svg>`;
  const kinds = new Map();   // appid -> { kind: 'neutron' | 'mac' | '', at }
  const pending = new Set();
  const kindOf = appid => {
    const hit = kinds.get(appid);
    if (hit && Date.now() - hit.at < 30000) return hit.kind;
    if (!pending.has(appid)) {
      pending.add(appid);
      details(appid).then(d => {
        const kind = isNeutron(d.strCompatToolName) ? 'neutron'
          : (d.vecPlatforms || []).includes('osx') && !d.strCompatToolName ? 'mac' : '';
        kinds.set(appid, { kind, at: Date.now() });
      }).finally(() => pending.delete(appid));
    }
    return hit ? hit.kind : null;
  };
  const rowAppid = row => {
    const fk = Object.keys(row).find(k => k.startsWith('__reactFiber'));
    for (let f = fk && row[fk], n = 0; f && n < 4; f = f.return, n++) {
      const p = f.memoizedProps;
      if (p && typeof p === 'object' && typeof p.appid === 'number') return p.appid;
    }
    return 0;
  };
  const listTick = () => {
    const main = [...g_PopupManager.m_mapPopups.values()].find(p => /^SP Desktop/.test(p.m_strName || ''));
    const doc = main && main.m_popup && main.m_popup.document;
    if (!doc) return;
    for (const grid of doc.querySelectorAll('.ReactVirtualized__Grid__innerScrollContainer')) {
      // redraw right on scroll (one tick per frame), the 1 s tick stays as a fallback
      const scroller = grid.parentElement;
      if (scroller && !scroller.__neutronScroll) {
        scroller.__neutronScroll = true;
        let queued = false;
        scroller.addEventListener('scroll', () => {
          if (queued) return;
          queued = true;
          main.m_popup.requestAnimationFrame(() => { queued = false; try { listTick(); } catch (e) { /* list changed */ } });
        }, { passive: true });
      }
      for (const panel of grid.children) {
        // a game row: Panel > div > div > row (icon, name, status); category rows have no appid
        const row = panel.firstElementChild && panel.firstElementChild.firstElementChild &&
          panel.firstElementChild.firstElementChild.firstElementChild;
        if (!row) continue;
        const appid = rowAppid(row);
        let icon = row.querySelector(':scope > [data-neutron-kind]');
        const kind = appid ? kindOf(appid) : '';
        if (kind === null) continue;   // details pending, next tick
        if (icon && (icon.dataset.neutronApp !== String(appid) || icon.dataset.neutronKind !== kind)) { icon.remove(); icon = null; }
        if (icon || !kind) continue;
        // a small tile, the same for both kinds: 16 px, the glyph centered at 12 px
        icon = doc.createElement('span');
        icon.dataset.neutronApp = String(appid);
        icon.dataset.neutronKind = kind;
        icon.style.cssText = 'margin-left:auto;margin-right:8px;flex:none;width:16px;height:16px;' +
          'display:flex;align-items:center;justify-content:center;border-radius:4px;' +
          'background:rgba(255,255,255,0.08);color:#b8c1cc';
        if (kind === 'neutron') {
          icon.innerHTML = MARK;
          icon.style.background = 'none';
          icon.title = 'Runs through neutron';
        } else {
          icon.textContent = '\uF8FF';   // Apple logo in the macOS system font
          icon.style.font = '12px/1 -apple-system, "SF Pro Text", sans-serif';
          icon.style.paddingBottom = '1px';
          icon.title = 'Native Mac game';
        }
        row.appendChild(icon);
      }
    }
  };
  // Kinds of the whole library in the background at start, so rows that scroll into
  // view have their icon right away.
  (async () => {
    const all = (window.collectionStore?.allGamesCollection?.allApps || []).map(a => a.appid);
    for (let i = 0; i < all.length; i += 8) {
      await Promise.all(all.slice(i, i + 8).map(id => {
        kindOf(id);
        return new Promise(r => setTimeout(r, 30));
      }));
    }
  })();
  window.__neutronList = setInterval(() => { try { listTick(); } catch (e) { /* list changed meanwhile */ } }, 1000);

  window.__neutronPanel = true;
  return 'installed';
})()
