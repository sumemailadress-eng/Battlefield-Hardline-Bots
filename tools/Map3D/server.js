// Thugbot 3D map (the FloodBots one, pointed at Thugbot's files): a local web page that shows a map's raycast scan (the
// bots' nav grid: every walkable surface, up to 12 floors per cell, plus walls / ledges worked out from the blocked links between cells) as a 3D point cloud, with
// the routes the bots load drawn on top. Routes can be drawn, moved and deleted in the page and saved back into the
// same files Thugbot reads at map load. Separate tool: it never touches the bot DLL or the running server.
//   node server.js [serverFolder] [port]      then open http://localhost:8643/  (8642 is the FloodBots one)
// Files (in the server folder):
//   Thugbot_nav\<map>.v8.nav           scan (read only)
//   Thugbot_learned\<map>.v2.learn     "route\t<foot|ground|boat>\t<uses> <made>\tx,y,z x,y,z ..." (other lines kept)
//   Thugbot_learned\<map>.vroutes      "route ax ay az bx by bz n x y z ..." vehicle network, and <map>.exits (water exits):
//                                      Thugbot reads neither yet, so the page hides them (FEATURES) until the DLL loads them
//   Thugbot_routes\<map>.v8.routes     "spawn ..." lines kept, "route <from> <to> <len> <n> x,y,z ..." spawn/flag routes
//   Thugbot_report\<map>_*.html        flags and spawns (read only)
// Every save first copies the old files to Thugbot_learned_backup_3d\.
const http = require('http'), fs = require('fs'), path = require('path');
const { findOverhead } = require('./overhead.js');   // bridges and beams

function iniServer() {
    try {
        const t = fs.readFileSync(path.join(__dirname, '..', '..', 'Thugbot-Paths.ini'), 'utf8');
        const m = t.match(/^\s*Server\s*=\s*(.+?)\s*$/mi); if (m) return m[1];
    } catch (e) {}
    return process.cwd();   // (no Thugbot-Paths.ini: run Setup-Thugbot.bat, or pass the server folder)
}
const SERVER = process.argv[2] || iniServer();
const PORT = +(process.argv[3] || 8643);
const NAV = path.join(SERVER, 'Thugbot_nav'), LEARN = path.join(SERVER, 'Thugbot_learned');
const ROUTES = path.join(SERVER, 'Thugbot_routes'), REPORT = path.join(SERVER, 'Thugbot_report');
const BACKUP = path.join(SERVER, 'Thugbot_learned_backup_3d'), CACHE = path.join(__dirname, 'cache');
const L = 12, WALKABLE = 1, WATER = 2;
const DIRS = [[1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1], [0, -1], [1, -1]];
const FEATURES = { veh: true, exits: true };   // (the DLL reads <map>.vroutes and <map>.exits since 2026-11-05)
const LIMITS = { learnRoutes: 160, learnPts: 240, vRoutes: 160, vPts: 256, pRoutes: 800, pPts: 128 };

const safeMap = m => /^[A-Za-z0-9_]+$/.test(m || '') ? m : null;

// ---- the scan -> point cloud. kinds: 0 floor, 1 water, 2 wall / blocked edge
function buildCloud(map) {
    const file = path.join(NAV, map + '.v8.nav');
    const st = fs.statSync(file);
    const cacheFile = path.join(CACHE, map + '.cloud6');   // (2: + slope per point, 3: walls down from roof edges, 4: + floor surface triangles)
    try { const c = fs.statSync(cacheFile); if (c.mtimeMs > st.mtimeMs) return fs.readFileSync(cacheFile); } catch (e) {}
    const buf = fs.readFileSync(file);
    const magic = buf.readUInt32LE(0);
    const x0 = buf.readFloatLE(36), z0 = buf.readFloatLE(40), cell = buf.readFloatLE(44);
    const w = buf.readInt32LE(48), h = buf.readInt32LE(52), layers = buf.readInt32LE(56);
    if ((magic !== 0x3556414e && magic !== 0x3456414e) || layers !== L) throw new Error('not a Thugbot nav grid: ' + file);
    const n = w * h * L, HO = 60;
    const height = new Float32Array(buf.buffer.slice(buf.byteOffset + HO, buf.byteOffset + HO + n * 4));
    const flags = buf.subarray(HO + n * 4, HO + n * 5), edges = buf.subarray(HO + n * 5, HO + n * 6);
    // (the tester 2026-11-06: "bridges show no opening on the bottom") an edge with a drop drew a wall down to the floor below: right for
    // a building's face, but it closed bridges off. Bridges and beams get no wall down.
    const isOver = new Uint8Array(n); for (const it of findOverhead(file).items) for (const i of it.nodes) isOver[i] = 1;
    const pos = [], kind = [], slope = []; const nodeIdx = new Int32Array(n).fill(-1);   // (floor node -> its point, for the surface)
    let ymin = Infinity, ymax = -Infinity;
    const put = (x, y, z, k, s = 0) => { pos.push(x, y, z); kind.push(k); slope.push(s); };
    for (let cz = 0; cz < h; cz++) for (let cx = 0; cx < w; cx++) {
        const c = cz * w + cx, wx = x0 + cx * cell, wz = z0 + cz * cell;
        for (let l = 0; l < L; l++) {
            const node = c * L + l;
            if (!(flags[node] & WALKABLE)) continue;
            const y = height[node]; if (!isFinite(y)) continue;
            if (y < ymin) ymin = y; if (y > ymax) ymax = y;
            // slope: the steepest walkable link to a straight neighbour, in degrees
            let steep = 0;
            for (let d = 0; d < 8; d += 2) {
                if (!(edges[node] & (1 << d))) continue;
                const nx = cx + DIRS[d][0], nz = cz + DIRS[d][1]; if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                const nc = (nz * w + nx) * L; let best = null;
                for (let k = 0; k < L; k++) if (flags[nc + k] & WALKABLE) { const dh = Math.abs(height[nc + k] - y); if (best === null || dh < best) best = dh; }
                if (best !== null && best > steep) steep = best;
            }
            nodeIdx[node] = kind.length;
            put(wx, y, wz, flags[node] & WATER ? 1 : 0, Math.min(90, Math.round(Math.atan(steep / cell) * 180 / Math.PI)));
            if (flags[node] & WATER) continue;
            const wall = [null, null, null, null, null, null, null, null];
            for (let d = 0; d < 8; d += 2) {   // straight directions only
                if (edges[node] & (1 << d)) continue;
                const nx = cx + DIRS[d][0], nz = cz + DIRS[d][1];
                // wall from lo to hi (metres above this floor): 2 m by default, up to a higher neighbour floor, and at an
                // edge with a drop (a roof, a balcony, a cliff) down to the floor below - so tower facades show
                let lo = 0.4, hi = 2;
                if (nx >= 0 && nz >= 0 && nx < w && nz < h) {
                    const nc = (nz * w + nx) * L; let best = null;
                    for (let k = 0; k < L; k++) if (flags[nc + k] & WALKABLE) { const dh = height[nc + k] - y; if (best === null || Math.abs(dh) < Math.abs(best)) best = dh; }
                    if (best !== null && best < -1) {
                        if (best > -2.5 || isOver[node]) continue;   // (a step down, or the side of a bridge: no wall)
                        let bottom = y + best;   // (the nearest neighbour floor is the highest one below)
                        for (let k = 0; k < L; k++) if (k !== l && (flags[c * L + k] & WALKABLE) && height[c * L + k] < y - 1 && height[c * L + k] > bottom) bottom = height[c * L + k];   // (only down to this cell's own next floor: each storey draws its own stretch)
                        lo = Math.max(bottom - y, -400); hi = -0.2;
                    } else if (best !== null && best > 1) hi = Math.min(best, 400);
                }
                wall[d] = [lo, hi];
            }
            // two blocked sides that meet at a corner (one step of a wall running diagonally across the grid): one diagonal
            // stretch from the middle of the one side to the middle of the other, so diagonal walls come out straight
            const column = (x, z, lo, hi) => { const step = Math.max(0.6, (hi - lo) / 60); for (let yy = lo; yy <= hi + 0.01; yy += step) put(x, y + yy, z, 2, 90); };
            for (let d = 0; d < 8; d += 2) {
                const e = (d + 2) % 8; if (!wall[d] || !wall[e]) continue;
                const ax = wx + DIRS[d][0] * cell / 2, az = wz + DIRS[d][1] * cell / 2, bx = wx + DIRS[e][0] * cell / 2, bz = wz + DIRS[e][1] * cell / 2;
                const lo = Math.min(wall[d][0], wall[e][0]), hi = Math.max(wall[d][1], wall[e][1]);
                for (const u of [0, 0.5, 1]) column(ax + (bx - ax) * u, az + (bz - az) * u, lo, hi);
                wall[d] = wall[e] = null;
            }
            for (let d = 0; d < 8; d += 2) {
                if (!wall[d]) continue;
                const ex = wx + DIRS[d][0] * cell / 2, ez = wz + DIRS[d][1] * cell / 2, px = -DIRS[d][1] * cell * 0.25, pz = DIRS[d][0] * cell * 0.25;
                for (let s = -1; s <= 1; s += 2) column(ex + px * s, ez + pz * s, wall[d][0], wall[d][1]);
            }
        }
    }
    // floor surface: each node joined to its linked neighbours at +x, +z and the diagonal into triangles, so ramps and
    // stairs show as slopes (points only show where the surface is)
    const idx = [];
    const near = (cx2, cz2, y, maxDh) => { if (cx2 >= w || cz2 >= h) return -1; const nc = (cz2 * w + cx2) * L; let bestK = -1, bd = maxDh; for (let k = 0; k < L; k++) if (nodeIdx[nc + k] >= 0) { const dh = Math.abs(height[nc + k] - y); if (dh < bd) { bd = dh; bestK = nc + k; } } return bestK; };
    for (let cz = 0; cz < h - 1; cz++) for (let cx = 0; cx < w - 1; cx++) for (let l = 0; l < L; l++) {
        const a = (cz * w + cx) * L + l; if (nodeIdx[a] < 0) continue; const y = height[a];
        const B = edges[a] & 1 || flags[a] & WATER ? near(cx + 1, cz, y, 2.5) : -1, C = edges[a] & 4 || flags[a] & WATER ? near(cx, cz + 1, y, 2.5) : -1, D = near(cx + 1, cz + 1, y, 3.5);
        const A = nodeIdx[a], b = B >= 0 ? nodeIdx[B] : -1, c = C >= 0 ? nodeIdx[C] : -1, d = D >= 0 ? nodeIdx[D] : -1;
        if (b >= 0 && c >= 0 && d >= 0) idx.push(A, b, d, A, d, c);
        else if (b >= 0 && d >= 0) idx.push(A, b, d);
        else if (c >= 0 && d >= 0) idx.push(A, d, c);
        else if (b >= 0 && c >= 0) idx.push(A, b, c);
    }
    const meta = { map, x0, z0, cell, w, h, count: kind.length, tris: idx.length, ymin, ymax, file: path.basename(file) };
    const j = Buffer.from(JSON.stringify(meta)); const pad = (4 - ((4 + j.length) % 4)) % 4;
    const head = Buffer.alloc(4); head.writeUInt32LE(j.length + pad);
    const out = Buffer.concat([head, j, Buffer.alloc(pad, 32), Buffer.from(new Float32Array(pos).buffer), Buffer.from(new Uint8Array(kind).buffer), Buffer.from(new Uint8Array(slope).buffer), Buffer.alloc((4 - (kind.length * 2) % 4) % 4), Buffer.from(new Uint32Array(idx).buffer)]);
    try { fs.mkdirSync(CACHE, { recursive: true }); fs.writeFileSync(cacheFile, out); } catch (e) {}
    return out;
}

// ---- route files
const read = f => { try { return fs.readFileSync(f, 'utf8'); } catch (e) { return null; } };
const exitsFile = m => path.join(LEARN, m + '.exits');
const vehiclesFile = m => path.join(LEARN, m + '.vehicles');   // where vehicles stood at round start, written by the bots: "veh <name> x y z yaw <rounds seen>" (read only)
const learnFile = m => path.join(LEARN, m + '.v2.learn'), vFile = m => path.join(LEARN, m + '.vroutes'), pFile = m => path.join(ROUTES, m + '.v8.routes');
function reportData(map) {
    let best = null;
    try { for (const f of fs.readdirSync(REPORT)) if (f.startsWith(map + '_') && f.endsWith('.html')) { const t = fs.statSync(path.join(REPORT, f)).mtimeMs; if (!best || t > best.t) best = { f, t }; } } catch (e) {}
    if (!best) return { flags: [], spawns: [] };
    const t = read(path.join(REPORT, best.f)); const i = t.indexOf('const D=');
    if (i < 0) return { flags: [], spawns: [] };
    let j = t.indexOf('\n', i); let s = t.slice(i + 8, j).trim(); if (s.endsWith(';')) s = s.slice(0, -1);
    try { const D = JSON.parse(s); return { flags: D.flags || [], spawns: D.spawns || [], level: D.level, written: best.t }; } catch (e) { return { flags: [], spawns: [] }; }
}
function loadRoutes(map) {
    const out = { learn: [], veh: [], planned: [], spawnLines: [], files: {} };
    const lt = read(learnFile(map));
    if (lt) for (const l of lt.split(/\r?\n/)) {
        if (!l.startsWith('route\t')) continue;
        const f = l.split('\t'); if (f.length < 4) continue;
        const [uses, made] = f[2].split(' ').map(Number);
        const pts = f[3].trim().split(/\s+/).map(p => p.split(',').map(Number)).filter(p => p.length === 3 && p.every(isFinite));
        if (pts.length >= 2) out.learn.push({ kind: f[1], uses: uses || 1, made: made || 0, pts });
    }
    const vt = read(vFile(map));
    if (vt) for (const l of vt.split(/\r?\n/)) {
        if (!l.startsWith('route ')) continue;
        const v = l.trim().split(/\s+/); const fb = v[v.length - 1] === 'fb'; const n = +v[7]; const pts = [];
        for (let q = 0; q < n; q++) { const p = [+v[8 + q * 3], +v[9 + q * 3], +v[10 + q * 3]]; if (p.every(isFinite)) pts.push(p); }
        if (pts.length >= 2) out.veh.push({ fb, pts });
    }
    const pt = read(pFile(map));
    if (pt) for (const l of pt.split(/\r?\n/)) {
        if (l.startsWith('spawn ')) { out.spawnLines.push(l); continue; }
        if (!l.startsWith('route ')) continue;
        const v = l.trim().split(/\s+/); const pts = v.slice(5).map(p => p.split(',').map(Number)).filter(p => p.length === 3 && p.every(isFinite));
        if (pts.length >= 2) out.planned.push({ from: v[1], to: v[2], len: +v[3], pts });
    }
    const et = read(exitsFile(map));
    out.exits = et ? et.split(/\r?\n/).map(l => l.trim().split(/\s+/).map(Number)).filter(p => p.length === 3 && p.every(isFinite)) : [];
    const vs = read(vehiclesFile(map));
    out.vehicles = vs ? vs.split(/\r?\n/).map(l => l.trim().split(/\s+/)).filter(v => v[0] === 'veh' && v.length >= 7).map(v => ({ name: v[1], p: [+v[2], +v[3], +v[4]], yaw: +v[5], seen: +v[6] })).filter(v => v.p.every(isFinite)) : [];
    out.files = { learn: !!lt, veh: !!vt, planned: !!pt, exits: !!et };
    return out;
}
const f1 = v => (Math.round(v * 10) / 10).toFixed(1);
const chunks = (pts, max) => { if (pts.length <= max) return [pts]; const o = []; for (let i = 0; i < pts.length - 1; i += max - 1) o.push(pts.slice(i, i + max)); return o.filter(c => c.length >= 2); };
const plen = pts => { let s = 0; for (let i = 1; i < pts.length; i++) s += Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][2] - pts[i - 1][2]); return s; };
const stamp = () => new Date().toISOString().replace(/[-:]/g, '').replace('T', '_').slice(0, 15);
function backup(file) { if (!fs.existsSync(file)) return; fs.mkdirSync(BACKUP, { recursive: true }); fs.copyFileSync(file, path.join(BACKUP, path.basename(file) + '.' + stamp())); }
function saveRoutes(map, body) {
    const done = [];
    const clean = r => (r.pts || []).map(p => p.map(Number)).filter(p => p.length === 3 && p.every(isFinite));
    if (Array.isArray(body.learn)) {
        const file = learnFile(map); const old = read(file);
        let keep = old ? old.split(/\r?\n/).filter(l => !l.startsWith('route\t')) : [`AltBotMod learned\t1\t${map}_TurfWarLarge0`];
        while (keep.length && keep[keep.length - 1] === '') keep.pop();
        const now = Math.floor(Date.now() / 1000); let n = 0;
        for (const r of body.learn) for (const c of chunks(clean(r), LIMITS.learnPts)) {
            if (c.length < 2) continue;
            const kind = ['foot', 'ground', 'boat'].includes(r.kind) ? r.kind : 'foot';
            keep.push(`route\t${kind}\t${Math.max(1, r.uses | 0 || 10)} ${r.made | 0 || now}\t` + c.map(p => p.map(f1).join(',')).join(' ')); n++;
        }
        backup(file); fs.writeFileSync(file, keep.join('\n') + '\n'); done.push(`${n} learned route(s)`);
    }
    if (Array.isArray(body.veh)) {
        const file = vFile(map); const lines = [];
        for (const r of body.veh) for (const c of chunks(clean(r), LIMITS.vPts)) {
            if (c.length < 2) continue; const a = c[0], b = c[c.length - 1];
            lines.push(`route ${a.map(f1).join(' ')} ${b.map(f1).join(' ')} ${c.length} ` + c.map(p => p.map(f1).join(' ')).join(' ') + (r.fb ? ' fb' : ''));
        }
        backup(file); fs.writeFileSync(file, lines.join('\n') + (lines.length ? '\n' : '')); done.push(`${lines.length} vehicle route(s)`);
    }
    if (Array.isArray(body.planned)) {
        const file = pFile(map); const old = read(file) || '';
        const head = old.split(/\r?\n/).filter(l => l.startsWith('#') || l.startsWith('spawn '));
        if (!head.length || !head[0].startsWith('#')) head.unshift(`# Thugbot routes for ${map}_TurfWarLarge0: spawn <team> x y z <seen> / route <from> <to> <length> <points> x,y,z ...`);
        let n = 0;
        for (const r of body.planned) {
            const from = String(r.from || '').replace(/\s/g, ''), to = String(r.to || '').replace(/\s/g, '');
            if (!from || !to) continue;
            const c = clean(r).slice(0, LIMITS.pPts); if (c.length < 2) continue;
            head.push(`route ${from} ${to} ${(isFinite(r.len) && r.len > 0 && c.length === (r.pts || []).length ? r.len : plen(c)).toFixed(0)} ${c.length} ` + c.map(p => p.map(f1).join(',')).join(' ')); n++;
        }
        fs.mkdirSync(ROUTES, { recursive: true }); backup(file); fs.writeFileSync(file, head.join('\n') + '\n'); done.push(`${n} spawn/flag route(s)`);
    }
    if (Array.isArray(body.exits)) {   // ways out of the water: "x y z" per line, 64 at most (pool.inc)
        const file = exitsFile(map); const ex = body.exits.map(p => (p || []).map(Number)).filter(p => p.length === 3 && p.every(isFinite)).slice(0, 64);
        backup(file); fs.writeFileSync(file, ex.map(p => p.map(f1).join(' ')).join('\n') + (ex.length ? '\n' : '')); done.push(`${ex.length} water exit(s)`);
    }
    return done;
}
function liveMap() {
    try {
        const t = read(path.join(REPORT, 'latest.html')); const st = fs.statSync(path.join(REPORT, 'latest.html'));
        const m = t && t.match(/"level":"([A-Za-z0-9_]+?)_[A-Za-z]+\d*"/);
        return m && Date.now() - st.mtimeMs < 3 * 60 * 1000 ? m[1] : null;
    } catch (e) { return null; }
}

const overheadData = map => findOverhead(path.join(NAV, map + '.v8.nav')).items.map(({ nodes, ...it }) => it);

// ---- where the roads are (the tester 2026-11-06), from what the bots already learned, as binary: [scan count, trail count] as Uint32, then
// x y z floats. <map>.roadscan = grid nodes on a road material ("w h x0 z0 cell", then one node per line; only for this grid);
// <map>.roads = lines of "x y z x y z ..." that vehicles were driven along.
function roadData(map) {
    const scan = [], trail = [];
    const buf = fs.readFileSync(path.join(NAV, map + '.v8.nav'));
    const x0 = buf.readFloatLE(36), z0 = buf.readFloatLE(40), cell = buf.readFloatLE(44), w = buf.readInt32LE(48), h = buf.readInt32LE(52), n = w * h * L;
    const height = new Float32Array(buf.buffer.slice(buf.byteOffset + 60, buf.byteOffset + 60 + n * 4));
    const rs = read(path.join(LEARN, map + '.roadscan'));
    if (rs) {
        const lines = rs.split(/\r?\n/), head = lines[0].trim().split(/\s+/).map(Number);
        if (head[0] === w && head[1] === h && Math.abs(head[2] - x0) < 0.01 && Math.abs(head[3] - z0) < 0.01)
            for (let k = 1; k < lines.length; k++) {
                const node = +lines[k]; if (!lines[k] || !(node >= 0 && node < n)) continue;
                const y = height[node]; if (!isFinite(y)) continue;
                const c = Math.floor(node / L), cz = Math.floor(c / w), cx = c - cz * w; scan.push(x0 + cx * cell, y, z0 + cz * cell);
            }
    }
    const rd = read(path.join(LEARN, map + '.roads'));
    if (rd) for (const l of rd.split(/\r?\n/)) {
        const v = l.trim().split(/\s+/).map(Number);
        for (let k = 0; k + 5 < v.length; k += 3) {   // (points along each stretch, every 2 m)
            const d = Math.hypot(v[k + 3] - v[k], v[k + 5] - v[k + 2]), steps = Math.min(40, Math.max(1, Math.ceil(d / 2)));
            if (d > 80 || !isFinite(d)) continue;
            for (let q = 0; q < steps; q++) { const f = q / steps; trail.push(v[k] + (v[k + 3] - v[k]) * f, v[k + 1] + (v[k + 4] - v[k + 1]) * f, v[k + 2] + (v[k + 5] - v[k + 2]) * f); }
        }
    }
    const head = Buffer.from(new Uint32Array([scan.length / 3, trail.length / 3]).buffer);
    return Buffer.concat([head, Buffer.from(new Float32Array(scan).buffer), Buffer.from(new Float32Array(trail).buffer)]);
}

// ---- web server
const send = (res, code, type, body) => { res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://localhost'); const map = safeMap(u.searchParams.get('map'));
    try {
        if (u.pathname === '/' || u.pathname === '/index.html') return send(res, 200, 'text/html; charset=utf-8', fs.readFileSync(path.join(__dirname, 'index.html')));
        if (u.pathname === '/api/maps') {
            // Thugbot keeps one grid per level (mp_bank.v8.nav); a per-mode one (mp_bank_TurfWarLarge0.v8.nav) is an older
            // copy the bots no longer load, so it is listed only when the level has no grid of its own
            const all = fs.readdirSync(NAV).filter(f => f.endsWith('.v8.nav')).map(f => f.slice(0, -7));
            const maps = all.filter(m => { const b = m.match(/^(.+?)_[A-Z][A-Za-z]+\d+$/); return !b || !all.includes(b[1]); }).sort();
            return send(res, 200, 'application/json', JSON.stringify({ server: SERVER, maps, live: liveMap(), limits: LIMITS, features: FEATURES }));
        }
        // the map's own top-down image, read from this PC's game files (textures\\<map>.jpg + .json: re\\scripts\\fb_minimap.py)
        if (u.pathname === '/api/texinfo' && map) { const t = read(path.join(__dirname, 'textures', map + '.json')); return send(res, 200, 'application/json', t || 'null'); }
        if (u.pathname === '/api/teximg' && map) return send(res, 200, 'image/jpeg', fs.readFileSync(path.join(__dirname, 'textures', map + '.jpg')));
        if (u.pathname === '/api/roads' && map) return send(res, 200, 'application/octet-stream', roadData(map));
        if (u.pathname === '/api/playarea' && map) {   // the play area per mode: Thugbot_learned\\<map>.playarea (re\\scripts\\fb_playarea.py)
    const modes = {}; const txt = read(path.join(LEARN, map + '.playarea'));
    if (txt) txt.split(/\r?\n/).forEach(l => { const w = l.trim().split(/\s+/); if (w[0] !== 'area' || w.length < 3) return; const n = +w[2], pts = []; for (let k = 0; k < n; k++) pts.push([+w[3 + k * 2], +w[4 + k * 2]]); (modes[w[1]] = modes[w[1]] || []).push(pts); });
    return send(res, 200, 'application/json', JSON.stringify(modes));
}
if (u.pathname === '/api/overhead' && map) return send(res, 200, 'application/json', JSON.stringify(overheadData(map)));
        if (u.pathname === '/api/cloud' && map) return send(res, 200, 'application/octet-stream', buildCloud(map));
        if (u.pathname === '/api/routes' && map && req.method === 'GET') return send(res, 200, 'application/json', JSON.stringify({ ...loadRoutes(map), ...reportData(map), live: liveMap() === map }));
        if (u.pathname === '/api/routes' && map && req.method === 'POST') {
            let b = ''; req.on('data', d => { b += d; if (b.length > 50e6) req.destroy(); });
            req.on('end', () => { try { const done = saveRoutes(map, JSON.parse(b)); console.log(new Date().toLocaleTimeString(), map, 'saved:', done.join(', ')); send(res, 200, 'application/json', JSON.stringify({ ok: true, done })); } catch (e) { send(res, 500, 'application/json', JSON.stringify({ ok: false, error: String(e.message || e) })); } });
            return;
        }
        if (u.pathname === '/api/ply' && map) {   // the scan as a PLY point cloud (for CloudCompare / Blender / MeshLab)
            const c = buildCloud(map); const jl = c.readUInt32LE(0); const meta = JSON.parse(c.subarray(4, 4 + jl).toString());
            const pos = new Float32Array(c.buffer.slice(c.byteOffset + 4 + jl, c.byteOffset + 4 + jl + meta.count * 12)); const kind = c.subarray(4 + jl + meta.count * 12);
            const col = [[150, 160, 150], [40, 120, 230], [230, 120, 60]];
            const head = `ply\nformat binary_little_endian 1.0\ncomment Thugbot scan of ${map}\nelement vertex ${meta.count}\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n`;
            const body = Buffer.alloc(meta.count * 15);
            for (let i = 0; i < meta.count; i++) { body.writeFloatLE(pos[i * 3], i * 15); body.writeFloatLE(pos[i * 3 + 2], i * 15 + 4); body.writeFloatLE(pos[i * 3 + 1], i * 15 + 8); const k = col[kind[i]] || col[0]; body[i * 15 + 12] = k[0]; body[i * 15 + 13] = k[1]; body[i * 15 + 14] = k[2]; }
            res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Disposition': `attachment; filename="${map}.ply"` });
            return res.end(Buffer.concat([Buffer.from(head), body]));
        }
        send(res, 404, 'text/plain', 'not found');
    } catch (e) { console.error(e); send(res, 500, 'text/plain', String(e.message || e)); }
});
module.exports = { roadData, buildCloud, loadRoutes, reportData, overheadData, liveMap, LIMITS };
if (require.main === module) server.listen(PORT, '127.0.0.1', () => console.log(`Thugbot 3D map: http://localhost:${PORT}/  (server folder ${SERVER})`));
