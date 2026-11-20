import './style.css';

type LogFile = { name: string; path: string; size: number; sha: string };
type Upload = { id: string; date: string; path: string; files: LogFile[]; bytes: number };
type DashboardData = { uploads: Upload[]; days: { date: string; uploads: number; bytes: number }[]; totalBytes: number; dedupeSavedBytes: number; dedupeSavedFiles: number };
const app = document.querySelector<HTMLDivElement>('#app')!;
let data: DashboardData = { uploads: [], days: [], totalBytes: 0, dedupeSavedBytes: 0, dedupeSavedFiles: 0 };
let sortMode: "newest" | "largest" | "smallest" = "newest";
let online = false;
let search = '';
let selected: string | null = null;
let refreshTimer = 0;

const icon = (name: string) => {
  const paths: Record<string, string> = {
    grid: '<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
    logs: '<path d="M8 6h13M8 12h13M8 18h13"/><path d="M3 6h.01M3 12h.01M3 18h.01"/>',
    activity: '<path d="M3 12h4l3-8 4 16 3-8h4"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-4-4"/>',
    download: '<path d="M12 3v12m-5-5 5 5 5-5"/><path d="M5 20h14"/>',
    chevron: '<path d="m9 18 6-6-6-6"/>',
    close: '<path d="m18 6-12 12M6 6l12 12"/>',
    refresh: '<path d="M20 7v5h-5M4 17v-5h5"/><path d="M5.6 9a7 7 0 0 1 11.5-2L20 12M4 12l2.9 5a7 7 0 0 0 11.5-2"/>',
    file: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6M8 13h8M8 17h8"/>'
  };
  return `<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${paths[name] ?? ''}</svg>`;
};
const bytes = (n: number) => n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1048576).toFixed(2)} MB`;
const dateLabel = (date: string) => new Date(date + 'T12:00:00').toLocaleDateString(undefined, { month: 'short', day: 'numeric', year: 'numeric' });
const escapeHtml = (s: string) => s.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);

function chart() {
  const byDate = new Map(data.days.map(day => [day.date, day]));
  const values = Array.from({ length: 14 }, (_, i) => {
    const date = new Date();
    date.setHours(12, 0, 0, 0);
    date.setDate(date.getDate() - (13 - i));
    const key = date.toISOString().slice(0, 10);
    return byDate.get(key) ?? { date: key, uploads: 0, bytes: 0 };
  });
  const max = Math.max(1, ...values.map(d => d.uploads));
  const w = 720, h = 188, left = 8, bottom = 158, top = 12;
  const step = (w - left * 2) / Math.max(values.length, 1);
  const points = values.map((d, i) => [left + step * i + step / 2, bottom - (d.uploads / max) * (bottom - top)]);
  const line = points.map((p, i) => `${i ? 'L' : 'M'} ${p[0]} ${p[1]}`).join(' ');
  const area = `${line} L ${points.at(-1)?.[0] ?? 0} ${bottom} L ${points[0]?.[0] ?? 0} ${bottom} Z`;
  return `<svg class="traffic-chart" viewBox="0 0 ${w} ${h}" role="img" aria-label="Daily upload traffic over the last fourteen days">
    <defs><linearGradient id="chartFill" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="var(--chart)" stop-opacity=".25"/><stop offset="1" stop-color="var(--chart)" stop-opacity="0"/></linearGradient></defs>
    ${[0,1,2,3].map(i => { const y = top + i * (bottom-top)/3; return `<line x1="0" y1="${y}" x2="${w}" y2="${y}" class="chart-grid"/>`; }).join('')}
    <path d="${area}" fill="url(#chartFill)"/><path d="${line}" class="chart-line"/>
    ${points.map(p => `<circle cx="${p[0]}" cy="${p[1]}" r="3.5" class="chart-dot"/>`).join('')}
    ${values.map((d,i) => i % Math.max(1, Math.ceil(values.length/7)) === 0 ? `<text x="${left+step*i+step/2}" y="181" text-anchor="middle" class="chart-label">${d.date ? new Date(d.date+'T12:00:00').toLocaleDateString(undefined,{month:'short',day:'numeric'}) : ''}</text>` : '').join('')}
  </svg>`;
}

function render() {
  const visible = data.uploads.filter(u => (u.id + ' ' + u.date + ' ' + u.files.map(f => f.name).join(' ')).toLowerCase().includes(search.toLowerCase()));
  visible.sort((a, b) => sortMode === "largest" ? b.bytes - a.bytes : sortMode === "smallest" ? a.bytes - b.bytes : b.path.localeCompare(a.path));
  const selectedUpload = data.uploads.find(u => u.path === selected);
  app.innerHTML = `
    <div class="shell">
      <aside class="sidebar">
        <a class="brand" href="#" aria-label="Vivify Log Relay home"><span class="brand-mark">V</span><span><strong>vivify</strong><small>LOG RELAY</small></span></a>
        <div class="nav-label">WORKSPACE</div>
        <a class="nav-item active" href="#overview">${icon('grid')}<span>Overview</span></a>
        <a class="nav-item" href="#uploads">${icon('logs')}<span>Uploaded logs</span><span class="nav-count">${data.uploads.length}</span></a>
        <div class="sidebar-bottom"><div class="connection"><span class="status-dot ${online ? 'online' : ''}"></span><span><strong>${online ? 'Relay connected' : 'Connecting to relay'}</strong><small>Cloudflare Worker</small></span></div><div class="sidebar-foot">VIVIFY QUEST <span>DIAGNOSTICS</span></div></div>
      </aside>
      <main class="main">
        <header class="topbar"><div class="mobile-brand"><span class="brand-mark">V</span><strong>Vivify</strong></div><div class="breadcrumb">Workspace <span>/</span> <strong>Overview</strong></div><div class="top-actions"><span class="live-indicator"><i></i> LIVE</span><button class="icon-button" id="refresh" aria-label="Refresh data" title="Refresh">${icon('refresh')}</button></div></header>
        <div class="content">
          <section class="page-heading"><div><div class="eyebrow">DIAGNOSTICS / LOG RELAY</div><h1>Overview</h1><p>A clear view of every diagnostic upload sent by Vivify.</p></div><div class="updated">LAST SYNC <strong id="last-sync">Just now</strong></div></section>
          <section class="stats-grid" aria-label="Relay statistics">
            <article class="stat-card stat-primary"><div class="stat-top"><span>Total uploads</span><span class="stat-icon">${icon('download')}</span></div><div class="stat-value" id="upload-count">${data.uploads.length.toLocaleString()}</div><div class="stat-foot"><span class="pulse"></span> Updates automatically</div></article>
            <article class="stat-card"><div class="stat-top"><span>Log files received</span><span class="stat-icon muted">${icon('file')}</span></div><div class="stat-value">${data.uploads.reduce((n,u)=>n+u.files.length,0).toLocaleString()}</div><div class="stat-foot">Across all uploads</div></article>
            <article class="stat-card"><div class="stat-top"><span>Data stored</span><span class="stat-icon muted">${icon('activity')}</span></div><div class="stat-value">${bytes(data.totalBytes)}</div><div class="stat-foot">Unique diagnostic data on LOGS</div></article>
            <article class="stat-card stat-saved"><div class="stat-top"><span>Storage saved by dedupe</span><span class="stat-icon saved-icon">${icon('activity')}</span></div><div class="stat-value">${bytes(data.dedupeSavedBytes || 0)}</div><div class="stat-foot">${(data.dedupeSavedFiles || 0).toLocaleString()} duplicate files skipped</div></article>
            <article class="stat-card"><div class="stat-top"><span>Latest upload</span><span class="stat-icon muted">${icon('refresh')}</span></div><div class="stat-value stat-date">${data.uploads[0] ? dateLabel(data.uploads[0].date) : '—'}</div><div class="stat-foot">${data.uploads[0] ? data.uploads[0].files.length + ' files in latest upload' : 'Waiting for first upload'}</div></article>
          </section>
          <section class="panel traffic-panel"><div class="panel-heading"><div><h2>Upload traffic</h2><p>Daily uploads received over the last 14 days</p></div><div class="chart-legend"><i></i> Uploads / day</div></div><div class="chart-wrap">${chart()}</div><div class="chart-summary"><strong>${data.days.slice(-14).reduce((n,d)=>n+d.uploads,0)}</strong><span>uploads in this period</span></div></section>
          <section class="panel uploads-panel" id="uploads"><div class="panel-heading uploads-heading"><div><h2>Uploaded logs</h2><p>Browse diagnostic files, newest first</p></div><div class="upload-tools"><label class="search-box">${icon('search')}<input id="search" type="search" placeholder="Search uploads or files…" value="${escapeHtml(search)}" /></label><label class="sort-control"><span>Sort by</span><select id="sort-size" aria-label="Sort uploaded logs"><option value="newest" ${sortMode === "newest" ? "selected" : ""}>Newest</option><option value="largest" ${sortMode === "largest" ? "selected" : ""}>Largest files</option><option value="smallest" ${sortMode === "smallest" ? "selected" : ""}>Smallest files</option></select></label></div></div>
            <div class="table-head"><span>UPLOAD</span><span>FILES</span><span>SIZE</span><span>DATE</span><span></span></div>
            <div class="upload-list">${visible.length ? visible.map(u => `<button class="upload-row" data-upload="${escapeHtml(u.path)}"><span class="upload-name"><span class="folder-icon">${icon('logs')}</span><span><strong>${escapeHtml(u.id)}</strong><small>${escapeHtml(u.path)}</small></span></span><span class="file-count">${u.files.length} files</span><span class="size-cell">${bytes(u.bytes)}</span><span class="date-cell">${dateLabel(u.date)}</span><span class="row-arrow">${icon('chevron')}</span></button>`).join('') : `<div class="empty-state"><div class="empty-icon">${icon('logs')}</div><strong>${search ? 'No matching uploads' : 'No logs uploaded yet'}</strong><p>${search ? 'Try a different search term.' : 'When Vivify sends diagnostic logs, each upload will appear here.'}</p></div>`}</div>
            <div class="list-footer"><span>Showing <strong>${visible.length}</strong> of <strong>${data.uploads.length}</strong> uploads</span><span>Sorted by ${sortMode === "newest" ? "date" : sortMode === "largest" ? "file size · largest first" : "file size · smallest first"}</span></div>
          </section>
          <footer>Vivify Quest <span>·</span> Log Relay <span>·</span> Diagnostic data</footer>
        </div>
      </main>
    </div>
    ${selectedUpload ? `<div class="overlay" id="overlay"><section class="drawer" role="dialog" aria-modal="true" aria-labelledby="drawer-title"><div class="drawer-head"><div><div class="eyebrow">UPLOAD DETAILS</div><h2 id="drawer-title">${escapeHtml(selectedUpload.id)}</h2><p>${dateLabel(selectedUpload.date)} · ${bytes(selectedUpload.bytes)}</p></div><button class="icon-button" id="close-drawer" aria-label="Close">${icon('close')}</button></div><div class="drawer-files"><h3>FILES IN THIS UPLOAD</h3>${selectedUpload.files.slice().sort((a, b) => sortMode === "largest" ? b.size - a.size : sortMode === "smallest" ? a.size - b.size : 0).map(f=>`<div class="file-entry"><button class="file-row" data-file="${escapeHtml(f.path)}"><span class="file-icon">${icon('file')}</span><span><strong>${escapeHtml(f.name)}</strong><small>${bytes(f.size)}</small></span><span class="file-open">${icon('chevron')}</span></button><a class="file-download" href="/api/log?path=${encodeURIComponent(f.path)}&download=1" download="${escapeHtml(f.name)}" aria-label="Download ${escapeHtml(f.name)}">${icon('download')}<span>Download</span></a></div>`).join('')}</div><div class="drawer-note">Logs are stored on the repository's LOGS branch. Routine retention removes logs older than 30 days; storage-pressure cleanup preserves the most recent two hours.</div></section></div>` : ''}
    <div class="toast" id="toast" role="status"></div>
    <div class="viewer" id="viewer" hidden><div class="viewer-card"><div class="viewer-head"><strong id="viewer-name">Log file</strong><button class="icon-button" id="viewer-close" aria-label="Close">${icon('close')}</button></div><pre id="viewer-content">Loading…</pre></div></div>
  `;
  document.querySelector('#refresh')?.addEventListener('click', () => load(true));
  document.querySelector<HTMLSelectElement>('#sort-size')?.addEventListener('change', e => { sortMode = (e.target as HTMLSelectElement).value as typeof sortMode; render(); });
  document.querySelector<HTMLInputElement>('#search')?.addEventListener('input', e => { search = (e.target as HTMLInputElement).value; const pos = (e.target as HTMLInputElement).selectionStart ?? 0; render(); const input = document.querySelector<HTMLInputElement>('#search'); input?.focus(); input?.setSelectionRange(pos,pos); });
  document.querySelectorAll<HTMLElement>('[data-upload]').forEach(el => el.addEventListener('click', () => { selected = el.dataset.upload!; render(); }));
  document.querySelector('#close-drawer')?.addEventListener('click', () => { selected = null; render(); });
  document.querySelector('#overlay')?.addEventListener('click', e => { if (e.target === e.currentTarget) { selected = null; render(); } });
  document.querySelectorAll<HTMLElement>('[data-file]').forEach(el => el.addEventListener('click', () => openFile(el.dataset.file!)));
  document.querySelector('#viewer-close')?.addEventListener('click', () => { const viewer = document.querySelector<HTMLElement>('#viewer')!; viewer.hidden = true; });
}

async function openFile(path: string) {
  const viewer = document.querySelector<HTMLElement>('#viewer')!;
  document.querySelector('#viewer-name')!.textContent = path.split('/').at(-1)!;
  document.querySelector('#viewer-content')!.textContent = 'Loading log contents…';
  viewer.hidden = false;
  try {
    const response = await fetch('/api/log?path=' + encodeURIComponent(path));
    if (!response.ok) throw new Error('Could not load this log file.');
    document.querySelector('#viewer-content')!.textContent = await response.text();
  } catch (error) { document.querySelector('#viewer-content')!.textContent = error instanceof Error ? error.message : 'Unknown error'; }
}

async function load(showToast = false) {
  try {
    const response = await fetch('/api/logs', { cache: 'no-store' });
    if (!response.ok) throw new Error('Relay returned ' + response.status);
    data = await response.json() as DashboardData;
    online = true;
    render();
    if (showToast) toast('Dashboard refreshed');
    const sync = document.querySelector('#last-sync');
    if (sync) sync.textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  } catch {
    online = false;
    if (!document.querySelector('.shell')) render();
    document.querySelector('.connection strong')!.textContent = 'Relay unavailable';
    document.querySelector('.status-dot')!.classList.remove('online');
    if (showToast) toast('Could not reach the log relay');
  }
}
function toast(message: string) {
  const el = document.querySelector<HTMLElement>('#toast')!;
  el.textContent = message; el.classList.add('show');
  window.setTimeout(() => el.classList.remove('show'), 2400);
}
render();
void load();
refreshTimer = window.setInterval(() => void load(), 15000);
window.addEventListener('beforeunload', () => window.clearInterval(refreshTimer));
