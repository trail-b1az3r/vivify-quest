// Tests worker.js against a fake GitHub API: node test.mjs
import { createHash } from "node:crypto";
import worker, { sha3_256 } from "./worker.js";
let refSha = null, commits = {}, trees = {}, blobs = {}, n = 0, raceOnce = false, calls = [], authHeaders = [];
globalThis.fetch = async (url, init) => {
  const path = url.replace("https://api.github.com/repos/trail-b1az3r/vivify-quest", "");
  const body = init.body ? JSON.parse(init.body) : null;
  calls.push(init.method + " " + path);
  if (init.method === "GET") authHeaders.push(init.headers?.Authorization);
  const json = (status, data) => new Response(data ? JSON.stringify(data) : null, { status });
  if (init.method === "GET" && path === "/git/ref/heads/LOGS") return refSha ? json(200, { object: { sha: refSha } }) : json(404, { message: "Not Found" });
  if (init.method === "GET" && path.startsWith("/git/commits/")) return json(200, commits[path.split("/").pop()]);
  if (init.method === "GET" && path.startsWith("/git/trees/")) {
    const sha = path.split("/").pop().split("?")[0];
    const tree = (trees[sha]?.tree || []).map((entry, i) => {
      const blobSha = createHash("sha1").update(`blob ${Buffer.byteLength(entry.content)}\0`).update(entry.content).digest("hex");
      blobs[blobSha] = entry.content;
      return { path: entry.path, type: "blob", size: entry.content.length, sha: blobSha };
    });
    return json(200, { tree });
  }
  if (init.method === "GET" && path.startsWith("/git/blobs/")) {
    const content = blobs[path.split("/").pop()];
    return content === undefined ? json(404, {}) : json(200, { content: Buffer.from(content).toString("base64"), encoding: "base64" });
  }
  if (init.method === "POST" && path === "/git/trees") {
    const sha = "t" + ++n;
    const inherited = body.base_tree ? (trees[body.base_tree]?.tree || []).map(entry => ({ ...entry })) : [];
    for (const entry of body.tree || []) {
      const at = inherited.findIndex(item => item.path === entry.path);
      if (entry.sha === null) { if (at >= 0) inherited.splice(at, 1); }
      else if (at >= 0) inherited[at] = entry;
      else inherited.push(entry);
    }
    trees[sha] = { tree: inherited, base_tree: body.base_tree };
    return json(201, { sha });
  }
  if (init.method === "POST" && path === "/git/commits") { const sha = "c" + ++n; commits[sha] = { tree: { sha: body.tree }, parents: body.parents }; return json(201, { sha }); }
  if (init.method === "POST" && path === "/git/refs") { refSha = body.sha; return json(201, {}); }
  if (init.method === "PATCH" && path === "/git/refs/heads/LOGS") {
    if (raceOnce) { raceOnce = false; refSha = "cX"; commits["cX"] = { tree: { sha: "tX" }, parents: [] }; return json(422, {}); }
    refSha = body.sha; return json(200, {});
  }
  return json(500, {});
};
const env = { GITHUB_TOKEN: "x" };
const post = (obj) => worker.fetch(new Request("https://relay/api/upload", { method: "POST", body: JSON.stringify(obj) }), env);
const good = { version: "0.15.20", lite: false, upload: "0123456789abcdef", files: [{ name: "VivifyReport.txt", from: 0, text: "hello" }, { name: "vivify_global-p4.txt", from: 10, text: "log" }] };
let fails = 0; const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };
check("SHA3-256 matches the FIPS 202 abc test vector", Buffer.from(sha3_256(new TextEncoder().encode("abc"))).toString("hex") === "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
let r = await post(good); let t = await r.text();
check("first upload creates the orphan branch", r.status === 200 && refSha && commits[refSha].parents.length === 0 && calls.includes("POST /git/refs"));
const firstTree = trees[commits[refSha].tree.sha];
check("files land in logs/<date>/<time>_<version>_<id>/ and persist the SHA3 index", firstTree.tree.filter(e => e.path.startsWith("logs/")).every(e => /^logs\/\d{4}-\d\d-\d\d\/\d{6}_0\.15\.20_0123456789abcdef\//.test(e.path)) && firstTree.tree.some(e => e.path === ".vivify-sha3-index.json") && !firstTree.base_tree);
const first = refSha;
r = await worker.fetch(new Request("https://relay/api/logs"), env);
const index = await r.json();
check("dashboard API lists uploaded bundles", r.status === 200 && index.uploads.length === 1 && index.uploads[0].files.length === 2 && index.totalBytes === 8);
check("configured token authenticates GitHub reads", authHeaders.at(-1) === "Bearer x");
check("health endpoint reports upload and file counts", (await worker.fetch(new Request("https://relay/api/health"), env)).status === 200);
{
  const health = await (await worker.fetch(new Request("https://relay/api/health"), env)).json();
  check("health says uploads can be saved with a token", health.writeReady === true);
  const bare = await (await worker.fetch(new Request("https://relay/api/health"), {})).json();
  check("health says uploads cannot be saved without one", bare.writeReady === false);
}
check("a POST outside /api is pointed at /api/upload",
      (await worker.fetch(new Request("https://relay/", { method: "POST", body: JSON.stringify(good) }), env)).status === 404);
r = await worker.fetch(new Request("https://relay/api/logs"), {});
check("public dashboard reads work without a write token", r.status === 200 && authHeaders.at(-1) === undefined);
const logPath = index.uploads[0].files[0].path;
r = await worker.fetch(new Request("https://relay/api/log?path=" + encodeURIComponent(logPath)), env);
check("dashboard API reads an uploaded log", r.status === 200 && (await r.text()) === "hello");
const download = await worker.fetch(new Request("https://relay/api/log?path=" + encodeURIComponent(logPath) + "&download=1"), env);
check("log download endpoint sends an attachment filename", download.status === 200 && download.headers.get("Content-Disposition") === 'attachment; filename="VivifyReport.txt"');
const filtered = await (await worker.fetch(new Request("https://relay/api/logs?upload=" + encodeURIComponent(index.uploads[0].id)), env)).json();
check("log listing can filter to one upload", filtered.uploads.length === 1 && filtered.uploads[0].id === index.uploads[0].id);
check("log listing rejects malformed date filters", (await worker.fetch(new Request("https://relay/api/logs?date=not-a-date"), env)).status === 400);
check("dashboard rejects paths outside the log allowlist", (await worker.fetch(new Request("https://relay/api/log?path=" + encodeURIComponent("../secrets.txt")), env)).status === 400);
const beforeDuplicate = refSha;
r = await post({ ...good, upload: "1123456789abcdef" });
check("identical files are deduplicated without a new commit", r.status === 200 && (await r.text()).includes("deduplicated 2 file(s)") && refSha === beforeDuplicate);
r = await post({ ...good, upload: "2123456789abcdef", files: [{ name: "VivifyReport.txt", text: "hello" }, { name: "VivifyCrash.txt", text: "new crash detail" }] });
check("mixed uploads store only files with new content", r.status === 200 && (await r.text()).includes("stored 1, deduplicated 1") && commits[refSha].parents[0] === beforeDuplicate);
const beforeWithinBatch = refSha;
r = await post({ ...good, upload: "3123456789abcdef", files: [{ name: "VivifyReport.txt", text: "same in request" }, { name: "VivifyCrash.txt", text: "same in request" }] });
check("duplicates inside one request are stored only once", r.status === 200 && (await r.text()).includes("stored 1, deduplicated 1") && commits[refSha].parents[0] === beforeWithinBatch);
raceOnce = true;
r = await post({ ...good, upload: "4123456789abcdef", files: [{ name: "VivifyCrash.txt", text: "race-only unique payload" }] });
check("a race retries on top of the moved branch", r.status === 200 && commits[refSha].parents[0] === "cX");
check("bad file name refused", (await post({ ...good, files: [{ name: "../../.github/workflows/x.yml", text: "a" }] })).status === 400);
check("other names refused", (await post({ ...good, files: [{ name: "secrets.txt", text: "a" }] })).status === 400);
check("bad version refused", (await post({ ...good, version: "<script>" })).status === 400);
check("bad upload id refused", (await post({ ...good, upload: "../x" })).status === 400);
check("too many files refused", (await post({ ...good, files: Array(13).fill(good.files[0]) })).status === 400);
check("too much text refused", (await post({ ...good, files: [{ name: "VivifyReport.txt", text: "a".repeat(4.6 * 1024 * 1024) }] })).status === 413);
r = await post({ ...good, upload: "1123456789abcdef", files: [{ name: "VivifySession-part001.txt", from: 0, text: "session chunk" }, { name: "vivify_global-p4-part002.txt", from: 1536, text: "global chunk" }] });
check("split session and global log parts are accepted", r.status === 200);
r = await post({ ...good, upload: "2123456789abcdef", files: [{ name: "VivifySession-part1000.txt", text: "bad part suffix" }] });
check("invalid split suffix is refused", r.status === 400);
check("unknown GET route returns 404", (await worker.fetch(new Request("https://relay/"), env)).status === 404);
check("no token refused", (await worker.fetch(new Request("https://relay/api/upload", { method: "POST", body: "{}" }), {})).status === 500);

{
  const currentTree = trees[commits[refSha].tree.sha];
  currentTree.tree.push({ path: "logs/2000-01-01/expired_upload/VivifyReport.txt", content: "expired log" });
  let retentionPromise;
  await worker.scheduled({}, env, { waitUntil: promise => { retentionPromise = promise; } });
  await retentionPromise;
  const afterRetention = await (await worker.fetch(new Request("https://relay/api/logs"), env)).json();
  check("scheduled retention removes logs older than 30 days", !afterRetention.uploads.some(item => item.date === "2000-01-01"));
}
console.log(fails + " failure(s)"); process.exit(fails ? 1 : 0);
