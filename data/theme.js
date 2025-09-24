/* [TRAKKR] Theme helper with persistence + events
   - Persists to localStorage("trakkr-theme")
   - Applies on load
   - Exposes window.__trakkrTheme.get/set/toggle and a 'trakkr-theme-changed' event
*/
(() => {
  const KEY = 'trakkr-theme';
  const root = document.documentElement;
  const subs = new Set();

  function currentAttr() {
    return root.getAttribute('data-theme') || '';
  }

  function readStoredOrSystem() {
    try {
      const saved = localStorage.getItem(KEY);
      if (saved === 'light' || saved === 'dark') return saved;
    } catch {}
    // fallback to system preference if nothing stored
    try {
      return (window.matchMedia && matchMedia('(prefers-color-scheme: light)').matches) ? 'light' : 'dark';
    } catch {}
    return currentAttr() || 'dark';
  }

  function get() {
    try {
      const saved = localStorage.getItem(KEY);
      return (saved === 'light' || saved === 'dark') ? saved : (currentAttr() || 'dark');
    } catch {
      return currentAttr() || 'dark';
    }
  }

  function set(mode) {
    const m = (mode === 'light') ? 'light' : 'dark';
    root.setAttribute('data-theme', m);
    try { localStorage.setItem(KEY, m); } catch {}
    // notify listeners and your page JS
    const ev = new Event('trakkr-theme-changed');
    window.dispatchEvent(ev);
    subs.forEach(fn => { try { fn(m); } catch {} });
    return m;
  }

  function toggle() {
    return set(get() === 'light' ? 'dark' : 'light');
  }

  // Apply immediately on load (in case the inline boot didn’t run)
  set(readStoredOrSystem());

  // Cross-tab sync
  window.addEventListener('storage', (e) => {
    if (e.key === KEY && (e.newValue === 'light' || e.newValue === 'dark')) {
      root.setAttribute('data-theme', e.newValue);
      const ev = new Event('trakkr-theme-changed');
      window.dispatchEvent(ev);
      subs.forEach(fn => { try { fn(e.newValue); } catch {} });
    }
  });

  // API
  window.__trakkrTheme = {
    get, set, toggle,
    subscribe(fn) { subs.add(fn); return () => subs.delete(fn); }
  };
})();
