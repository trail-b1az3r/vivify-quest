# Vivify log relay

The relay receives the mod's diagnostic uploads and stores unique log contents on the
repository's `LOGS` branch. It also hosts a responsive dashboard for browsing
those uploads. Before committing, it hashes each file with SHA3-256 and skips content already
present on the active branch, including duplicates within one request. A compact
`.vivify-sha3-index.json` is stored on the branch so normal uploads do not need to
re-download every historical log. The first upload after upgrading backfills the index
from existing logs. Mixed uploads store only their new files; fully duplicate uploads
create no commit. The 30-day retention task also removes expired entries from the index.

## Dashboard

The dashboard is a Vite + TypeScript application with a hand-built Material 3
Expressive-inspired interface. It includes:

- A live upload counter and summary of received files and stored data.
- A 14-day upload traffic graph.
- A newest-first upload list with search and per-upload file details.
- An in-browser plain-text log viewer.
- A responsive layout designed for desktop and phone screens.
- Automatic refresh every 15 seconds.

The dashboard API exposes:
- `GET /api/health` for relay status and aggregate counts.
- `GET /api/logs` to list uploads, with optional `?date=YYYY-MM-DD` and
  `?upload=<upload-id>` filters.
- `GET /api/log?path=<encoded-repository-path>` to read one log as plain text.
- `GET /api/log?path=<encoded-repository-path>&download=1` to download one
  log as an attachment.

Each file in an upload's detail drawer has a Download action. The dashboard
continues to support in-browser viewing as well. Reads are limited to files
listed in the relay's log index; the GitHub write token remains in the Worker
secret and is never sent to the browser.

A scheduled Worker task runs hourly and removes log files whose upload date is
more than 30 days old from the active `LOGS` branch. Expiration is based on
the upload's UTC timestamp encoded in its stored path. This is branch-level retention: because
Git stores prior commits, an expired file can remain in old commit history.


## Set up and deploy

1. Create a fine-grained GitHub token with access only to
   `trail-b1az3r/vivify-quest` and **Contents: Read and write** permission.
   Protect `main` with a branch rule; the relay writes to the separate
   `LOGS` branch.
2. Install dependencies and build the dashboard:
   ```sh
   cd tools/log-relay
   npm install
   npm run build
   ```
3. Deploy the Worker:
   ```sh
   npx wrangler login
   npx wrangler secret put GITHUB_TOKEN
   npx wrangler deploy
   ```
   Wrangler deploys the built `dist/` dashboard alongside the Worker.
4. Set the repository Actions variable `VIVIFY_LOG_RELAY_URL` to the
   deployed `workers.dev` address. Builds from then on send logs to it.
   A player can also set `logRelayUrl` in Vivify's config.

For local development, build once and start the Worker in one terminal:

```sh
npm run build
npx wrangler dev
```

Then start Vite in a second terminal with `npm run dev`. Vite proxies
`/api/*` requests to the local Worker at `127.0.0.1:8787`, while serving the
dashboard with hot reload.

## Test

Run `node test.mjs` to test upload validation, content deduplication (existing files,
within-request duplicates, and mixed uploads), log downloads, listing filters,
scheduled retention, and GitHub branch commits against a fake GitHub API.
Run `npm run build` to type-check and build the dashboard.

## Troubleshooting

- `GET /api/health` reports `"writeReady": false`: the Worker has no
  `GITHUB_TOKEN` secret. Add it (`npx wrangler secret put GITHUB_TOKEN`, or
  Cloudflare dashboard → Workers → the relay → Settings → Variables and
  Secrets) and redeploy. The mod checks this before uploading and logs
  "relay has no GitHub token" instead of sending.
- Uploads go to `POST /api/upload`. A `POST` to `/` is answered by the
  dashboard assets (405) on an older Worker, or 404 with a hint on this one.
- Intermittent 403 from `/api/health` or `/api/logs` means GitHub's
  anonymous rate limit; it stops once the token secret is set.
