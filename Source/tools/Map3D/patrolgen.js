// Boat patrols from a water mask (the tester 2026-11-15: "any way to paint water on maps? ... generated patrol routes" / "analyze the
// maps and make some patrol routes"). One copy of the logic for the 3D map page (<script src="/patrolgen.js">, PatrolGen.*) and
// for make-boat-patrols.js (require). No DOM, no three.js.
//
// On a 4 m grid over the play area: how far each water cell is from land (chamfer distance), then a loop through open water past
// the flags - for each flag the nearest water at least `clear` m from land, put in a round trip order, joined by the cheapest way
// over the water that keeps to the middle (A*), cut down to straight legs of up to 60 m that come no nearer the shore than the
// path did. Open water with fewer than 2 flags by it gets a loop round 5 spread-out points of its middle. Then a route from each
// boat's dock out to the nearest loop. Where boats are known, only the water they start on is used: the scan takes the sea
// level under hillsides for water, and that never joins the water the boats float on.
(function (root) {
    'use strict';
    // Scan water: auto[c] 1 = open water, 2 = water under a floor more than 3 m up (server.js waterData). Under a floor is the
    // sea level under a hillside (the scan's rays go on through terrain) - not water - except where it joins open water at the
    // same height within 8 m (under a bridge, a pier). Changes auto in place to 0 / 1.
    function filterCovered(auto, autoH, w, h, cell) {
        let front = []; const step = new Uint8Array(w * h); for (let c = 0; c < w * h; c++) if (auto[c] === 1) front.push(c);
        for (let d = 1; d <= Math.round(8 / cell) && front.length; d++) {
            const next = [];
            for (const c of front) { const cz = Math.floor(c / w), cx = c - cz * w; for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) { const x = cx + dx, z = cz + dz; if (x < 0 || z < 0 || x >= w || z >= h) continue; const q = z * w + x; if (auto[q] === 2 && !step[q] && Math.abs(autoH[q] - autoH[c]) < 2) { step[q] = d; next.push(q); } } }
            front = next;
        }
        for (let c = 0; c < w * h; c++) if (auto[c] === 2) auto[c] = step[c] ? 1 : 0; else if (auto[c] === 3) auto[c] = 0;   // (3: ground just above the water - a beach, an island)
    }
    const isBoatName = n => /boat|jetski|cat_|hovercraft|\.acv|rhib|pwc/i.test(n) && !/heli|\.air|jet\b/i.test(n);
    const shortName = n => n.replace(/^vehicles\./i, '');
    const plen = pts => { let s = 0; for (let i = 1; i < pts.length; i++) s += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][2] - pts[i - 1][2]); return s; };
    function heapPush(h, k, f) { h.push([f, k]); let i = h.length - 1; while (i > 0) { const p = (i - 1) >> 1; if (h[p][0] <= h[i][0]) break; [h[p], h[i]] = [h[i], h[p]]; i = p; } }
    function heapPop(h) { const top = h[0], last = h.pop(); if (h.length) { h[0] = last; let i = 0; for (;;) { const l = 2 * i + 1, r = l + 1; let m = i; if (l < h.length && h[l][0] < h[m][0]) m = l; if (r < h.length && h[r][0] < h[m][0]) m = r; if (m === i) break; [h[m], h[i]] = [h[i], h[m]]; i = m; } } return top; }

    // W: { w, h, x0, z0, cell, isWater(c), height(c) } per scan cell (c = cz * w + cx).
    // o: { play: {x0 x1 z0 z1} or null, flags: [{x, z}], vehicles: [{name, p:[x,y,z]}], clear: 14, loop: true, launch: true,
    //      maxPts: 256, exclude: [{x0 x1 z0 z1}] boxes that are not water }
    // Returns { out: [{ type: 'boat', pts, note }], notes: [text], loops, docks, flagsCovered: ['A', ...], dry: ['C', ...] }
    function generate(W, o) {
        const out = [], notes = [], C = Math.max(3, +o.clear || 14), maxPts = o.maxPts || 256;
        const res = (extra) => ({ out, notes, loops: 0, docks: 0, flagsCovered: [], dry: [], ...extra });
        const f = Math.max(1, Math.round(4 / W.cell)), gc = W.cell * f;
        const P = o.play || { x0: W.x0, x1: W.x0 + W.w * W.cell, z0: W.z0, z1: W.z0 + W.h * W.cell };
        const sx = Math.max(0, Math.floor((P.x0 - W.x0) / W.cell)), sz = Math.max(0, Math.floor((P.z0 - W.z0) / W.cell));
        const gw = Math.ceil((Math.min(W.w, Math.ceil((P.x1 - W.x0) / W.cell)) - sx) / f), gh = Math.ceil((Math.min(W.h, Math.ceil((P.z1 - W.z0) / W.cell)) - sz) / f), n = gw * gh;
        if (gw < 3 || gh < 3) { notes.push('The play area is too small.'); return res(); }
        const X = k => W.x0 + (sx + (k % gw) * f + (f - 1) / 2) * W.cell, Z = k => W.z0 + (sz + Math.floor(k / gw) * f + (f - 1) / 2) * W.cell;
        const excluded = (x, z) => (o.exclude || []).some(b => x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1);
        const wet = new Uint8Array(n), gy = new Float32Array(n);
        for (let j = 0; j < gh; j++) for (let i = 0; i < gw; i++) {
            let cnt = 0, hs = 0, tot = 0;
            for (let b = 0; b < f; b++) for (let a = 0; a < f; a++) { const cx = sx + i * f + a, cz = sz + j * f + b; if (cx >= W.w || cz >= W.h) continue; tot++; const c = cz * W.w + cx; if (W.isWater(c)) { cnt++; hs += W.height(c); } }
            const k = j * gw + i; if (tot && cnt === tot && !excluded(X(k), Z(k))) { wet[k] = 1; gy[k] = hs / cnt; }
        }
        const PT = k => [Math.round(X(k) * 10) / 10, Math.round(gy[k] * 10) / 10, Math.round(Z(k) * 10) / 10];
        // distance to land (the play area's edge counts as land: patrols stay inside)
        const clr = new Float32Array(n), D2 = gc * Math.SQRT2;
        for (let k = 0; k < n; k++) clr[k] = wet[k] ? 1e9 : 0;
        const at = (i, j) => i < 0 || j < 0 || i >= gw || j >= gh ? 0 : clr[j * gw + i];
        for (let j = 0; j < gh; j++) for (let i = 0; i < gw; i++) { const k = j * gw + i; if (!wet[k]) continue; clr[k] = Math.min(clr[k], at(i - 1, j) + gc, at(i, j - 1) + gc, at(i - 1, j - 1) + D2, at(i + 1, j - 1) + D2); }
        for (let j = gh - 1; j >= 0; j--) for (let i = gw - 1; i >= 0; i--) { const k = j * gw + i; if (!wet[k]) continue; clr[k] = Math.min(clr[k], at(i + 1, j) + gc, at(i, j + 1) + gc, at(i + 1, j + 1) + D2, at(i - 1, j + 1) + D2); }
        const minClr = Math.max(4, Math.min(8, C * 0.5));   // (a boat needs this much water each side to get through)
        // water bodies a boat can sail round (8-connected, at minClr, one water height)
        const comp = new Int32Array(n).fill(-1), compSize = [];
        for (let s = 0; s < n; s++) {
            if (comp[s] >= 0 || clr[s] < minClr) continue; const id = compSize.length, st = [s]; comp[s] = id; let size = 0;
            while (st.length) { const k = st.pop(); size++; const i = k % gw, j = (k - i) / gw; for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) { const a = i + di, b = j + dj; if (a < 0 || b < 0 || a >= gw || b >= gh) continue; const q = b * gw + a; if (comp[q] < 0 && clr[q] >= minClr && Math.abs(gy[q] - gy[k]) < 1.5) { comp[q] = id; st.push(q); } } }
            compSize.push(size);
        }
        if (!compSize.length) { notes.push('No water wide enough for a boat in the play area.'); return res(); }
        const cellAt = (x, z) => { const i = Math.floor(((x - W.x0) / W.cell - sx) / f), j = Math.floor(((z - W.z0) / W.cell - sz) / f); return i < 0 || j < 0 || i >= gw || j >= gh ? -1 : j * gw + i; };
        const nearestCell = (x, z, maxD, c, cps = null) => {   // nearest cell within maxD with clearance >= c (in one of the components cps, when given)
            let best = -1, bd = maxD; const R = Math.ceil(maxD / gc), k0 = cellAt(Math.max(P.x0, Math.min(P.x1, x)), Math.max(P.z0, Math.min(P.z1, z)));
            if (k0 < 0) return -1; const i0 = k0 % gw, j0 = (k0 - i0) / gw;
            for (let j = Math.max(0, j0 - R); j <= Math.min(gh - 1, j0 + R); j++) for (let i = Math.max(0, i0 - R); i <= Math.min(gw - 1, i0 + R); i++) {
                const k = j * gw + i; if (clr[k] < c || comp[k] < 0 || (cps && !cps.has(comp[k]))) continue; const d = Math.hypot(X(k) - x, Z(k) - z); if (d < bd) { bd = d; best = k; }
            }
            return best;
        };
        const astar = (s, g, need) => {   // cells with clearance >= need; cost grows near the shore
            const cost = new Float32Array(n).fill(Infinity), from = new Int32Array(n).fill(-1), done = new Uint8Array(n), h = [], gx = X(g), gz = Z(g);
            cost[s] = 0; heapPush(h, s, 0); let seen = 0;
            while (h.length) {
                const [, k] = heapPop(h); if (done[k]) continue; done[k] = 1; if (k === g) break; if (++seen > 600000) return null;
                const i = k % gw, j = (k - i) / gw;
                for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) {
                    if (!di && !dj) continue; const a = i + di, b = j + dj; if (a < 0 || b < 0 || a >= gw || b >= gh) continue; const q = b * gw + a;
                    if (done[q] || clr[q] < need || Math.abs(gy[q] - gy[k]) > 1.5) continue;   // (no hopping between water at different heights)
                    const nc = cost[k] + (di && dj ? D2 : gc) * (1 + 3 * Math.max(0, (C - clr[q]) / C));
                    if (nc < cost[q]) { cost[q] = nc; from[q] = k; heapPush(h, q, nc + Math.hypot(X(q) - gx, Z(q) - gz)); }
                }
            }
            if (!done[g]) return null; const path = []; for (let k = g; k >= 0; k = from[k]) path.push(k); return path.reverse();
        };
        // straight legs of up to 60 m that come no nearer the shore than the path did there (capped at the clearance asked for)
        const simplify = path => {
            const r = [path[0]]; let a = 0;
            while (a < path.length - 1) {
                let best = a + 1, low = clr[path[a + 1]];
                for (let b = a + 2; b < path.length; b++) {
                    low = Math.min(low, clr[path[b - 1]]); const need = Math.min(C * 0.8, low, clr[path[b]]);
                    const ax = X(path[a]), az = Z(path[a]), bx = X(path[b]), bz = Z(path[b]), L = Math.hypot(bx - ax, bz - az); if (L > 60) break;
                    let ok = true; for (let t = 0; t <= L; t += gc / 2) { const k = cellAt(ax + (bx - ax) * t / L, az + (bz - az) * t / L); if (k < 0 || clr[k] < need || Math.abs(gy[k] - gy[path[a]]) > 1.5) { ok = false; break; } }
                    if (ok) best = b;
                }
                r.push(path[best]); a = best;
            }
            return r;
        };
        const legPath = (s, g) => astar(s, g, minClr);
        const cap = pts => { while (pts.length > maxPts) pts = pts.filter((p, q) => q === 0 || q === pts.length - 1 || q % 2); return pts; };
        // the water the boats start on (boats 40 m apart)
        const boats = []; (o.vehicles || []).filter(v => isBoatName(v.name)).forEach(v => { if (!boats.some(b => Math.hypot(b.p[0] - v.p[0], b.p[2] - v.p[2]) < 40)) boats.push(v); });
        const dockComps = new Set(boats.map(v => nearestCell(v.p[0], v.p[2], 80, minClr)).filter(k => k >= 0).map(k => comp[k]));
        const useComps = dockComps.size ? dockComps : null;
        // stations by the flags
        const flagName = q => String.fromCharCode(65 + q), stations = [], dry = [];
        (o.flags || []).forEach((fl, q) => { let k = nearestCell(fl.x, fl.z, 300, C, useComps); if (k < 0) k = nearestCell(fl.x, fl.z, 300, minClr, useComps); if (k < 0) dry.push(flagName(q)); else stations.push({ k, comp: comp[k], name: flagName(q) }); });
        const loops = [];
        const byComp = new Map(); stations.forEach(s => { if (!byComp.has(s.comp)) byComp.set(s.comp, []); byComp.get(s.comp).push(s); });
        const order = st => {   // a round trip: nearest neighbour, then 2-opt (straight distances)
            if (st.length <= 3) return st; const d = (a, b) => Math.hypot(X(a.k) - X(b.k), Z(a.k) - Z(b.k));
            let t = [st[0]], left = st.slice(1); while (left.length) { const last = t[t.length - 1]; let bi = 0; left.forEach((s, q) => { if (d(last, s) < d(last, left[bi])) bi = q; }); t.push(left.splice(bi, 1)[0]); }
            for (let again = true, it = 0; again && it < 50; it++) { again = false; for (let a = 0; a < t.length - 1; a++) for (let b = a + 2; b < t.length; b++) { const A = t[a], B = t[a + 1], Cc = t[b], Dd = t[(b + 1) % t.length]; if (d(A, Cc) + d(B, Dd) < d(A, B) + d(Cc, Dd) - 0.1) { t = [...t.slice(0, a + 1), ...t.slice(a + 1, b + 1).reverse(), ...t.slice(b + 1)]; again = true; } } }
            return t;
        };
        const covered = new Set();
        const loopThrough = (st, label) => {   // legs between the stations in order; legs that fail split the loop into open routes
            const t = order(st), chains = []; let cur = null;
            for (let q = 0; q < t.length; q++) {
                const a = t[q], b = t[(q + 1) % t.length]; if (t.length === 2 && q === 1) break;
                const p = legPath(a.k, b.k);
                if (!p) { if (cur) chains.push(cur); cur = null; notes.push(`no way by boat ${a.name} → ${b.name}`); continue; }
                const s = simplify(p); if (!cur) cur = s; else cur.push(...s.slice(1));
                if (/^[A-Z]$/.test(a.name)) covered.add(a.name); if (/^[A-Z]$/.test(b.name)) covered.add(b.name);
            }
            if (cur) chains.push(cur);
            chains.forEach(ch => {
                let pts = ch.map(PT);
                if (t.length === 2 && chains.length === 1) pts = [...pts, ...pts.slice(0, -1).reverse()];   // (two stations: there and back)
                else if (chains.length === 1 && ch.length > 2 && ch[ch.length - 1] !== ch[0]) pts.push(pts[0]);
                if (chains.length === 1 && pts.length > 2) pts[pts.length - 1] = pts[0].slice();
                pts = cap(pts); if (pts.length >= 2) loops.push({ pts, comp: comp[ch[0]], label });
            });
        };
        if (o.loop !== false) {
            // (a pond, canal or fountain in a town is no place for a patrol: water of 15 hectares at least, unless boats start on it)
            const minArea = o.minArea ?? 150000;
            byComp.forEach((st, cp) => { if (st.length >= 2 && (compSize[cp] * gc * gc >= minArea || dockComps.has(cp))) loopThrough(st, 'flags ' + st.map(s => s.name).join(' ')); else if (st.length >= 2) notes.push(`flags ${st.map(s => s.name).join(' ')}: their water is only ${(compSize[cp] * gc * gc / 10000).toFixed(1)} ha - no patrol`); });
            // open water with fewer than 2 flags by it (and at least 6 hectares): a loop round 5 spread-out points of its middle -
            // only where no flag loop was made at all, or a boat starts on that water
            const anyLoop = loops.length > 0;
            const big = compSize.map((s, cp) => ({ cp, area: s * gc * gc })).filter(q => q.area >= 60000 && (byComp.get(q.cp) || []).length < 2 && (useComps ? dockComps.has(q.cp) && !loops.some(l => l.comp === q.cp) : !anyLoop)).sort((a, b) => b.area - a.area).slice(0, 3);
            for (const { cp } of big) {
                const cand = []; for (let k = 0; k < n; k++) if (comp[k] === cp && clr[k] >= C) cand.push(k); if (cand.length < 10) continue;
                const st = (byComp.get(cp) || []).slice(); if (!st.length) { let m = cand[0]; cand.forEach(k => { if (clr[k] > clr[m]) m = k; }); st.push({ k: m, comp: cp, name: 'middle' }); }
                while (st.length < 5) { let bk = -1, bd = -1; for (const k of cand) { let dm = Infinity; for (const s of st) dm = Math.min(dm, Math.hypot(X(k) - X(s.k), Z(k) - Z(s.k))); if (dm > bd) { bd = dm; bk = k; } } if (bk < 0 || bd < 40) break; st.push({ k: bk, comp: cp, name: 'point ' + st.length }); }
                if (st.length >= 3) loopThrough(st, 'open water');
            }
        }
        loops.forEach(l => out.push({ type: 'boat', pts: l.pts, note: l.label }));
        // from each boat's dock to the nearest loop waypoint it can reach
        let docks = 0;
        if (o.launch !== false) for (const v of boats) {
            const s = nearestCell(v.p[0], v.p[2], 80, minClr); if (s < 0) { notes.push(`${shortName(v.name)} at ${v.p[0].toFixed(0)}, ${v.p[2].toFixed(0)}: no open water within 80 m`); continue; }
            const targets = [];
            loops.filter(l => l.comp === comp[s]).forEach(l => l.pts.forEach(p => { const k = cellAt(p[0], p[2]); if (k >= 0) targets.push(k); }));
            if (!targets.length) stations.filter(st => st.comp === comp[s]).forEach(st => targets.push(st.k));
            if (!targets.length) continue;
            targets.sort((a, b) => Math.hypot(X(a) - X(s), Z(a) - Z(s)) - Math.hypot(X(b) - X(s), Z(b) - Z(s)));
            if (Math.hypot(X(targets[0]) - X(s), Z(targets[0]) - Z(s)) < 30) continue;   // (already on the loop)
            const p = legPath(s, targets[0]); if (!p) continue;
            const pts = cap(simplify(p).map(PT)); pts.unshift([v.p[0], gy[s] || v.p[1], v.p[2]].map(x => Math.round(x * 10) / 10));
            out.push({ type: 'boat', pts, note: 'dock ' + shortName(v.name) }); docks++;
        }
        const km = out.reduce((s, r) => s + plen(r.pts), 0) / 1000;
        notes.unshift(`${loops.length} patrol loop(s)` + (loops.length ? ' (' + loops.map(l => l.label).join('; ') + ')' : '') + `, ${docks} dock route(s), ${km.toFixed(1)} km in all.` + (dry.length ? ` Flags with no open water within 300 m${useComps ? ' of where the boats start' : ''}: ${dry.join(', ')}.` : ''));
        return res({ loops: loops.length, docks, flagsCovered: [...covered].sort(), dry, km });
    }
    const api = { filterCovered, generate, isBoatName, plen };
    if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.PatrolGen = api;
})(typeof window !== 'undefined' ? window : this);
