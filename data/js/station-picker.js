/* [TRAKKR] Station picker
   - Data URL: /data/stations.min.json
   - Input shows: "Name (CRS)"
   - Hidden #station_crs carries the CRS
   - Works with <input id="station" list="stations-list"> and <datalist id="stations-list">
*/
(function () {
  "use strict";

  const INPUT_ID     = "station";
  const DATALIST_ID  = "stations-list";
  const HIDDEN_ID    = "station_crs";
  const STATIONS_URL = "/data/stations.min.json"; // <- per your layout

  const $ = (id) => document.getElementById(id);
  const displayOf = (name, crs) => `${name} (${crs})`;

  // normaliser
  const norm = (r) => ({
    name: String(r.stationName ?? r.name ?? "").trim(),
    crs:  String(r.crsCode    ?? r.crs ?? r.code ?? "").trim().toUpperCase()
  });

  // indexes
  const byCrs = new Map();         // CRS -> Name
  const nameKeys = [];             // lowercased names for prefix match

  function buildIndexes(rows){
    byCrs.clear(); nameKeys.length = 0;
    for (const r of rows){
      const {name, crs} = norm(r);
      if (!name || !/^[A-Z]{3}$/.test(crs)) continue;
      if (!byCrs.has(crs)) byCrs.set(crs, name);
    }
    for (const [crs, name] of byCrs){
      nameKeys.push(name.toLowerCase());
    }
    nameKeys.sort((a,b)=> a.localeCompare(b, "en", {sensitivity:"base"}));
  }

  function populateDatalist(dl){
    dl.textContent = "";
    const frag = document.createDocumentFragment();
    // Two options per station so native datalist matches when typing by name or CRS
    for (const [crs, name] of byCrs){
      const a = document.createElement("option"); a.value = displayOf(name, crs); a.dataset.crs = crs; frag.appendChild(a);
      const b = document.createElement("option"); b.value = crs;                  b.dataset.crs = crs; frag.appendChild(b);
    }
    dl.appendChild(frag);
  }

  function resolveToCRS(input, hidden, dl){
    const raw = (input.value || "").trim();

    // exact option match
    const opt = Array.from(dl.options).find(o => o.value === raw);
    if (opt && opt.dataset.crs){
      const code = opt.dataset.crs;
      const name = byCrs.get(code);
      input.value  = displayOf(name, code);
      hidden.value = code;
      return;
    }

    // "Name (CRS)" typed
    const m = raw.match(/\(([A-Za-z]{3})\)\s*$/);
    if (m){
      const code = m[1].toUpperCase();
      const name = byCrs.get(code);
      if (name){
        input.value  = displayOf(name, code);
        hidden.value = code;
        return;
      }
    }

    // bare CRS typed
    if (/^[A-Za-z]{3}$/.test(raw)){
      const code = raw.toUpperCase();
      const name = byCrs.get(code);
      if (name){
        input.value  = displayOf(name, code);
        hidden.value = code;
        return;
      }
    }

    // prefix name match (helpful if user typed just the start)
    const lower = raw.toLowerCase();
    const key = nameKeys.find(k => k.startsWith(lower));
    if (key){
      // find its CRS
      const entry = [...byCrs.entries()].find(([, n]) => n.toLowerCase() === key);
      if (entry){
        const [code, name] = entry;
        input.value  = displayOf(name, code);
        hidden.value = code;
        return;
      }
    }

    // unknown / incomplete: don’t mutate input; clear hidden
    hidden.value = "";
  }

  async function loadStations(){
    const r = await fetch(`${STATIONS_URL}?v=${Date.now()}`, { cache:"no-store" });
    if (!r.ok) throw new Error(`HTTP ${r.status} loading ${STATIONS_URL}`);
    const data = await r.json();
    if (!Array.isArray(data) || !data.length) throw new Error("Empty/invalid JSON");
    return data;
  }

  async function init(){
    const input  = $(INPUT_ID);
    const dl     = $(DATALIST_ID);
    const hidden = $(HIDDEN_ID);
    if (!input || !dl || !hidden) return;

    try{
      const rows = await loadStations();
      buildIndexes(rows);
      populateDatalist(dl);

      // update hint
      const hint = input.parentElement && input.parentElement.querySelector(".hint");
      if (hint) hint.textContent = `Stations loaded: ~${byCrs.size}. Start typing a station name or CRS.`;

      // normalise to "Name (CRS)" + write hidden CRS when user commits
      input.addEventListener("change", () => resolveToCRS(input, hidden, dl));
      input.addEventListener("blur",   () => resolveToCRS(input, hidden, dl));

      console.info("[TRAKKR] stations.min.json loaded OK");
    } catch (e) {
      console.error("[TRAKKR] Failed to load /data/stations.min.json:", e);
      const hint = input.parentElement && input.parentElement.querySelector(".hint");
      if (hint) hint.textContent = "⚠ Cannot load station list. Ensure /data/stations.min.json is present.";
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init, { once:true });
  } else {
    init();
  }
})();
