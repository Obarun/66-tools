// 66-web frontend — ops dashboard on the live JSON API.
//
// No build step: embedded into the binary at compile time and served from
// /app.js. Talks to /api/services, /api/trees and the SSE stream /api/events.

"use strict";

/* ---------- status vocabulary ---------- */
// 66 service_status_t.state string -> visual bucket.
const ST = {
	up: "running", done: "done", failed: "failed",
	starting: "trans", stopping: "trans", finishing: "trans", restarting: "trans",
	down: "down", waiting: "down",
};
function svcBucket(s) { return ST[s.status] || "down"; }

/* ---------- state ---------- */
const state = {
	base: "", services: [], trees: [], byName: new Map(),
	mode: "services", graphScope: "trees",
	filter: "", activeStatus: "all", activeType: "all",
	selected: null,      // { kind:'svc'|'tree', name, tree? }
	lastOutput: null,    // { ok, text }
	drawerWidth: null,   // px, set by the resize handle
	graphMode: "overview",   // services scope: "overview" | "focus"
	graphFocus: null,        // key tree/name of the focused service
	graphDir: "both",        // focus: "both" | "depends" | "requiredby"
	graphOpen: new Set(),    // open navigator folders ("t:<tree>", "m:<module>")
	graphLogBadge: true,     // draw X-log as a badge on X
	graphImplied: true,      // hide A -> C when A -> B ->* C exists
	graphXf: { sig: null, k: 1, x: 0, y: 0 }, // canvas zoom/pan, kept while the view is unchanged
	es: null,
};

const el = (id) => document.getElementById(id);
function esc(s) {
	return String(s == null ? "" : s).replace(/[&<>"']/g,
		c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function cap1(v) { return v.charAt(0).toUpperCase() + v.slice(1); }

/* ---------- api ---------- */
// Every /api/* call carries X-66-Web. A cross-site page cannot set a custom
// header without a CORS preflight, which the server refuses, so the server
// rejects any mutation that arrives without it. Basic credentials are not
// touched here: same-origin, the browser attaches them itself.
const API_HEADERS = { "X-66-Web": "1" };

async function api(method, path) {
	try {
		const r = await fetch(path, { method, headers: API_HEADERS });
		const text = await r.text();
		let body = null;
		try { body = text ? JSON.parse(text) : null; } catch (_) { /* not json */ }
		return { ok: r.ok, status: r.status, body, raw: text };
	} catch (_) {
		return { ok: false, status: 0, body: null, raw: "" };
	}
}

async function loadAll() {
	const [svc, trees] = await Promise.all([
		api("GET", "/api/services"),
		api("GET", "/api/trees"),
	]);
	if (svc.ok && svc.body) { state.base = svc.body.base || ""; state.services = svc.body.services || []; }
	if (trees.ok && trees.body) { state.trees = trees.body.trees || []; }
	state.byName = new Map(state.services.map(s => [s.tree + "/" + s.name, s]));

	const n = state.services.length;
	el("scope").textContent = `${state.base} · ${n} service${n === 1 ? "" : "s"}`;
	renderVitals();
	render();
}

/* ---------- derivations ---------- */
function servicesOf(tree) {
	return state.services.filter(s => s.tree === tree).sort((a, b) => a.name.localeCompare(b.name));
}
function treeList() { return state.trees.map(t => t.name); }
function treeByName(name) { return state.trees.find(t => t.name === name); }

// Two orthogonal axes: STATUS (health, from svcBucket) and TYPE (from typestr).
// "settled" = running + down + failed is the availability denominator, so done
// and transitioning services never skew the ratio. A "down" is off, not a
// failure; "failed" is a real error — the two are kept distinct.
const STATUS_ORDER = ["running", "down", "failed", "done", "trans"];
const STATUS_LABEL = { running: "running", down: "down", failed: "failed", done: "done", trans: "transitioning" };
const TYPE_ORDER = ["classic", "oneshot", "module", "event"];

function statusCounts(list) {
	const c = { total: list.length, running: 0, down: 0, failed: 0, done: 0, trans: 0 };
	for (const s of list) c[svcBucket(s)]++;
	c.settled = c.running + c.down + c.failed;
	return c;
}
function typeCounts(list) {
	const c = { classic: 0, oneshot: 0, module: 0, event: 0 };
	for (const s of list) if (c[s.typestr] !== undefined) c[s.typestr]++;
	return c;
}
function treeStats(name) { return statusCounts(servicesOf(name)); }

// Headline health of a tree, from its services' status.
function treeHealth(t) {
	if (!t.enabled) return "idle";
	const st = treeStats(t.name);
	if (st.settled === 0) return "idle";
	if (st.failed > 0 || st.down > 0) return "down";
	return "up";
}

function agg() {
	return { st: statusCounts(state.services), ty: typeCounts(state.services), total: state.services.length };
}

/* ---------- vitals ---------- */
function renderVitals() {
	const a = agg();
	const st = a.st, ty = a.ty;
	const denom = st.settled || 1;

	// Ring: up (green) / failed (red) / down (grey), over settled services only.
	el("ring").style.setProperty("--up-deg", (st.running / denom * 360).toFixed(1));
	el("ring").style.setProperty("--down-deg", (st.failed / denom * 360).toFixed(1));
	el("ring-pct").textContent = Math.round(st.running / denom * 100) + "%";
	el("ring-legend").innerHTML =
		legendRow("st-running", "up", st.running) + legendRow("st-failed", "failed", st.failed) + legendRow("st-down", "down", st.down);

	el("tiles").innerHTML =
		tile("", a.total, "services") +
		tile("up", st.running, "running") +
		tile("idle", st.down, "down") +
		tile("down", st.failed, "failed") +
		(st.done ? tile("done", st.done, "done") : "") +
		tile("", state.trees.length, "trees");

	renderFilters(st, ty);
}
// Two filter rows — status (health) and type — each listing only non-empty
// buckets. Filtering the wall is the cross-product status AND type.
function renderFilters(st, ty) {
	const statusTabs = [{ f: "all", label: "all", n: st.total }]
		.concat(STATUS_ORDER.filter(k => st[k] > 0).map(k => ({ f: k, label: STATUS_LABEL[k], n: st[k] })));
	if (!statusTabs.some(t => t.f === state.activeStatus)) state.activeStatus = "all";

	const typeTabs = [{ f: "all", label: "all", n: st.total }]
		.concat(TYPE_ORDER.filter(k => ty[k] > 0).map(k => ({ f: k, label: k, n: ty[k] })));
	if (!typeTabs.some(t => t.f === state.activeType)) state.activeType = "all";

	el("statusfilter").innerHTML = filterButtons(statusTabs, state.activeStatus);
	el("typefilter").innerHTML = filterButtons(typeTabs, state.activeType);
}
function filterButtons(tabs, active) {
	return tabs.map(t => `<button data-f="${t.f}" class="${active === t.f ? "active" : ""}" type="button">${t.label} <span class="cnt">${t.n}</span></button>`).join("");
}
function legendRow(sw, lbl, n) { return `<div class="row"><span class="swatch ${sw}"></span> ${lbl} <b>${n}</b></div>`; }
function tile(kind, num, lbl) { return `<div class="tile ${kind}"><div class="num">${num}</div><div class="lbl">${lbl}</div></div>`; }

/* ---------- dispatcher ---------- */
function render() {
	el("wall").classList.toggle("wgraph", state.mode === "graph");
	if (state.mode === "services") renderWall();
	else if (state.mode === "trees") renderTrees();
	else if (state.mode === "graph") renderGraph();
	else renderAvailable();
}

/* ---------- available: frontends on disk not yet parsed ---------- */
let availCache = null;   // cached scan; filtered client-side, refetched on entry / after an action
async function renderAvailable() {
	if (availCache === null) {
		el("wall").innerHTML = '<div class="wall-empty">Loading…</div>';
		const r = await api("GET", "/api/available");
		availCache = (r.body && r.body.available) || [];
	}
	const q = state.filter;
	const list = q ? availCache.filter(a => a.name.toLowerCase().includes(q)) : availCache;
	if (!list.length) {
		el("wall").innerHTML = '<div class="wall-empty">' + (q ? "No available frontend matches “" + esc(q) + "”." : "No unparsed frontends — everything on disk is already parsed.") + '</div>';
		return;
	}
	el("wall").innerHTML = '<div class="avail-grid">' + list.map(availCard).join("") + '</div>';
}
function availCard(a) {
	const badge = (t, cls) => `<span class="abadge ${cls || ""}">${esc(t)}</span>`;
	const valid = !!a.type;
	return `<div class="avail-card" data-an="${esc(a.name)}" data-instd="${a.instanced ? 1 : 0}">`
		+ `<div class="ahead">`
		+ `<div class="an">${esc(a.name)}</div>`
		+ (a.removable ? `<button class="avail-rm" data-rm="1" type="button" title="remove frontend">✕</button>` : "")
		+ `</div>`
		+ `<div class="avail-badges">`
		+ (valid ? badge(a.type, "t-" + a.type) : badge("invalid frontend", "t-invalid"))
		+ `</div>`
		+ (a.description ? `<div class="avail-desc">${esc(a.description)}</div>` : "")
		+ `<button class="avail-src" data-view="1" type="button" title="view frontend">${esc(a.source)}</button>`
		+ (a.instanced ? `<input class="avail-inst" type="text" placeholder="instance (e.g. blog)" autocomplete="off">` : "")
		+ `<div class="avail-actions">`
		+ `<button class="btn" data-av="start" type="button" ${valid ? "" : "disabled"}>start</button>`
		+ `<button class="btn" data-av="enable" type="button" ${valid ? "" : "disabled"}>enable</button>`
		+ `<button class="btn primary" data-av="enablestart" type="button" ${valid ? "" : "disabled"}>enable + start</button>`
		+ `</div></div>`;
}
el("wall").addEventListener("click", async (e) => {
	if (state.mode !== "available") return;

	const view = e.target.closest("[data-view]");
	if (view) {
		const card = view.closest(".avail-card"); if (!card) return;
		const name = card.dataset.an;
		showModal("Loading…", "", "", []);
		const r = await api("GET", "/api/frontend/" + encodeURIComponent(name));
		if (r.status === 200 && r.body) showModal(name, "", r.body.content || "(empty)", [{ label: "Close", onClick: closeModal }]);
		else showModal("Could not read frontend", "err", (r.body && r.body.error) || ("request failed (" + r.status + ")"), [{ label: "Close", onClick: closeModal }]);
		return;
	}

	const rm = e.target.closest("[data-rm]");
	if (rm) {
		const card = rm.closest(".avail-card"); if (!card) return;
		const name = card.dataset.an;
		showModal("Remove “" + name + "”?", "warn", "This deletes the frontend from disk. This cannot be undone.", [
			{ label: "Cancel", onClick: closeModal },
			{ label: "Remove", cls: "danger", onClick: async () => {
				closeModal();
				const r = await api("POST", "/api/remove/" + encodeURIComponent(name));
				if (r.status === 200) { toast("“" + name + "” removed"); availCache = null; renderAvailable(); }
				else showModal("Could not remove it", "err", (r.body && r.body.error) || ("request failed (" + r.status + ")"), [{ label: "Close", onClick: closeModal }]);
			} },
		]);
		return;
	}

	const av = e.target.closest("[data-av]"); if (!av) return;
	const card = av.closest(".avail-card"); if (!card) return;
	const name = card.dataset.an, instanced = card.dataset.instd === "1";
	let full = name;
	if (instanced) {
		const inst = (card.querySelector(".avail-inst").value || "").trim();
		if (!inst) { showModal("Instance required", "warn", "This is a template — enter an instance name (e.g. blog).", [{ label: "Close", onClick: closeModal }]); return; }
		full = (name.endsWith("@") ? name : name + "@") + inst;
	}
	const action = av.dataset.av;
	const done = action === "start" ? "started" : action === "enablestart" ? "enabled + started" : "enabled";
	av.disabled = true;
	try {
		const resp = await fetch("/api/enable", { method: "POST", headers: { "Content-Type": "application/json", ...API_HEADERS }, body: JSON.stringify({ name: full, action }) });
		const data = await resp.json().catch(() => ({}));
		if (resp.status === 200) { toast("“" + full + "” " + done); availCache = null; loadAll(); }
		else if (resp.status === 503) { showModal("66 could not " + action + " it", "err", data.output || "(no output)", [{ label: "Close", onClick: closeModal }]); av.disabled = false; }
		else { showModal("Action failed", "err", data.error || ("request failed (" + resp.status + ")"), [{ label: "Close", onClick: closeModal }]); av.disabled = false; }
	} catch (err) { showModal("Network error", "err", String(err), [{ label: "Close", onClick: closeModal }]); av.disabled = false; }
});

/* ---------- services wall ---------- */
function matchWall(s) {
	return (state.activeStatus === "all" || svcBucket(s) === state.activeStatus)
		&& (state.activeType === "all" || s.typestr === state.activeType)
		&& (!state.filter || s.name.toLowerCase().includes(state.filter));
}
function renderWall() {
	const wall = el("wall");
	wall.innerHTML = "";
	let any = false;
	for (const t of treeList()) {
		const list = servicesOf(t);
		if (!list.length) continue;
		const st = treeStats(t);

		const block = document.createElement("div");
		block.className = "tree-block";
		const statCls = (st.failed > 0 || st.down > 0) ? "hasdown" : (st.settled > 0 && st.running === st.settled ? "allup" : "");
		block.innerHTML =
			`<div class="tree-head"><span class="tname" data-open="tree" data-name="${esc(t)}">${esc(t)}</span>`
			+ `<span class="grow"></span><span class="tstat ${statCls}">${st.running}/${st.settled} up`
			+ `${st.down ? ` · ${st.down} down` : ""}${st.failed ? ` · ${st.failed} failed` : ""}${st.done ? ` · ${st.done} done` : ""}</span></div>`;

		const grid = document.createElement("div");
		grid.className = "grid";
		let shown = 0;
		for (const s of list) {
			const show = matchWall(s);
			if (show) { shown++; any = true; }
			grid.insertAdjacentHTML("beforeend", card(s, t, !show));
		}
		if (shown === 0) block.style.display = "none";
		block.appendChild(grid);
		wall.appendChild(block);
	}
	if (!any) wall.insertAdjacentHTML("beforeend", `<div class="wall-empty">No service matches the current filter.</div>`);
}
function card(s, tree, hidden) {
	const b = svcBucket(s);
	const dc = s.depends ? s.depends.length : 0;
	const tag = (s.typestr && s.typestr !== "classic") ? `<span class="ctype ${esc(s.typestr)}">${esc(s.typestr)}</span>` : "";
	return `<div class="card st-${b} ${hidden ? "hide" : ""}" data-open="svc" data-name="${esc(s.name)}" data-tree="${esc(tree)}">`
		+ `<div class="top"><span class="dot st-${b}"></span><span class="cname">${esc(s.name)}</span>${tag}</div>`
		+ `<div class="meta"><span class="st">${esc(s.status || "down")}</span>`
		+ (s.pid ? `<span>pid ${s.pid}</span>` : "")
		+ (dc ? `<span class="grow"></span><span title="direct dependencies">${dc} dep${dc > 1 ? "s" : ""}</span>` : "")
		+ `</div></div>`;
}

/* ---------- trees view ---------- */
function renderTrees() {
	const wall = el("wall");
	wall.innerHTML = "";
	const grid = document.createElement("div");
	grid.className = "tree-grid";
	let any = false;
	for (const t of state.trees) {
		if (state.filter && !t.name.toLowerCase().includes(state.filter)) continue;
		any = true;
		grid.insertAdjacentHTML("beforeend", treeCard(t));
	}
	if (!any) { wall.innerHTML = `<div class="wall-empty">No tree matches the filter.</div>`; return; }
	wall.appendChild(grid);
}
function treeCard(t) {
	const h = treeHealth(t), st = treeStats(t.name);
	return `<div class="tcard ${h}" data-open="tree" data-name="${esc(t.name)}">`
		+ `<div class="tc-top"><span class="tc-name">${esc(t.name)}</span>`
		+ (t.current ? `<span class="crown">★</span><span class="tc-current">current</span>` : "")
		+ `<span class="grow"></span><span class="tc-group">${esc(t.group || "")}</span></div>`
		+ `<div class="tc-ratio">${miniBar(st)}<span>${st.running}/${st.settled} up</span></div>`
		+ `<div class="caps">${cap("enabled", t.enabled)}</div>`
		+ `</div>`;
}
function miniBar(st) {
	if (!st.total) return `<div class="minibar"></div>`;
	const seg = (cls, n) => n ? `<div class="seg st-${cls}" style="width:${(n / st.total * 100).toFixed(1)}%"></div>` : "";
	return `<div class="minibar">${seg("running", st.running)}${seg("failed", st.failed)}${seg("down", st.down)}${seg("done", st.done)}${seg("trans", st.trans)}</div>`;
}
function cap(l, on) { return `<span class="cap ${on ? "on" : ""}">${l}</span>`; }

/* ---------- dependency graph: model ---------- */
// One node per service, keyed tree/name. A module is a node that "contains" its
// services: those arrows are drawn apart and never make the module a
// "required by" of its own services (it shows in the focus header instead).
function graphModel() {
	const nodes = new Map();
	for (const s of state.services) nodes.set(s.tree + "/" + s.name, {
		key: s.tree + "/" + s.name, name: s.name, tree: s.tree, st: svcBucket(s),
		inns: s.inns || null, module: s.typestr === "module", logger: s.logger || null, log: null,
		open: `data-open="svc" data-name="${esc(s.name)}" data-tree="${esc(s.tree)}"`,
		rawdeps: s.depends || [], rawcontents: s.contents || [],
	});
	// A name is looked up in the same tree first, then in any tree: a service
	// may depend on a service or a module living in another tree.
	const byName = new Map();
	for (const n of nodes.values()) if (!byName.has(n.name)) byName.set(n.name, n.key);
	const find = (from, name) => nodes.has(from.tree + "/" + name) ? from.tree + "/" + name : byName.get(name) || null;
	for (const n of nodes.values()) {
		n.contains = new Set(n.rawcontents.map(d => find(n, d)).filter(Boolean));
		n.depends = [...n.rawdeps.map(d => find(n, d)).filter(Boolean), ...n.contains];
	}
	if (state.graphLogBadge) for (const n of [...nodes.values()]) {
		const lk = n.logger && find(n, n.logger);
		if (!lk || !nodes.has(lk)) continue;
		n.log = nodes.get(lk).st;
		nodes.delete(lk);
	}
	for (const n of nodes.values()) {
		n.depends = [...new Set(n.depends.filter(d => nodes.has(d) && d !== n.key))];
		for (const d of [...n.contains]) if (!nodes.has(d)) n.contains.delete(d);
	}
	return graphLink(nodes);
}
function treeModel() {
	const known = new Set(state.trees.map(t => t.name));
	const ST_OF = { up: "running", down: "failed", idle: "down" };
	const nodes = new Map();
	for (const t of state.trees) {
		const st = treeStats(t.name);
		nodes.set(t.name, {
			key: t.name, name: t.name, tree: t.name, st: ST_OF[treeHealth(t)], current: !!t.current,
			title: `${st.running}/${st.settled} up`, log: null, module: false, contains: new Set(),
			open: `data-open="tree" data-name="${esc(t.name)}"`,
			depends: (t.depends || []).filter(d => known.has(d)),
		});
	}
	return graphLink(nodes);
}
// Hide implied arrows (A -> C when A -> B ->* C already exists), never a
// "contains" arrow, then derive the reverse edges.
function graphLink(nodes) {
	if (state.graphImplied) {
		const memo = new Map();
		const reach = (k) => {
			if (memo.has(k)) return memo.get(k);
			const r = new Set(); memo.set(k, r);
			for (const d of nodes.get(k).depends) { r.add(d); for (const x of reach(d)) r.add(x); }
			return r;
		};
		for (const n of nodes.values())
			n.depends = n.depends.filter(d => n.contains.has(d) || !n.depends.some(o => o !== d && reach(o).has(d)));
	}
	for (const n of nodes.values()) n.rev = [];
	for (const n of nodes.values()) for (const d of n.depends) nodes.get(d).rev.push(n.key);
	for (const n of nodes.values()) n.req = n.rev.filter(r => !nodes.get(r).contains.has(n.key));
	return nodes;
}
// Overview only: modules stay closed. Their services are hidden and any arrow
// to one of them is carried by the module itself.
function closeModules(full) {
	const modKey = new Map();
	for (const n of full.values()) if (n.module) modKey.set(n.name, n.key);
	const inClosed = (n) => n && n.inns && modKey.has(n.inns);
	const out = new Map();
	for (const n of full.values()) {
		if (inClosed(n)) continue;
		const deps = n.depends.filter(d => !n.contains.has(d))
			.map(d => inClosed(full.get(d)) ? modKey.get(full.get(d).inns) : d)
			.filter(d => d !== n.key);
		out.set(n.key, { ...n, depends: [...new Set(deps)], contains: new Set(), size: n.contains.size });
	}
	return graphLink(out);
}
function graphComponents(nodes) {
	const seen = new Set(), out = [];
	for (const n of nodes.values()) {
		if (seen.has(n.key)) continue;
		const c = [], stack = [n.key];
		seen.add(n.key);
		while (stack.length) {
			const k = stack.pop(), m = nodes.get(k);
			c.push(m);
			for (const x of [...m.depends, ...m.rev]) if (!seen.has(x)) { seen.add(x); stack.push(x); }
		}
		out.push(c);
	}
	return out.sort((a, b) => b.length - a.length || a[0].name.localeCompare(b[0].name));
}
// Breadth-first distance from key, following next(node).
function graphWalk(nodes, key, next) {
	const m = new Map([[key, 0]]), q = [key];
	while (q.length) {
		const k = q.shift();
		for (const x of next(nodes.get(k))) if (!m.has(x)) { m.set(x, m.get(k) + 1); q.push(x); }
	}
	return m;
}

/* ---------- dependency graph: drawing ---------- */
const G_NH = 24, G_GX = 14, G_GY = 34, G_CH = 6.7, G_DETW = 50, G_MODW = 48;
function gNodeW(n) { return Math.round(n.name.length * G_CH + 30 + (n.log ? 16 : 0) + (n.module ? G_MODW : 0) + (n.current ? 16 : 0) + G_DETW); }
function gNodeSvg(n, p, focus) {
	const detx = p.w - G_DETW - (n.log ? 12 : 4);
	return `<g class="gn st-${n.st}${n.module ? " mod" : ""}${n.key === focus ? " sel" : ""}" data-k="${esc(n.key)}" transform="translate(${p.x},${p.y})">`
		+ `<title>${esc(n.name)}${n.title ? " · " + esc(n.title) : " · tree/" + esc(n.tree)}${n.log ? " · logger " + n.log : ""}</title>`
		+ `<rect class="box" width="${p.w}" height="${G_NH}" rx="12"/>`
		+ `<circle class="dot" cx="12" cy="12" r="3.5"/>`
		+ `<text class="lbl" x="22" y="16">${esc(n.name)}${n.current ? " ★" : ""}</text>`
		+ (n.module ? `<text class="mtag" x="${detx - G_MODW + 4}" y="16">module</text>` : "")
		+ `<text class="gdet" ${n.open} x="${detx}" y="16">details</text>`
		+ (n.log ? `<rect class="logb st-${n.log}" x="${p.w - 17}" y="8" width="8" height="8" rx="2"/>` : "")
		+ `</g>`;
}
function gEdgesSvg(n, pos, dir, keep) {
	let e = "";
	for (const d of n.depends) {
		if (!keep(d)) continue;
		const a = pos[d], b = pos[n.key];
		let path;
		if (dir === "TB") {
			const x1 = a.x + a.w / 2, y1 = a.y + G_NH, x2 = b.x + b.w / 2, y2 = b.y, m = (y1 + y2) / 2;
			path = `M${x1} ${y1} C${x1} ${m},${x2} ${m},${x2} ${y2}`;
		} else {
			const x1 = a.x + a.w, y1 = a.y + G_NH / 2, x2 = b.x, y2 = b.y + G_NH / 2, m = (x1 + x2) / 2;
			path = `M${x1} ${y1} C${m} ${y1},${m} ${y2},${x2} ${y2}`;
		}
		e += `<path class="ge${n.contains.has(d) ? " contains" : ""}" data-f="${esc(d)}" data-t="${esc(n.key)}" d="${path}" marker-end="url(#garrow)"/>`;
	}
	return e;
}
const G_DEFS = `<defs><marker id="garrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto"><path d="M0,0 L10,5 L0,10 z"/></marker></defs>`;

// Top-down layered layout: rank = longest path from a node without
// dependencies, barycentric ordering, ranks wider than wrap split in rows.
function gLayeredTB(list, wrap) {
	const set = new Map(list.map(n => [n.key, n]));
	const dep = (n) => n.depends.filter(d => set.has(d));
	const rev = (n) => n.rev.filter(d => set.has(d));
	const rank = {};
	const rk = (k) => {
		if (rank[k] !== undefined) return rank[k];
		rank[k] = 0; // provisional: breaks a dependency cycle instead of recursing forever
		return rank[k] = Math.max(0, ...dep(set.get(k)).map(d => rk(d) + 1));
	};
	list.forEach(n => rk(n.key));
	const maxR = Math.max(...list.map(n => rank[n.key]));
	const order = [];
	for (let r = 0; r <= maxR; r++) order[r] = list.filter(n => rank[n.key] === r).sort((a, b) => a.name.localeCompare(b.name));
	for (let s = 0; s < 10; s++) {
		const idx = {};
		order.forEach(row => row.forEach((n, i) => idx[n.key] = i));
		const rs = [...order.keys()];
		if (s % 2) rs.reverse();
		for (const r of rs) order[r] = order[r].map(n => {
			const nb = [...dep(n), ...rev(n)].map(k => idx[k]);
			return { n, bc: nb.length ? nb.reduce((a, b) => a + b, 0) / nb.length : idx[n.key] };
		}).sort((a, b) => a.bc - b.bc).map(o => o.n);
	}
	const rows = [];
	order.forEach(row => { for (let i = 0; i < row.length; i += wrap) rows.push(row.slice(i, i + wrap)); });
	const rw = rows.map(R => R.reduce((a, n) => a + gNodeW(n) + G_GX, -G_GX)), full = Math.max(...rw), pos = {};
	rows.forEach((R, i) => {
		let x = (full - rw[i]) / 2;
		const y = i * (G_NH + G_GY);
		R.forEach(n => { const w = gNodeW(n); pos[n.key] = { x, y, w }; x += w + G_GX; });
	});
	const edges = list.map(n => gEdgesSvg(n, pos, "TB", d => set.has(d))).join("");
	return { W: full, H: rows.length * (G_NH + G_GY) - G_GY, body: `<g>${edges}</g><g>${list.map(n => gNodeSvg(n, pos[n.key], null)).join("")}</g>` };
}
// Left-to-right columns around the focus: column = signed distance,
// dependencies negative, dependents positive.
function gLayoutCols(nodes, keys, col, focus) {
	const cols = {};
	for (const k of keys) (cols[col(k)] = cols[col(k)] || []).push(nodes.get(k));
	const colNodes = Object.keys(cols).map(Number).sort((a, b) => a - b).map(c => cols[c]);
	for (let i = 1; i < colNodes.length; i++) {
		const prev = new Map(colNodes[i - 1].map((n, j) => [n.key, j]));
		colNodes[i] = colNodes[i].map(n => {
			const nb = [...n.depends, ...n.rev].filter(k => prev.has(k)).map(k => prev.get(k));
			return { n, bc: nb.length ? nb.reduce((a, b) => a + b, 0) / nb.length : Infinity };
		}).sort((a, b) => a.bc - b.bc || a.n.name.localeCompare(b.n.name)).map(o => o.n);
	}
	const colH = colNodes.map(c => c.length * (G_NH + 10) - 10), full = Math.max(...colH), pos = {};
	let x = 0, lbl = "";
	colNodes.forEach((c, i) => {
		let y = 24 + (full - colH[i]) / 2;
		c.forEach(n => { pos[n.key] = { x, y, w: gNodeW(n) }; y += G_NH + 10; });
		const id = col(c[0].key);
		if (id === -1 || id === 1) lbl += `<text class="gcolh" x="${x}" y="10">${id < 0 ? "depends ←" : "required by →"}</text>`;
		x += Math.max(...c.map(gNodeW)) + 80;
	});
	const flat = colNodes.flat();
	// Only arrows pointing away from the focus, so the picture reads left to right.
	const edges = flat.map(n => gEdgesSvg(n, pos, "LR", d => keys.has(d) && col(d) < col(n.key))).join("");
	return { W: x - 80, H: full + 24, body: `<g>${lbl}</g><g>${edges}</g><g>${flat.map(n => gNodeSvg(n, pos[n.key], focus)).join("")}</g>` };
}

/* ---------- dependency graph: view ---------- */
let gModel = null, gLay = null;
function renderGraph() {
	const isTree = state.graphScope === "trees", isFocus = state.graphMode === "focus";
	const seg = (id, attr, items, cur) => `<div class="segmented" id="${id}">`
		+ items.map(([v, l]) => `<button ${attr}="${v}" class="${cur === v ? "active" : ""}" type="button">${l}</button>`).join("") + `</div>`;
	el("wall").innerHTML = `<div class="graph-head">`
		+ seg("gscope", "data-g", [["trees", "trees"], ["services", "services"]], state.graphScope)
		+ (isTree ? "" : seg("gmode", "data-gm", [["overview", "overview"], ["focus", "focus"]], state.graphMode)
			+ `<label class="gchk" title="The logger X-log is drawn as a small square on X instead of a separate node"><input type="checkbox" id="glog"${state.graphLogBadge ? " checked" : ""}> loggers as badges</label>`
			+ `<label class="gchk" title="If A depends on B and B depends on C, the direct arrow A → C adds nothing: it is not drawn"><input type="checkbox" id="gimplied"${state.graphImplied ? " checked" : ""}> hide implied arrows</label>`)
		+ `<span class="grow"></span><span class="glegend">click = ${isTree ? "highlight" : "focus"} · details = drawer · scroll = zoom · drag = pan</span></div>`
		+ `<div class="gbox" id="gbox">${isTree ? "" : `<nav class="gnav" id="gnav"></nav>`}<div class="gpane" id="gpane"></div></div>`;
	sizeGraph();
	if (isTree) { gModel = null; graphTrees(); return; }
	gModel = graphModel();
	if (isFocus && state.graphFocus) revealInNav(gModel.get(state.graphFocus));
	renderGraphNav();
	if (isFocus) graphFocus(gModel); else graphOverview(gModel);
}
function sizeGraph() {
	const box = el("gbox"); if (!box) return;
	const top = box.getBoundingClientRect().top + window.scrollY;
	box.style.height = Math.max(360, window.innerHeight - top - 12) + "px";
}
function graphTrees() {
	const pane = el("gpane"), nodes = treeModel();
	if (!nodes.size) { pane.innerHTML = `<div class="wall-empty">No trees.</div>`; return; }
	graphCanvas(pane, gLayeredTB([...nodes.values()], 8), "trees");
}
function graphOverview(full) {
	const pane = el("gpane"), nodes = closeModules(full);
	pane.classList.add("scroll");
	if (!nodes.size) { pane.innerHTML = `<div class="wall-empty">No services.</div>`; return; }
	const comps = graphComponents(nodes), linked = comps.filter(c => c.length > 1), alone = comps.filter(c => c.length === 1).map(c => c[0]);
	let h = `<div class="gislands"><div class="gsect"><h3>linked groups · ${linked.length}</h3><div class="gcards">`;
	for (const c of linked) {
		const lay = gLayeredTB(c, 6);
		h += `<div class="gcard"><div class="gch"><b>${c.length}</b> services <span>tree/${esc([...new Set(c.map(n => n.tree))].sort().join(", "))}</span></div>`
			+ `<svg viewBox="-4 -4 ${lay.W + 8} ${lay.H + 8}" width="${lay.W + 8}">${G_DEFS}${lay.body}</svg></div>`;
	}
	h += `</div></div><div class="gsect"><h3>no link · ${alone.length}</h3>`;
	const byTree = {};
	alone.forEach(n => (byTree[n.tree] = byTree[n.tree] || []).push(n));
	for (const t of Object.keys(byTree).sort()) {
		h += `<div class="gtreegrp"><div class="gtn">tree/${esc(t)} · ${byTree[t].length}</div><div class="gchips">`;
		for (const n of byTree[t].sort((a, b) => a.name.localeCompare(b.name)))
			h += `<span class="gchip st-${n.st}${n.module ? " mod" : ""}" data-k="${esc(n.key)}"><i></i>${esc(n.name)}`
				+ (n.module ? `<span class="mt">module</span>` : "") + (n.log ? `<span class="lg">log</span>` : "")
				+ `<span class="gdet" ${n.open}>details</span></span>`;
		h += `</div></div>`;
	}
	pane.innerHTML = h + `</div></div>`;
}
function graphFocus(nodes) {
	const pane = el("gpane"), key = state.graphFocus, f = key && nodes.get(key);
	if (!f) { pane.innerHTML = `<div class="wall-empty">Pick a service in the list.</div>`; return; }
	const own = (n) => n.key === key ? n.depends.filter(d => !n.contains.has(d)) : n.depends;
	const up = state.graphDir !== "requiredby" ? graphWalk(nodes, key, n => n.depends) : new Map([[key, 0]]);
	const dn = state.graphDir !== "depends" ? graphWalk(nodes, key, n => n.req) : new Map([[key, 0]]);
	const keys = new Set([...up.keys(), ...dn.keys()]);
	const col = (k) => k === key ? 0 : up.has(k) ? -up.get(k) : dn.get(k);
	pane.innerHTML = `<div class="gfbar"><span class="gwho"><b>${esc(f.name)}</b> · tree/${esc(f.tree)}`
		+ (f.inns ? ` · module ${esc(f.inns)}` : "") + (f.module ? ` · contains ${f.contains.size}` : "")
		+ ` · depends on ${graphWalk(nodes, key, own).size - 1} · required by ${graphWalk(nodes, key, n => n.req).size - 1}</span>`
		+ `<div class="segmented" id="gdir">`
		+ [["both", "both"], ["depends", "depends"], ["requiredby", "required by"]]
			.map(([v, l]) => `<button data-gd="${v}" class="${state.graphDir === v ? "active" : ""}" type="button">${l}</button>`).join("")
		+ `</div></div>`;
	graphCanvas(pane, gLayoutCols(nodes, keys, col, key), `focus:${key}:${state.graphDir}:${state.graphLogBadge}:${state.graphImplied}`);
}

/* ---------- dependency graph: navigator (tree > module > service) ---------- */
function revealInNav(n) {
	if (!n) return;
	state.graphOpen.add("t:" + n.tree);
	if (n.inns) state.graphOpen.add("m:" + n.inns);
	if (n.module) state.graphOpen.add("m:" + n.name);
}
function renderGraphNav() {
	const nav = el("gnav"); if (!nav || !gModel) return;
	const q = state.filter, focus = state.graphMode === "focus" ? state.graphFocus : null;
	const all = [...gModel.values()];
	const hit = (n) => !q || n.name.toLowerCase().includes(q);
	const isOpen = (id) => !!q || state.graphOpen.has(id);
	const mods = new Map(all.filter(n => n.module).map(n => [n.name, n]));
	const member = (n) => n.inns && mods.has(n.inns);
	const svc = (n, lvl, label, extra) => `<div class="gnv svc st-${n.st}${n.key === focus ? " on" : ""}" style="--lvl:${lvl}" data-k="${esc(n.key)}" title="${esc(n.name)}">`
		+ `<span class="car"></span><i></i>${esc(label)}${extra}<span class="c">${n.depends.length - n.contains.size}↑ ${n.req.length}↓</span></div>`;
	let h = "";
	for (const t of [...new Set(all.map(n => n.tree))].sort()) {
		const inTree = all.filter(n => n.tree === t);
		const tmods = inTree.filter(n => n.module).sort((a, b) => a.name.localeCompare(b.name));
		const plain = inTree.filter(n => !n.module && !member(n) && hit(n)).sort((a, b) => a.name.localeCompare(b.name));
		const mhits = tmods.map(m => [m, all.filter(n => n.inns === m.name && hit(n)).sort((a, b) => a.name.localeCompare(b.name))])
			.filter(([m, list]) => !q || hit(m) || list.length);
		if (q && !plain.length && !mhits.length) continue;
		// Count what the folder lists: its own services, plus each module with its members.
		const shown = inTree.filter(n => !n.module && !member(n)).length
			+ tmods.reduce((a, m) => a + 1 + all.filter(n => n.inns === m.name).length, 0);
		const tid = "t:" + t;
		h += `<div class="gnv fold" style="--lvl:0" data-gfold="${esc(tid)}"><span class="car">${isOpen(tid) ? "▾" : "▸"}</span><span class="ic"></span>tree/${esc(t)}<span class="c">${shown}</span></div>`;
		if (!isOpen(tid)) continue;
		for (const [m, list] of mhits) {
			const mid = "m:" + m.name;
			h += `<div class="gnv fold mod${m.key === focus ? " on" : ""}" style="--lvl:1" data-gmod="${esc(m.key)}">`
				+ `<span class="car" data-gfold="${esc(mid)}">${isOpen(mid) ? "▾" : "▸"}</span><span class="ic"></span>${esc(m.name)}`
				+ `<span class="c">module · ${m.contains.size} · ${m.depends.length - m.contains.size}↑ ${m.req.length}↓</span></div>`;
			if (!isOpen(mid)) continue;
			// A module's services may live in another tree than the module itself.
			for (const n of list) {
				const short = n.name.startsWith(m.name + ":") ? n.name.slice(m.name.length + 1) : n.name;
				h += svc(n, 2, short, n.tree !== m.tree ? `<span class="ot">tree/${esc(n.tree)}</span>` : "");
			}
		}
		for (const n of plain) h += svc(n, 1, n.name, "");
	}
	nav.innerHTML = h || `<div class="gnv" style="--lvl:0">no match</div>`;
}
function graphSetFocus(key) {
	state.graphFocus = key;
	state.graphMode = "focus";
	renderGraph();
}

/* ---------- dependency graph: zoom / pan canvas ---------- */
// The transform is kept across re-renders (SSE refresh) while the view
// signature is unchanged; a new view is fitted to the pane.
function graphCanvas(pane, lay, sig) {
	gLay = lay;
	pane.insertAdjacentHTML("beforeend", `<svg class="gzoom">${G_DEFS}<g class="gvp">${lay.body}</g></svg>`
		+ `<div class="gzctl"><button class="icon-btn" data-gz="fit" type="button">fit</button><button class="icon-btn" data-gz="1" type="button">100%</button></div>`);
	if (state.graphXf.sig === sig) graphApply(); else { state.graphXf.sig = sig; graphFit(); }
}
function graphApply() {
	const vp = el("wall").querySelector(".gvp"); if (!vp) return;
	const t = state.graphXf;
	vp.setAttribute("transform", `translate(${t.x},${t.y}) scale(${t.k})`);
}
function graphFit() {
	const svg = el("wall").querySelector("svg.gzoom"); if (!svg || !gLay) return;
	const cw = svg.clientWidth, ch = svg.clientHeight, M = 30, TOP = 44, t = state.graphXf;
	t.k = Math.min((cw - 2 * M) / gLay.W, (ch - TOP - 2 * M) / gLay.H, 1.4);
	t.x = (cw - gLay.W * t.k) / 2;
	t.y = TOP + (ch - TOP - gLay.H * t.k) / 2;
	graphApply();
}
function graphHover(g, on) {
	const svg = g.closest("svg"), k = g.dataset.k;
	if (!on) { svg.querySelectorAll(".ge, .gn").forEach(x => x.classList.remove("hl", "dim")); return; }
	const keep = new Set([k]);
	svg.querySelectorAll(".ge").forEach(p => {
		const hit = p.dataset.f === k || p.dataset.t === k;
		p.classList.toggle("hl", hit); p.classList.toggle("dim", !hit);
		if (hit) { keep.add(p.dataset.f); keep.add(p.dataset.t); }
	});
	svg.querySelectorAll(".gn").forEach(n => n.classList.toggle("dim", !keep.has(n.dataset.k)));
}

/* ---------- drawer: shared bits ---------- */
function showDrawer() { if (state.drawerWidth) el("drawer").style.width = state.drawerWidth + "px"; el("scrim").classList.add("open"); el("drawer").classList.add("open"); el("drawer").setAttribute("aria-hidden", "false"); }
function closeDrawer() { state.selected = null; state.lastOutput = null; el("scrim").classList.remove("open"); el("drawer").classList.remove("open"); el("drawer").setAttribute("aria-hidden", "true"); }

function kv(k, v) { return `<dt>${k}</dt><dd>${esc(v)}</dd>`; }
function kvRaw(k, html) { return `<dt>${k}</dt><dd>${html}</dd>`; }
function chip(on) { return `<span class="chip ${on ? "on" : ""}">${on ? "yes" : "no"}</span>`; }
function relTime(sec) {
	if (!sec) return "";
	let d = Math.floor(Date.now() / 1000) - sec; if (d < 0) d = 0;
	if (d < 60) return d + "s";
	if (d < 3600) return Math.floor(d / 60) + "min";
	if (d < 86400) return Math.floor(d / 3600) + "h " + Math.floor((d % 3600) / 60) + "min";
	return Math.floor(d / 86400) + "d " + Math.floor((d % 86400) / 3600) + "h";
}
function depChip(name, kind, tree) {
	if (kind === "tree") {
		const t = treeByName(name);
		const cls = t ? treeHealth(t) : "idle";
		return `<span class="dep-chip" data-open="tree" data-name="${esc(name)}"><span class="dot ${cls}"></span>${esc(name)}</span>`;
	}
	const s = state.services.find(x => x.name === name);
	const cls = s ? "st-" + svcBucket(s) : "st-down";
	return `<span class="dep-chip" data-open="svc" data-name="${esc(name)}" data-tree="${esc(s ? s.tree : tree || "")}"><span class="dot ${cls}"></span>${esc(name)}</span>`;
}
function depGroup(label, arrow, list, kind, tree) {
	const items = list || [];
	const chips = items.length
		? `<div class="dep-chips">` + items.map(n => depChip(n, kind, tree)).join("") + `</div>`
		: `<div class="dep-none">none</div>`;
	return `<div class="grp"><div class="glabel"><span class="arrow">${arrow}</span> ${label}</div>${chips}</div>`;
}
function actGrp(label, acts, kind, name, tree) {
	return `<div class="act-grp"><div class="gl">${label}</div><div class="act-row">`
		+ acts.map(([verb, cls]) => `<button class="btn ${cls}" data-act="${verb}" data-actkind="${kind}" data-actname="${esc(name)}" data-acttree="${esc(tree || "")}">${cap1(verb)}</button>`).join("")
		+ `</div></div>`;
}
function outputBlock() {
	if (!state.lastOutput) return "";
	return `<pre class="output ${state.lastOutput.ok ? "" : "error"}">${esc(state.lastOutput.text)}</pre>`;
}

/* ---------- service drawer ---------- */
async function openServiceDrawer(name, tree) {
	state.selected = { kind: "svc", name, tree };
	showDrawer();
	el("d-name").textContent = name;
	el("d-sub").textContent = "tree/" + (tree || "");
	el("d-body").innerHTML = `<div class="dep-none">loading…</div>`;
	const r = await api("GET", "/api/services/" + encodeURIComponent(name));
	if (!state.selected || state.selected.kind !== "svc" || state.selected.name !== name) return;
	if (!r.ok || !r.body) { el("d-body").innerHTML = `<div class="wall-empty">Service not found.</div>`; return; }
	renderServiceDrawer(r.body);
}
function renderServiceDrawer(s) {
	const b = ST[s.status] || "down";
	el("d-name").textContent = s.name;
	el("d-sub").textContent = `tree/${s.treename || ""} · ${s.typestr || s.type}`;
	const since = s.since ? relTime(s.since) + " ago" + (s.who ? " · by " + s.who : "") : "—";

	el("d-body").innerHTML =
		`<span class="big-badge st-${b}"><span class="dot st-${b}"></span>${esc(s.status || "down")}</span>`
		+ `<div class="dsection"><h3>Metadata</h3><dl class="kv">`
		+ kv("Description", s.description || "(none)")
		+ kvRaw("Tree", s.treename ? `<span class="srclink" data-open="tree" data-name="${esc(s.treename)}" title="show tree detail">${esc(s.treename)}</span>` : "—")
		+ kv("Type", s.typestr || String(s.type))
		+ kvRaw("PID", s.pid ? String(s.pid) : "—")
		+ kv("Since", since)
		+ kvRaw("Source", s.source ? `<span class="srclink" data-src="1" title="show frontend file">${esc(s.source)}</span>` : "—")
		+ kvRaw("Enabled", chip(!!s.enabled))
		+ kvRaw("Logger", chip(!!s.islog))
		+ kvRaw("Earlier", chip(!!s.earlier))
		+ kvRaw("Config", s.config ? `<span class="srclink" data-cfg="1" title="show configuration">view</span>` : "none")
		+ `</dl></div>`
		+ `<div class="dsection" id="d-frontend" style="display:none"><h3>Frontend file <span style="color:var(--ink-faint)" id="d-frontend-path"></span></h3><pre class="logtail" id="d-frontend-pre"></pre></div>`
		+ `<div class="dsection" id="d-config" style="display:none"><h3>Configuration</h3><pre class="logtail" id="d-config-pre"></pre></div>`
		+ `<div class="dsection"><h3>Dependency graph</h3><div class="deps">`
		+ depGroup("depends on", "→", s.depends, "svc", s.treename)
		+ depGroup("required by", "←", s.requiredby, "svc", s.treename)
		+ (s.provide && s.provide.length ? depGroup("provides", "⊕", s.provide, "svc", s.treename) : "")
		+ (s.conflict && s.conflict.length ? depGroup("conflicts", "⊗", s.conflict, "svc", s.treename) : "")
		+ `</div></div>`
		+ (s.contents && s.contents.length
			? `<div class="dsection"><h3>Contents <span style="color:var(--ink-faint)">(${s.contents.length})</span></h3><div class="dep-chips">${s.contents.map(n => depChip(n, "svc", s.treename)).join("")}</div></div>`
			: "")
		+ `<div class="dsection"><h3>Actions</h3>`
		+ actGrp("Operate", [["start", "go"], ["reload", ""], ["reconfigure", ""], ["enable", ""]], "svc", s.name, s.treename)
		+ actGrp("Destructive", [["restart", "danger"], ["stop", "danger"], ["disable", "danger"], ["free", "danger"], ["remove", "danger"]], "svc", s.name, s.treename)
		+ outputBlock()
		+ `</div>`
		+ `<div class="dsection"><h3>Log <span style="color:var(--ink-faint)">(last 5 lines)</span></h3><pre class="logtail" id="d-log">…</pre></div>`;

	fetchLog(s.name);
}

async function fetchLog(name) {
	const r = await api("GET", "/api/services/" + encodeURIComponent(name) + "/log");
	const pre = el("d-log");
	if (!pre || !state.selected || state.selected.kind !== "svc" || state.selected.name !== name) return;
	const log = (r.ok && r.body && r.body.log) ? r.body.log.replace(/\s+$/, "") : "";
	pre.textContent = log || "(no log)";
}

async function toggleFrontend() {
	const sec = el("d-frontend");
	if (!sec || !state.selected || state.selected.kind !== "svc") return;
	if (sec.style.display !== "none") { sec.style.display = "none"; return; }
	sec.style.display = "";
	el("d-frontend-pre").textContent = "…";
	const name = state.selected.name;
	const r = await api("GET", "/api/services/" + encodeURIComponent(name) + "/frontend");
	if (!state.selected || state.selected.name !== name) return;
	if (r.ok && r.body) {
		el("d-frontend-path").textContent = r.body.path || "";
		el("d-frontend-pre").textContent = r.body.content || "(empty)";
	} else {
		el("d-frontend-pre").textContent = "(unable to read frontend)";
	}
}

async function toggleConfig() {
	const sec = el("d-config");
	if (!sec || !state.selected || state.selected.kind !== "svc") return;
	if (sec.style.display !== "none") { sec.style.display = "none"; return; }
	sec.style.display = "";
	el("d-config-pre").textContent = "…";
	const name = state.selected.name;
	const r = await api("GET", "/api/services/" + encodeURIComponent(name) + "/config");
	if (!state.selected || state.selected.name !== name) return;
	const cfg = (r.ok && r.body && r.body.config) ? r.body.config.replace(/\s+$/, "") : "";
	el("d-config-pre").textContent = cfg || "(no configuration)";
}

/* ---------- tree drawer ---------- */
function openTreeDrawer(name) {
	const t = treeByName(name);
	if (!t) return;
	state.selected = { kind: "tree", name };
	showDrawer();
	el("d-name").textContent = t.name;
	el("d-sub").textContent = "tree";

	const st = treeStats(t.name);
	const health = treeHealth(t);
	const badgeCls = health === "up" ? "st-running" : health === "down" ? "st-failed" : "st-down";
	const badgeLbl = health === "up" ? "running" : health === "down" ? "degraded" : "inactive";
	const contents = servicesOf(t.name);
	const contentChips = contents.length
		? contents.map(s => depChip(s.name, "svc", t.name)).join("")
		: `<div class="dep-none">empty tree</div>`;
	const allowChips = (t.allow && t.allow.length)
		? t.allow.map(a => `<span class="dep-chip"><span class="dot" style="background:var(--amber)"></span>${esc(a)}</span>`).join("")
		: `<div class="dep-none">none</div>`;

	el("d-body").innerHTML =
		`<span class="big-badge ${badgeCls}"><span class="dot ${badgeCls}"></span>${badgeLbl}</span>`
		+ `<div class="dsection"><h3>Metadata</h3><dl class="kv">`
		+ kv("Group", t.group || "none")
		+ kvRaw("Current", chip(!!t.current))
		+ kvRaw("Enabled", chip(!!t.enabled))
		+ `</dl></div>`
		+ `<div class="dsection"><h3>Availability</h3><div class="tc-ratio">${miniBar(st)}<span>${st.running}/${st.settled} up · ${st.down} down · ${st.failed} failed${st.done ? ` · ${st.done} done` : ""}</span></div></div>`
		+ `<div class="dsection"><h3>Tree dependency graph</h3><div class="deps">`
		+ depGroup("depends on", "→", t.depends, "tree")
		+ depGroup("required by", "←", t.requiredby, "tree")
		+ `</div></div>`
		+ `<div class="dsection"><h3>Services in tree <span style="color:var(--ink-faint)">(${st.total})</span></h3><div class="dep-chips">${contentChips}</div></div>`
		+ `<div class="dsection"><h3>Permissions <span style="color:var(--ink-faint)">(allow)</span></h3><div class="dep-chips">${allowChips}</div></div>`
		+ `<div class="dsection"><h3>Actions</h3>`
		+ actGrp("Operate", [["start", "go"], ["stop", "danger"], ["free", "danger"], ["init", ""]], "tree", t.name)
		+ `<div class="impact">Start / Stop / Free cascade to the tree's ${st.total} service(s).</div>`
		+ actGrp("Provision", [["enable", ""], ["disable", ""], ["current", ""]], "tree", t.name)
		+ actGrp("Destructive", [["remove", "danger"]], "tree", t.name)
		+ outputBlock()
		+ `</div>`;
}

/* ---------- actions ---------- */
async function runAction(kind, name, tree, verb) {
	for (const btn of el("d-body").querySelectorAll(".btn")) btn.disabled = true;
	const base = kind === "tree" ? "/api/trees/" : "/api/services/";
	const r = await api("POST", base + encodeURIComponent(name) + "/" + encodeURIComponent(verb));
	const body = r.body || {};
	state.lastOutput = {
		ok: r.ok && body.exit === 0,
		text: (body.output && body.output.trim()) ? body.output : (r.ok ? `exit=${body.exit}` : `HTTP ${r.status}`),
	};
	toast(`${verb} ${name}: ${state.lastOutput.ok ? "ok" : "failed"}`, state.lastOutput.ok ? null : "err");
	await loadAll();
	if (kind === "tree") openTreeDrawer(name);
	else openServiceDrawer(name, tree);
}

/* ---------- toast ---------- */
let toastTimer = null;
function toast(msg, kind) {
	const t = el("toast");
	t.textContent = msg;
	t.className = "toast show" + (kind === "err" ? " err" : "");
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => t.classList.remove("show"), 2800);
}

/* ---------- SSE ---------- */
let reloadTimer = null;
function scheduleReload() {
	clearTimeout(reloadTimer);
	reloadTimer = setTimeout(async () => {
		await loadAll();
		if (state.selected) {
			if (state.selected.kind === "tree") openTreeDrawer(state.selected.name);
			else openServiceDrawer(state.selected.name, state.selected.tree);
		}
	}, 150);
}
function setLive(on) { el("live").dataset.on = on ? "1" : "0"; }
function startEvents() {
	if (state.es) state.es.close();
	const es = new EventSource("/api/events");
	state.es = es;
	es.onopen = () => setLive(1);
	es.onmessage = (ev) => {
		let m = null;
		try { m = JSON.parse(ev.data); } catch (_) { return; }
		if (m && m.type === "state_changed") scheduleReload();
	};
	es.onerror = () => setLive(0);
}

/* ---------- events ---------- */
function openFrom(node) {
	if (node.dataset.open === "tree") openTreeDrawer(node.dataset.name);
	else openServiceDrawer(node.dataset.name, node.dataset.tree);
}
el("wall").addEventListener("click", (e) => {
	if (state.mode === "graph" && graphClick(e)) return;
	const o = e.target.closest("[data-open]");
	if (o) openFrom(o);
});
// Graph clicks; returns 1 when handled.
function graphClick(e) {
	const t = e.target;
	const gs = t.closest("#gscope [data-g]");
	if (gs) { state.graphScope = gs.dataset.g; renderGraph(); return 1; }
	const gm = t.closest("#gmode [data-gm]");
	if (gm) { state.graphMode = gm.dataset.gm; renderGraph(); return 1; }
	const gd = t.closest("#gdir [data-gd]");
	if (gd) { state.graphDir = gd.dataset.gd; renderGraph(); return 1; }
	const gz = t.closest("[data-gz]");
	if (gz) {
		if (gz.dataset.gz === "fit") graphFit();
		else { Object.assign(state.graphXf, { k: 1, x: 30, y: 54 }); graphApply(); }
		return 1;
	}
	const det = t.closest(".gdet");
	if (det) { openFrom(det); return 1; }
	const fold = t.closest("[data-gfold]");
	if (fold) {
		const id = fold.dataset.gfold;
		if (state.graphOpen.has(id)) state.graphOpen.delete(id); else state.graphOpen.add(id);
		renderGraphNav();
		return 1;
	}
	const mod = t.closest("[data-gmod]");
	if (mod) { graphSetFocus(mod.dataset.gmod); return 1; }
	const pick = t.closest(".gnv.svc, .gchip, .gn");
	if (pick && state.graphScope === "services") { graphSetFocus(pick.dataset.k); return 1; }
	return 0;
}
el("wall").addEventListener("change", (e) => {
	if (e.target.id === "glog") { state.graphLogBadge = e.target.checked; renderGraph(); }
	else if (e.target.id === "gimplied") { state.graphImplied = e.target.checked; renderGraph(); }
});
el("wall").addEventListener("mouseover", (e) => { const n = e.target.closest(".gn"); if (n) graphHover(n, true); });
el("wall").addEventListener("mouseout", (e) => { const n = e.target.closest(".gn"); if (n) graphHover(n, false); });
el("wall").addEventListener("wheel", (e) => {
	const svg = e.target.closest("svg.gzoom"); if (!svg) return;
	e.preventDefault();
	const r = svg.getBoundingClientRect(), mx = e.clientX - r.left, my = e.clientY - r.top, t = state.graphXf;
	const k = Math.max(0.1, Math.min(4, t.k * (e.deltaY < 0 ? 1.12 : 1 / 1.12)));
	t.x = mx - (mx - t.x) * k / t.k;
	t.y = my - (my - t.y) * k / t.k;
	t.k = k;
	graphApply();
}, { passive: false });
let gDrag = null;
el("wall").addEventListener("pointerdown", (e) => {
	const svg = e.target.closest("svg.gzoom");
	if (!svg || e.target.closest(".gn")) return;
	gDrag = { x: e.clientX - state.graphXf.x, y: e.clientY - state.graphXf.y, svg };
	svg.classList.add("grabbing");
});
window.addEventListener("pointermove", (e) => {
	if (!gDrag) return;
	state.graphXf.x = e.clientX - gDrag.x;
	state.graphXf.y = e.clientY - gDrag.y;
	graphApply();
});
window.addEventListener("pointerup", () => {
	if (!gDrag) return;
	gDrag.svg.classList.remove("grabbing");
	gDrag = null;
});
window.addEventListener("resize", () => { if (state.mode === "graph") sizeGraph(); });
el("d-body").addEventListener("click", (e) => {
	const act = e.target.closest("[data-act]");
	if (act) { runAction(act.dataset.actkind, act.dataset.actname, act.dataset.acttree, act.dataset.act); return; }
	const src = e.target.closest("[data-src]");
	if (src) { toggleFrontend(); return; }
	const cfg = e.target.closest("[data-cfg]");
	if (cfg) { toggleConfig(); return; }
	const o = e.target.closest("[data-open]");
	if (o) openFrom(o);
});
el("modes").addEventListener("click", (e) => {
	const b = e.target.closest("[data-m]");
	if (!b) return;
	state.mode = b.dataset.m;
	for (const x of el("modes").children) x.classList.toggle("active", x === b);
	const showFilters = state.mode === "services";
	el("statusfilter").style.display = showFilters ? "" : "none";
	el("typefilter").style.display = showFilters ? "" : "none";
	el("filter").parentElement.style.display = "";
	el("filter").placeholder = state.mode === "trees" ? "filter trees…" : state.mode === "graph" ? "filter graph…" : state.mode === "available" ? "filter available…" : "filter services…";
	if (state.mode === "available") availCache = null;   // force a fresh scan on entering
	render();
});
el("statusfilter").addEventListener("click", (e) => {
	const b = e.target.closest("[data-f]");
	if (!b) return;
	state.activeStatus = b.dataset.f;
	renderVitals();
	renderWall();
});
el("typefilter").addEventListener("click", (e) => {
	const b = e.target.closest("[data-f]");
	if (!b) return;
	state.activeType = b.dataset.f;
	renderVitals();
	renderWall();
});
el("filter").addEventListener("input", (e) => {
	state.filter = e.target.value.trim().toLowerCase();
	if (state.mode === "graph") renderGraphNav();
	else render();
});
// Graph mode: Enter focuses the first service the filter keeps in the navigator.
el("filter").addEventListener("keydown", (e) => {
	if (e.key !== "Enter" || state.mode !== "graph") return;
	const first = el("wall").querySelector(".gnv.svc");
	if (!first) return;
	state.filter = ""; e.target.value = "";
	graphSetFocus(first.dataset.k);
});
el("refresh").addEventListener("click", () => { loadAll(); toast("refreshed"); });
el("scrim").addEventListener("click", closeDrawer);
el("d-close").addEventListener("click", closeDrawer);
document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeDrawer(); });

/* ---------- drawer resize ---------- */
(function initDrawerResize() {
	const handle = el("d-resize"), drawer = el("drawer");
	let dragging = false;
	handle.addEventListener("mousedown", (e) => {
		dragging = true;
		handle.classList.add("dragging");
		document.body.style.userSelect = "none";
		e.preventDefault();
	});
	document.addEventListener("mousemove", (e) => {
		if (!dragging) return;
		const w = Math.min(Math.max(window.innerWidth - e.clientX, 340), window.innerWidth * 0.95);
		drawer.style.width = w + "px";
		state.drawerWidth = Math.round(w);
	});
	document.addEventListener("mouseup", () => {
		if (!dragging) return;
		dragging = false;
		handle.classList.remove("dragging");
		document.body.style.userSelect = "";
	});
})();

/* ==================================================================
 * Service-creation form engine (data-driven, from doc/66-frontend.md).
 * The same createServiceForm(mount, opts) drives a standalone service
 * here and — in a later increment — each contained service of a module.
 * ================================================================== */
const createServiceForm = (function () {
	const ALL = ["classic", "oneshot", "module", "event"];
	const ENUMS = {
		Timestamp: ["tai", "iso", "none"],
		Flags: ["down", "earlier"],
		EventTypeSource: ["inotify", "schedule", "timer"],
		EventTypeReactor: ["service", "signal", "user", "inotify", "schedule", "timer"],
		Do: ["start", "stop", "restart", "reload", "reconfigure", "free"],
		Signal: ["SIGTERM", "SIGHUP", "SIGINT", "SIGQUIT", "SIGKILL", "SIGUSR1", "SIGUSR2",
			"SIGSTOP", "SIGCONT", "SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGALRM", "SIGABRT", "SIGPIPE", "SIGSEGV"],
	};
	const LIMITS = ["LimitAS", "LimitCORE", "LimitCPU", "LimitDATA", "LimitFSIZE", "LimitLOCKS",
		"LimitMEMLOCK", "LimitMSGQUEUE", "LimitNICE", "LimitNOFILE", "LimitNPROC", "LimitRTPRIO",
		"LimitRTTIME", "LimitSIGPENDING", "LimitSTACK"];
	const CAPS = ["CAP_CHOWN", "CAP_DAC_OVERRIDE", "CAP_DAC_READ_SEARCH", "CAP_FOWNER", "CAP_FSETID",
		"CAP_KILL", "CAP_SETGID", "CAP_SETUID", "CAP_SETPCAP", "CAP_LINUX_IMMUTABLE", "CAP_NET_BIND_SERVICE",
		"CAP_NET_BROADCAST", "CAP_NET_ADMIN", "CAP_NET_RAW", "CAP_IPC_LOCK", "CAP_IPC_OWNER", "CAP_SYS_MODULE",
		"CAP_SYS_RAWIO", "CAP_SYS_CHROOT", "CAP_SYS_PTRACE", "CAP_SYS_PACCT", "CAP_SYS_ADMIN", "CAP_SYS_BOOT",
		"CAP_SYS_NICE", "CAP_SYS_RESOURCE", "CAP_SYS_TIME", "CAP_SYS_TTY_CONFIG", "CAP_MKNOD", "CAP_LEASE",
		"CAP_AUDIT_WRITE", "CAP_AUDIT_CONTROL", "CAP_SETFCAP", "CAP_MAC_OVERRIDE", "CAP_MAC_ADMIN", "CAP_SYSLOG",
		"CAP_WAKE_ALARM", "CAP_BLOCK_SUSPEND", "CAP_AUDIT_READ", "CAP_PERFMON", "CAP_BPF", "CAP_CHECKPOINT_RESTORE",
		"CAP_PIDFD_OPEN", "CAP_PIDFD_GETFD", "CAP_PIDFD_SPAWN", "CAP_SETPROCTITLE", "CAP_MEMLOCK"];
	const STATEW = ["down", "starting", "up", "stopping", "finishing", "restarting", "done", "failed", "waiting"];
	const RESULTW = ["success", "exited", "signaled", "timeout-start", "timeout-stop", "crash-limit", "exec-failed"];
	const IO = {
		StdIn: { opts: ["66log", "null", "parent", "close", "tty"], path: ["tty"] },
		StdOut: { opts: ["66log", "file", "console", "null", "syslog", "parent", "close", "tty"], path: ["tty", "file", "66log"] },
		StdErr: { opts: ["inherit", "console", "null", "syslog", "parent", "close", "file", "tty"], path: ["tty", "file"] },
	};
	const CO = ["classic", "oneshot"], COM = ["classic", "oneshot", "module"];
	const SCHEMA = [
		{ name: "Main", always: true, fields: [
			{ k: "Description", w: "quote", types: ALL, basic: true },
			{ k: "Depends", w: "pick", src: "svc", types: COM, basic: true },
			{ k: "__log", w: "logtoggle", types: CO, basic: true },
			{ k: "Version", w: "line", types: ALL },
			{ k: "User", w: "bracket", types: ALL, ph: "root · user · obarun" },
			{ k: "RequiredBy", w: "pick", src: "svc", types: COM },
			{ k: "OptsDepends", w: "pick", src: "svc", types: COM },
			{ k: "Provide", w: "bracket", types: COM },
			{ k: "Conflict", w: "pick", src: "svc", types: COM },
			{ k: "CopyFrom", w: "bracket", types: COM, ph: "/path/to/dir" },
			{ k: "Flags", w: "flags", e: "Flags", types: COM },
			{ k: "InTree", w: "line", types: ALL, ph: "global" },
			{ k: "EventType", w: "enum", e: "EventTypeSource", types: ["event"], req: true, basic: true },
			{ k: "Watch", w: "line", types: ["event"], when: ["inotify"], req: true, basic: true, ph: "/absolute/path" },
			{ k: "On", w: "bracket", types: ["event"], when: ["inotify"], req: true, basic: true, ph: "IN_CLOSE_WRITE IN_MODIFY" },
			{ k: "Expression", w: "quote", types: ["event"], when: ["schedule"], req: true, basic: true, ph: "0 30 * * * ?" },
			{ k: "Timezone", w: "line", types: ["event"], when: ["schedule"], ph: "UTC" },
			{ k: "Every", w: "line", types: ["event"], when: ["timer"], req: true, basic: true, ph: "30s · 5m · 1h" },
		]},
		{ name: "Start", types: CO, always: true, fields: [
			{ k: "Execute", w: "exec", req: true, basic: true },
			{ k: "RunAs", w: "line", ph: "user · 1000:19 · :19" },
			{ k: "Timeout", w: "uint" }, { k: "Notify", w: "uint" },
			{ k: "MaxDeath", w: "uint", ph: "5" }, { k: "MaxDeathInterval", w: "uint", ph: "30000" },
		]},
		{ name: "Stop", types: CO, fields: [
			{ k: "Execute", w: "exec" }, { k: "RunAs", w: "line" }, { k: "Timeout", w: "uint" },
			{ k: "DownSignal", w: "enum", e: "Signal" },
		]},
		{ name: "Logger", types: CO, gate: "log", fields: [
			{ k: "Backup", w: "uint", ph: "3" }, { k: "MaxSize", w: "uint", ph: "1000000" },
			{ k: "Timestamp", w: "enum", e: "Timestamp" }, { k: "Timeout", w: "uint" },
			{ k: "RunAs", w: "line" }, { k: "Execute", w: "exec" },
		]},
		{ name: "Environment", types: COM, fields: [{ k: "__env", w: "keyval" }] },
		{ name: "Regex", types: ["module"], fields: [
			{ k: "Configure", w: "quote", basic: true, ph: "@I" },
			{ k: "InFiles", w: "bracket", basic: true, ph: ":server:KEY=val ::KEY=@I" },
			{ k: "Directories", w: "bracket", ph: "DM=sddm" }, { k: "Files", w: "bracket", ph: "GENERIC=@I" },
		]},
		{ name: "Execute", types: CO, fields: [
			{ k: "BlockPrivileges", w: "line", ph: "true · false" }, { k: "UMask", w: "line", ph: "0022" },
			{ k: "Nice", w: "uint", min: -20, max: 19, ph: "-20 … 19" }, { k: "ChangeDirectory", w: "line", ph: "/path" },
			{ k: "CapsBound", w: "pick", src: "caps" }, { k: "CapsAmbient", w: "pick", src: "caps" },
			{ k: "StdIn", w: "io" }, { k: "StdOut", w: "io" }, { k: "StdErr", w: "io" },
			...LIMITS.map(l => ({ k: l, w: "uint", ph: "a number ≥ 0" })),
		]},
		{ name: "Event", types: COM,
			hint: "Do or Emit required · service/signal: From + (On XOR OnAll) · user: On, no From/OnAll · inotify/schedule/timer: From, no On/OnAll",
			fields: [
			{ k: "EventType", w: "enum", e: "EventTypeReactor", req: true },
			{ k: "From", w: "pick", src: "svc", when: ["service", "signal", "inotify", "schedule", "timer"], whenKey: "Event.EventType", req: true },
			{ k: "On", w: "on", when: ["service", "signal", "user"], whenKey: "Event.EventType" },
			{ k: "OnAll", w: "on", when: ["service", "signal"], whenKey: "Event.EventType" },
			{ k: "Do", w: "enum", e: "Do" }, { k: "Emit", w: "line" },
		]},
	];

	const q = v => '"' + v + '"';
	const brk = v => "( " + v.trim().split(/\s+/).filter(Boolean).join(" ") + " )";
	function execBlock(cmd) {
		if (!cmd.trim()) return "( )";
		if (cmd.includes("\n")) return "(\n" + cmd.split("\n").map(l => "    " + l).join("\n") + "\n)";
		return "( " + cmd.trim() + " )";
	}

	function createServiceForm(mount, opts) {
		opts = opts || {};
		const types = opts.types || ALL;
		const advanced = opts.advanced || (() => false);
		const extraBasic = new Set(opts.extraBasic || []);
		const omit = new Set(opts.omit || []);   // "Sec.Key" ids rendered in the form but kept out of the emitted frontend
		const dependsOptions = opts.dependsOptions || (() => []);
		const onChange = opts.onChange || (() => {});
		const V = Object.assign({ __type: types[0], "Main.EventType": "inotify", "__log": true }, opts.initial || {});
		const type = () => V.__type;

		function fieldApplies(sec, f) {
			const t = f.types || sec.types || ALL;
			if (!t.includes(type())) return false;
			if (f.when && !f.when.includes(V[f.whenKey || "Main.EventType"])) return false;
			if (sec.gate && !V["__" + sec.gate]) return false;
			if (!f.basic && !extraBasic.has(sec.name + "." + f.k) && !advanced()) return false;
			return true;
		}
		function sectionApplies(sec) {
			if (sec.types && !sec.types.includes(type())) return false;
			if (sec.gate && !V["__" + sec.gate]) return false;
			return sec.always || sec.fields.some(f => fieldApplies(sec, f));
		}
		function widget(sec, f) {
			const id = sec.name + "." + f.k, v = V[id], req = f.req ? '<span class="req">●</span>' : '';
			if (f.k === "__log") return `<label class="chk"><input type="checkbox" data-struct="1" data-id="__log" ${V["__log"] ? "checked" : ""}> supervised logger — <code>Options = ( log )</code></label>`;
			if (f.w === "exec") return `<div class="field"><label>${f.k} ${req}</label><textarea data-id="${id}" placeholder="${f.ph || ""}">${esc(v || "")}</textarea></div>`;
			if (f.w === "keyval") return `<div class="field"><label>Environment</label><textarea data-id="${id}" placeholder="KEY=value&#10;RUNDIR=!/run/app">${esc(v || "")}</textarea><span class="hint">one <code>KEY=value</code> per line · leading <code>!</code> = start-time only</span></div>`;
			if (f.w === "enum") {
				let list = ENUMS[f.e].slice();
				if (f.e === "Do" && type() === "oneshot") list = list.filter(x => x !== "reload");
				const blank = `<option value="" ${v ? "" : "selected"}>(unset)</option>`;
				const st = (id === "Main.EventType" || id === "Event.EventType") ? 'data-struct="1"' : "";
				return `<div class="field"><label>${f.k} ${req}</label><select ${st} data-id="${id}">${blank + list.map(o => `<option ${o === v ? "selected" : ""}>${o}</option>`).join("")}</select></div>`;
			}
			if (f.w === "flags") {
				const cur = new Set(V[id] || []);
				return `<div class="field"><label>${f.k}</label><div class="flags">${ENUMS[f.e].map(o => `<label class="chk"><input type="checkbox" data-flag="${id}" value="${o}" ${cur.has(o) ? "checked" : ""}> ${o}</label>`).join("")}</div></div>`;
			}
			if (f.w === "pick") {
				const sel = V[id] || [], all = f.src === "caps" ? CAPS : dependsOptions();
				const tags = sel.length ? sel.map(s => `<span class="dtag">${esc(s)}<button data-pickrm="${esc(s)}" data-pickid="${id}" type="button">✕</button></span>`).join("") : '<span class="hint">none</span>';
				const opts2 = '<option value="">+ add…</option>' + all.filter(s => !sel.includes(s)).map(s => `<option>${esc(s)}</option>`).join("");
				const hint = f.src === "caps" ? "valid capability names" : (opts.member ? "local services only (isolation)" : "available services");
				return `<div class="field"><label>${f.k} ${req}</label><div class="dtags">${tags}</div><select data-pickadd="${id}">${opts2}</select><span class="hint">${hint}</span></div>`;
			}
			if (f.w === "io") {
				const io = IO[f.k], tv = V[id] || "", pv = V[id + ".__path"] || "";
				const o = '<option value="">(default)</option>' + io.opts.map(x => `<option ${x === tv ? "selected" : ""}>${x}</option>`).join("");
				const pth = io.path.includes(tv) ? `<input type="text" data-id="${id}.__path" value="${esc(pv)}" placeholder="${tv === "66log" ? "/custom/log/dir (optional)" : "/absolute/path"}">` : "";
				return `<div class="field"><label>${f.k}</label><select data-struct="1" data-id="${id}">${o}</select>${pth}<span class="hint">tty/file take a <code>:path</code></span></div>`;
			}
			if (f.w === "on") {
				const et = V["Event.EventType"], sel = V[id] || [];
				const tags = sel.length ? sel.map(s => `<span class="dtag">${esc(s)}<button data-pickrm="${esc(s)}" data-pickid="${id}" type="button">✕</button></span>`).join("") : '<span class="hint">none</span>';
				let build = "", hint = "";
				if (et === "service") {
					const froms = V["Event.From"] || [];
					const svcOpts = '<option value="">(bare)</option>' + froms.map(s => `<option>${esc(s)}</option>`).join("");
					const condOpts = STATEW.concat(RESULTW).map(w => `<option>${esc(w)}</option>`).join("");
					build = `<div class="onbuild"><select data-onsvc="${id}">${svcOpts}</select><span class="sep">:</span><select data-oncond="${id}">${condOpts}</select><button class="btn" data-onadd="${id}" type="button">add</button></div>`
						+ `<div class="onbuild"><input type="text" data-onfree="${id}" placeholder="or type an advanced token: exited:3 · db:signaled:SIGKILL"><button class="btn" data-onaddfree="${id}" type="button">add</button></div>`;
					hint = "&lt;from-service&gt;:&lt;state&gt; (svc must be a From member) or a bare state/result word";
				} else if (et === "signal") {
					const opts2 = '<option value="">+ add signal…</option>' + ENUMS.Signal.filter(s => !sel.includes(s)).map(s => `<option>${esc(s)}</option>`).join("");
					build = `<select data-onsig="${id}">${opts2}</select>`;
					hint = "signal names routed to the From services";
				} else {
					build = `<div class="onbuild"><input type="text" data-onfree="${id}" placeholder="event name (e.g. cert-renewed)"><button class="btn" data-onaddfree="${id}" type="button">add</button></div>`;
					hint = "free-form event names · lifecycle: boot.done, boot.failed, shutdown.begin";
				}
				return `<div class="field"><label>${f.k} ${req}</label><div class="dtags">${tags}</div>${build}<span class="hint">${hint}</span></div>`;
			}
			if (f.w === "uint") {
				const min = (f.min != null) ? f.min : 0, max = (f.max != null) ? ` max="${f.max}"` : "";
				return `<div class="field"><label>${f.k} ${req}</label><input type="number" min="${min}"${max} data-id="${id}" value="${esc(v == null ? "" : v)}" placeholder="${f.ph || ""}"></div>`;
			}
			return `<div class="field"><label>${f.k} ${req}</label><input type="text" data-id="${id}" value="${esc(v == null ? "" : v)}" placeholder="${f.ph || ""}"></div>`;
		}
		function render() {
			let h = "";
			if (opts.showName !== false) {
				h += `<div class="field"><label>${opts.nameLabel || "name"} <span class="req">●</span></label>`
					+ `<input type="text" data-id="__name" value="${esc(V.__name || "")}" placeholder="${opts.namePh || "myapp"}">`
					+ (opts.nameHint ? `<span class="hint">${opts.nameHint}</span>` : "") + `</div>`;
			}
			if (types.length > 1) {
				h += `<div class="field"><label>type <span class="req">●</span></label><div class="segmented tseg">`
					+ types.map(t => `<button data-t="${t}" class="${t === type() ? "active" : ""}" type="button">${t}</button>`).join("") + `</div></div>`;
			}
			for (const sec of SCHEMA) {
				if (!sectionApplies(sec)) continue;
				const vis = sec.fields.filter(f => fieldApplies(sec, f));
				if (!vis.length && !sec.always) continue;
				const secHint = sec.hint ? `<span class="hint" style="margin-bottom:.3em">${sec.hint}</span>` : "";
				h += `<div class="sec-block"><div class="sec-title">[${sec.name}]</div>${secHint}${vis.map(f => widget(sec, f)).join("")}</div>`;
			}
			mount.innerHTML = h;
		}
		function genFrontend() {
			let out = "";
			for (const sec of SCHEMA) {
				if (!sectionApplies(sec)) continue;
				let lines = sec.name === "Main" ? ["Type = " + type()] : [];
				for (const f of sec.fields) {
					if (!fieldApplies(sec, f)) continue;
					const id = sec.name + "." + f.k, v = V[id];
					if (omit.has(id)) continue;   // shown in the form, kept out of the frontend (routed elsewhere)
					if (f.k === "__log") { if (!V["__log"]) lines.push("Options = ( !log )"); continue; }
					if (f.w === "keyval") { (v || "").split("\n").map(l => l.trim()).filter(l => l.includes("=")).forEach(l => lines.push(l)); continue; }
					if (f.w === "pick" || f.w === "flags" || f.w === "on") { const a = V[id] || []; if (a.length) lines.push(f.k + " = ( " + a.join(" ") + " )"); continue; }
					if (f.w === "io") { const t = V[id]; if (t) { const p = V[id + ".__path"]; lines.push(f.k + " = " + ((IO[f.k].path.includes(t) && p) ? (t + ":" + p) : t)); } continue; }
					if (v == null || v === "") continue;
					if (f.w === "exec") lines.push("Execute = " + execBlock(v));
					else if (f.w === "quote") lines.push(f.k + " = " + q(v));
					else if (f.w === "bracket") lines.push(f.k + " = " + brk(v));
					else lines.push(f.k + " = " + v);
				}
				if (!lines.length && !sec.always) continue;
				if (out) out += "\n";
				out += "[" + sec.name + "]\n" + lines.join("\n") + "\n";
			}
			return out;
		}
		mount.addEventListener("input", e => {
			const t = e.target;
			if (t.dataset.flag) { const a = new Set(V[t.dataset.flag] || []); t.checked ? a.add(t.value) : a.delete(t.value); V[t.dataset.flag] = [...a]; onChange(t.dataset.flag); return; }
			if (t.dataset.id != null) { V[t.dataset.id] = (t.type === "checkbox") ? t.checked : t.value; if (t.dataset.struct) render(); onChange(t.dataset.id); }
		});
		mount.addEventListener("change", e => {
			const t = e.target;
			if (t.dataset.pickadd && t.value) { V[t.dataset.pickadd] = [...(V[t.dataset.pickadd] || []), t.value]; render(); onChange(t.dataset.pickadd); return; }
			if (t.dataset.onsig && t.value) { V[t.dataset.onsig] = [...(V[t.dataset.onsig] || []), t.value]; render(); onChange(t.dataset.onsig); return; }
			if (t.dataset.struct) { V[t.dataset.id] = (t.type === "checkbox") ? t.checked : t.value; render(); onChange(t.dataset.id); }
		});
		mount.addEventListener("click", e => {
			const rm = e.target.closest("[data-pickrm]");
			if (rm) { const id = rm.dataset.pickid; V[id] = (V[id] || []).filter(s => s !== rm.dataset.pickrm); render(); onChange(id); return; }
			const oa = e.target.closest("[data-onadd]");
			if (oa) { const id = oa.dataset.onadd, sv = mount.querySelector('[data-onsvc="' + id + '"]').value, cd = mount.querySelector('[data-oncond="' + id + '"]').value;
				if (cd) { V[id] = [...(V[id] || []), sv ? sv + ":" + cd : cd]; render(); onChange(id); } return; }
			const of = e.target.closest("[data-onaddfree]");
			if (of) { const id = of.dataset.onaddfree, inp = mount.querySelector('[data-onfree="' + id + '"]'), val = (inp.value || "").trim();
				if (val) { V[id] = [...(V[id] || []), val]; render(); onChange(id); } return; }
			const b = e.target.closest(".tseg [data-t]");
			if (b) { V.__type = b.dataset.t; render(); onChange("__type"); }
		});
		function reset() {
			for (const k of Object.keys(V)) delete V[k];
			Object.assign(V, { __type: types[0], "Main.EventType": "inotify", "__log": true }, opts.initial || {});
			render();
		}
		render();
		return { getFrontend: genFrontend, getState: () => V, name: () => V.__name || "", type: () => V.__type, rerender: render, reset };
	}
	return createServiceForm;
})();

/* ---------- create view controller (service + module) ---------- */
let cForm = null, cAdv = false, cMode = "service";
let cmFront = null, cmMembers = [], cmSeq = 0;

function cFrontHl(text) {
	return text.split("\n").map(line => {
		if (/^\[.*\]$/.test(line)) return '<span class="hl-sec">' + esc(line) + '</span>';
		if (line.trim().startsWith("#")) return '<span class="hl-cmt">' + esc(line) + '</span>';
		const m = line.match(/^([A-Za-z][\w-]*)(\s*=\s*)(.*)$/);
		if (m) return '<span class="hl-key">' + esc(m[1]) + '</span>' + esc(m[2]) + '<span class="hl-val">' + esc(m[3]) + '</span>';
		return esc(line);
	}).join("\n");
}
function cInTree(front) { const m = front.match(/^InTree = (.+)$/m); return m ? m[1].trim() : ""; }
function cActionOf() { const e = document.querySelector('input[name="c-act"]:checked'); return e ? e.value : "enablestart"; }
function cCommand(name, inTree, act) {
	if (act === "none") return "frontend written — not enabled/started";
	if (act === "start") return "66 start " + name;
	const t = inTree ? "" : ("-t " + el("c-tree").value + " ");
	const S = act === "enablestart" ? "-S " : "";
	return "66 " + t + "enable " + S + name;
}
/* Replace a mount node with a fresh clone so a re-instantiated form does not
 * stack duplicate event listeners on the persistent element. */
function freshMount(id) { const o = el(id); const n = o.cloneNode(false); o.parentNode.replaceChild(n, o); return n; }

function activeFrontend() { return cMode === "module" ? (cmFront ? cmFront.getFrontend() : "") : (cForm ? cForm.getFrontend() : ""); }
function activeName() {
	if (cMode === "module") {
		const n = cmName() || "module";
		const inst = el("cm-instance").value.trim();
		return inst ? (n + "@" + inst) : (n + "@…");
	}
	return cForm ? (cForm.name() || "service") : "service";
}
function refreshFooter() {
	const front = activeFrontend();
	const inTree = cInTree(front), act = cActionOf();
	const usesTree = (act === "enable" || act === "enablestart") && !inTree;
	el("c-tree").disabled = !usesTree;
	el("c-treehint").textContent = inTree ? ("InTree = " + inTree + " → used at enable") : (usesTree ? "" : "no tree needed for this action");
	el("c-cmd").innerHTML = "next · <b>" + esc(cCommand(activeName(), inTree, act)) + "</b>";
	const p = cMode === "module" ? ((cmName() || "module") + "/") : (cForm ? cForm.name() || "service" : "service");
	el("c-path").textContent = "→ ~/.66/service/" + p + "  (user)  ·  /etc/66/service/" + p + "  (root)";
}
function refreshCreate() {
	if (cForm) el("c-prev").innerHTML = cFrontHl(cForm.getFrontend());
	refreshFooter();
}

/* ----- module editor ----- */
function cmLocalNames(exceptId) { return cmMembers.filter(m => m.id !== exceptId).map(m => m.api.name()).filter(Boolean); }
function cmAddMember(init, activated) {
	const id = ++cmSeq;
	const wrap = document.createElement("div");
	wrap.className = "member";
	wrap.innerHTML = '<div class="member-bar"><span class="mi">frontend/</span>'
		+ `<label class="chk"><input type="checkbox" data-act="${id}" ${activated ? "checked" : ""}> always-on</label>`
		+ `<span class="grow" style="flex:1"></span><button class="btn sm danger" data-rm="${id}" type="button">remove</button></div>`
		+ '<div class="member-mount"></div>';
	el("cm-members").appendChild(wrap);
	const api = createServiceForm(wrap.querySelector(".member-mount"), {
		member: true, types: ["classic", "oneshot", "event"], advanced: () => cAdv,
		dependsOptions: () => cmLocalNames(id),
		nameLabel: "local name", namePh: "server",
		initial: init || { __type: "classic" },
		onChange: (kind) => { refreshModule(); if (kind === "__name") cmMembers.forEach(m => { if (m.id !== id) m.api.rerender(); }); },
	});
	cmMembers.push({ id, api, activated: !!activated, wrap });
	refreshModule();
}
function cmTree() {
	const nm = cmName() || "module";
	const st = cmFront ? cmFront.getState() : {};
	const act = cmMembers.filter(m => m.activated).map(m => m.api.name() || "?");
	const deps = st["Main.Depends"] || [], req = st["Main.RequiredBy"] || [];
	const hasCfg = el("cm-configure").value.trim().length > 0;
	let t = `<b>${esc(nm)}/</b>\n├── <span class="f">${esc(nm)}@</span>            <span class="c"># module frontend</span>\n├── <b>frontend/</b>\n`;
	cmMembers.forEach((m, i) => { t += `│   ${i === cmMembers.length - 1 ? "└──" : "├──"} <span class="f">${esc(m.api.name() || "?")}</span>\n`; });
	if (hasCfg) t += '├── <b>configure/</b>\n│   └── <span class="f">configure</span>\n';
	t += '└── <b>activated/</b>\n';
	const rows = [];
	act.forEach(a => rows.push({ leaf: a }));
	if (deps.length) rows.push({ dir: "depends", items: deps });
	if (req.length) rows.push({ dir: "requiredby", items: req });
	if (!rows.length) { t += '    <span class="c">(configure decides at parse time)</span>\n'; return t; }
	rows.forEach((r, i) => {
		const last = i === rows.length - 1, br = last ? "└──" : "├──";
		if (r.leaf) { t += `    ${br} <span class="f">${esc(r.leaf)}</span>  <span class="c"># empty</span>\n`; return; }
		t += `    ${br} <b>${esc(r.dir)}/</b>\n`;
		const pad = last ? "        " : "    │   ";
		r.items.forEach((it, j) => { t += `${pad}${j === r.items.length - 1 ? "└──" : "├──"} <span class="f">${esc(it)}</span>\n`; });
	});
	return t;
}
function cmFileblk(name, content) { return `<div class="fileblk"><div class="fh">${esc(name)}</div><pre>${cFrontHl(content)}</pre></div>`; }
function refreshModule() {
	if (!cmFront) return;
	const nm = cmName() || "module";
	let h = `<div class="tree">${cmTree()}</div>`;
	h += cmFileblk(nm + "@", cmFront.getFrontend());
	cmMembers.forEach(m => h += cmFileblk("frontend/" + (m.api.name() || "?"), m.api.getFrontend()));
	const cfg = el("cm-configure").value;
	if (cfg.trim()) h += cmFileblk("configure/configure", cfg);
	el("cm-prev").innerHTML = h;
	refreshFooter();
}
function cmName() { return el("cm-name").value.trim(); }
function buildModule() {
	cmMembers = []; el("cm-members").innerHTML = ""; el("cm-instance").value = ""; el("cm-name").value = "";
	el("cm-configure").value = '#!/usr/bin/bash\n# $1 = [Regex] Configure value (the instance name)\ninstance="$1"\n';
	cmFront = createServiceForm(freshMount("cm-front"), {
		types: ["module"], advanced: () => cAdv, extraBasic: ["Environment.__env"],
		omit: ["Main.Depends", "Main.RequiredBy"],   // routed to activated/depends|requiredby dirs
		showName: false,
		dependsOptions: () => state.services.map(s => s.name),
		initial: { "Main.Description": "" },
		onChange: refreshModule,
	});
	refreshModule();
}

/* ----- modal ----- */
let cModalActions = {};
function closeModal() { el("c-modal").hidden = true; el("c-modal-foot").innerHTML = ""; cModalActions = {}; }
function showModal(title, titleCls, bodyText, buttons) {
	el("c-modal-title").textContent = title;
	el("c-modal-title").className = titleCls || "";
	el("c-modal-body").textContent = bodyText || "";
	cModalActions = {};
	el("c-modal-foot").innerHTML = buttons.map((b, i) => {
		cModalActions["b" + i] = b.onClick;
		return `<button class="btn ${b.cls || (b.primary ? "primary" : "")}" data-mb="b${i}" type="button">${esc(b.label)}</button>`;
	}).join("");
	el("c-modal").hidden = false;
}
function retryOverwrite() { el("c-overwrite").checked = true; closeModal(); doCreate(); }

/* ----- open / close / mode ----- */
function setMode(m) {
	cMode = m;
	for (const b of el("c-mode").children) b.classList.toggle("active", b.dataset.cm === m);
	el("c-title").textContent = m === "module" ? "New module" : "New service";
	el("c-body-service").hidden = (m === "module");
	el("c-body-module").hidden = (m !== "module");
	if (m === "module") { if (!cmFront) buildModule(); else refreshModule(); }
	else refreshCreate();
}
function openCreate() {
	document.body.classList.add("creating");
	el("createview").setAttribute("aria-hidden", "false");
	const names = state.trees.map(t => t.name);
	el("c-tree").innerHTML = (names.length ? names : ["global"]).map(n => `<option>${esc(n)}</option>`).join("");
	el("c-adv").checked = false; cAdv = false;
	el("c-overwrite").checked = false;
	closeModal();
	cmFront = null; cmMembers = []; el("cm-members").innerHTML = "";
	cForm = createServiceForm(freshMount("c-form"), {
		types: ["classic", "oneshot", "event"],
		advanced: () => cAdv,
		dependsOptions: () => state.services.map(s => s.name),
		nameHint: 'instance template <code>name@</code> · instance <code>name@tag</code>',
		initial: { __name: "", "Main.Description": "", "Start.Execute": "" },
		onChange: refreshCreate,
	});
	setMode("service");
}
function closeCreate() {
	document.body.classList.remove("creating");
	el("createview").setAttribute("aria-hidden", "true");
	closeModal();
	cForm = null; cmFront = null; cmMembers = [];
}

/* ----- create ----- */
function doCreate() { return cMode === "module" ? doCreateModule() : doCreateService(); }
function createResult(r, data, kind, name) {
	if (r.status === 201) { toast(kind + " “" + name + "” created"); closeCreate(); loadAll(); return; }
	if (r.status === 503) {
		showModal("66 could not enable the " + kind, "err",
			(data.output || "(no output)") + "\n\nThe " + kind + " was written but not enabled. Fix it, then create again — or overwrite.",
			[{ label: "Overwrite & retry", primary: true, onClick: retryOverwrite }, { label: "Close", onClick: closeModal }]);
	} else if (r.status === 409) {
		showModal("Already exists", "warn", (data.error || "") + "\nReplace it to overwrite.",
			[{ label: "Replace existing", primary: true, onClick: retryOverwrite }, { label: "Cancel", onClick: closeModal }]);
	} else {
		showModal("Create failed", "err", data.error || ("request failed (" + r.status + ")"), [{ label: "Close", onClick: closeModal }]);
	}
}
async function post(url, body, kind, name) {
	el("c-create").disabled = true;
	try {
		const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json", ...API_HEADERS }, body: JSON.stringify(body) });
		const data = await r.json().catch(() => ({}));
		createResult(r, data, kind, name);
	} catch (e) {
		showModal("Network error", "err", String(e), [{ label: "Close", onClick: closeModal }]);
	} finally {
		el("c-create").disabled = false;
	}
}
function doCreateService() {
	if (!cForm) return;
	const name = cForm.name().trim();
	if (!name) { showModal("Missing name", "err", "The service needs a name.", [{ label: "Close", onClick: closeModal }]); return; }
	const act = cActionOf(), content = cForm.getFrontend(), inTree = cInTree(content);
	const usesTree = (act === "enable" || act === "enablestart") && !inTree;
	const body = { type: cForm.type(), name, content, action: act };
	if (usesTree) body.tree = el("c-tree").value;
	if (el("c-overwrite").checked) body.overwrite = true;
	post("/api/services", body, "service", name);
}
function doCreateModule() {
	if (!cmFront) return;
	const name = cmName();
	if (!name) { showModal("Missing name", "err", "The module needs a name.", [{ label: "Close", onClick: closeModal }]); return; }
	const act = cActionOf(), instance = el("cm-instance").value.trim();
	if (act !== "none" && !instance) {
		showModal("Instance required", "warn", "Enabling or starting a module needs an instance name (e.g. blog).", [{ label: "Close", onClick: closeModal }]);
		return;
	}
	const st = cmFront.getState(), content = cmFront.getFrontend(), inTree = cInTree(content);
	const usesTree = (act === "enable" || act === "enablestart") && !inTree;
	const members = cmMembers.map(m => ({ local: m.api.name().trim(), content: m.api.getFrontend() })).filter(m => m.local);
	const activated = cmMembers.filter(m => m.activated && m.api.name().trim()).map(m => m.api.name().trim());
	const body = {
		name, content, members, activated, action: act,
		depends: st["Main.Depends"] || [],
		requiredby: st["Main.RequiredBy"] || [],
		configure: el("cm-configure").value.trim() ? el("cm-configure").value : "",
	};
	if (act !== "none") body.instance = instance;
	if (usesTree) body.tree = el("c-tree").value;
	if (el("c-overwrite").checked) body.overwrite = true;
	post("/api/modules", body, "module", name);
}

/* ----- wiring ----- */
el("newbtn").addEventListener("click", openCreate);
el("c-close").addEventListener("click", closeCreate);
el("c-mode").addEventListener("click", (e) => { const b = e.target.closest("[data-cm]"); if (b) setMode(b.dataset.cm); });
el("c-reset").addEventListener("click", () => {
	if (cMode === "module") { if (cmFront) cmFront.reset(); cmMembers.forEach(m => m.api.reset()); refreshModule(); }
	else { if (cForm) cForm.reset(); refreshCreate(); }
	closeModal();
});
el("c-modal-x").addEventListener("click", closeModal);
el("c-modal").addEventListener("click", (e) => { if (e.target === el("c-modal")) closeModal(); });
el("c-modal-foot").addEventListener("click", (e) => { const b = e.target.closest("[data-mb]"); if (b && cModalActions[b.dataset.mb]) cModalActions[b.dataset.mb](); });
el("c-adv").addEventListener("change", (e) => {
	cAdv = e.target.checked;
	if (cForm) cForm.rerender();
	if (cmFront) cmFront.rerender();
	cmMembers.forEach(m => m.api.rerender());
	cMode === "module" ? refreshModule() : refreshCreate();
});
el("c-tree").addEventListener("change", refreshFooter);
el("c-actions").addEventListener("change", refreshFooter);
el("c-create").addEventListener("click", doCreate);
el("cm-add").addEventListener("click", () => cmAddMember({ __type: "classic", __name: "" }, false));
el("cm-configure").addEventListener("input", refreshModule);
el("cm-name").addEventListener("input", refreshModule);
el("cm-instance").addEventListener("input", refreshFooter);
el("cm-members").addEventListener("change", (e) => {
	const a = e.target.closest("[data-act]"); if (!a) return;
	const m = cmMembers.find(x => x.id == a.dataset.act); if (m) { m.activated = a.checked; refreshModule(); }
});
el("cm-members").addEventListener("click", (e) => {
	const r = e.target.closest("[data-rm]"); if (!r) return;
	const idx = cmMembers.findIndex(x => x.id == r.dataset.rm); if (idx < 0) return;
	cmMembers[idx].wrap.remove();
	cmMembers.splice(idx, 1);
	cmMembers.forEach(m => m.api.rerender());
	refreshModule();
});
document.addEventListener("keydown", (e) => {
	if (e.key !== "Escape") return;
	if (!el("c-modal").hidden) closeModal();
	else if (document.body.classList.contains("creating")) closeCreate();
});

/* ---------- init ---------- */
loadAll();
startEvents();
