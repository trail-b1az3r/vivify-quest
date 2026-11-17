import { readFileSync } from "node:fs";

const event = JSON.parse(readFileSync(process.env.GITHUB_EVENT_PATH, "utf8"));
const [owner, repository] = process.env.GITHUB_REPOSITORY.split("/");
const issue = event.issue;
const action = event.action;
const isCommentEvent = process.env.GITHUB_EVENT_NAME === "issue_comment";
const githubToken = process.env.GITHUB_TOKEN;
const discordToken = process.env.DISCORD_BOT_TOKEN;
const defaultForumId = process.env.DISCORD_FORUM_CHANNEL_ID;
const featureForumId = process.env.DISCORD_FEATURE_REQUESTS_CHANNEL_ID;
const markerPattern = /<!-- discord-forum-thread:(\d+) starter:(\d+)(?: channel:(\d+))? -->/;

if (!issue || issue.pull_request) {
  console.log("Not an issue event; skipping.");
  process.exit(0);
}
if (!githubToken || !discordToken || !defaultForumId) {
  throw new Error("Missing GITHUB_TOKEN, DISCORD_BOT_TOKEN, or DISCORD_FORUM_CHANNEL_ID.");
}

async function github(url, { method = "GET", body } = {}) {
  const response = await fetch(url, {
    method,
    headers: {
      Accept: "application/vnd.github+json",
      "X-GitHub-Api-Version": "2022-11-28",
      Authorization: `Bearer ${githubToken}`,
      ...(body === undefined ? {} : { "Content-Type": "application/json" })
    },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  if (!response.ok) throw new Error(`GitHub API ${method} ${url} failed (${response.status}): ${await response.text()}`);
  return response.status === 204 ? null : response.json();
}

async function discord(path, { method = "GET", body } = {}) {
  const response = await fetch(`https://discord.com/api/v10${path}`, {
    method,
    headers: {
      Authorization: `Bot ${discordToken}`,
      "Content-Type": "application/json"
    },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  if (!response.ok) throw new Error(`Discord API ${method} ${path} failed (${response.status}): ${await response.text()}`);
  return response.status === 204 ? null : response.json();
}

const api = `https://api.github.com/repos/${owner}/${repository}`;
const issueUrl = issue.html_url;
const issueNumber = issue.number;

function clean(value = "") {
  return String(value).replace(/@everyone/g, "@\u200beveryone").replace(/@here/g, "@\u200bhere");
}
function truncate(value, max = 1850) {
  const valueClean = clean(value);
  return valueClean.length > max ? valueClean.slice(0, max - 20) + "\n… (truncated)" : valueClean;
}
function isFeatureRequest() {
  return (issue.labels || []).some(label => {
    const name = (typeof label === "string" ? label : label.name || "").toLowerCase().replace(/[_ ]+/g, "-");
    return name === "feature-request" || name === "enhancement";
  });
}
function destinationForumId() {
  if (isFeatureRequest()) {
    if (!featureForumId) throw new Error("This issue is a feature request, but DISCORD_FEATURE_REQUESTS_CHANNEL_ID is not configured.");
    return featureForumId;
  }
  return defaultForumId;
}
function labelsText() {
  const labels = (issue.labels || []).map(label => typeof label === "string" ? label : label.name);
  const assignees = (issue.assignees || []).map(user => user.login);
  return [
    `**State:** ${issue.state === "closed" ? "Closed" : "Open"}`,
    `**Labels:** ${labels.length ? labels.join(", ") : "None"}`,
    `**Assignees:** ${assignees.length ? assignees.join(", ") : "Unassigned"}`
  ].join("\n");
}
function issueContent() {
  const body = truncate(issue.body || "_No description provided._", 1250);
  return truncate(`**[ #${issueNumber} ] ${clean(issue.title)}**\n${issueUrl}\n\n${body}\n\n---\n${labelsText()}`, 1950);
}

async function findMapping() {
  const mappings = [];
  let page = 1;
  while (page <= 10) {
    const comments = await github(`${api}/issues/${issueNumber}/comments?per_page=100&page=${page}`);
    for (const comment of comments) {
      const match = comment.body?.match(markerPattern);
      if (match) mappings.push({
        threadId: match[1],
        starterId: match[2],
        channelId: match[3] || defaultForumId,
        commentId: comment.id,
        createdAt: comment.created_at
      });
    }
    if (comments.length < 100) break;
    page++;
  }
  if (!mappings.length) return null;

  // Keep the newest mapping and remove stale duplicate Discord posts left by earlier racing runs.
  mappings.sort((a, b) => new Date(b.createdAt) - new Date(a.createdAt));
  const canonical = mappings[0];
  const seenThreads = new Set([canonical.threadId]);
  for (const duplicate of mappings.slice(1)) {
    if (!seenThreads.has(duplicate.threadId)) {
      seenThreads.add(duplicate.threadId);
      try {
        await discord(`/channels/${duplicate.threadId}`, { method: "DELETE" });
        console.log(`Removed duplicate Discord thread ${duplicate.threadId} for issue #${issueNumber}.`);
      } catch (error) {
        // A stale mapping may already point to a thread that was manually deleted.
        if (!String(error).includes("10003") && !String(error).includes("Unknown Channel")) throw error;
      }
    }
    await github(`${api}/issues/comments/${duplicate.commentId}`, { method: "DELETE" });
  }
  return canonical;
}

async function saveMapping(threadId, starterId, channelId) {
  const marker = `<!-- discord-forum-thread:${threadId} starter:${starterId} channel:${channelId} -->`;
  await github(`${api}/issues/${issueNumber}/comments`, {
    method: "POST",
    body: { body: `Discord forum thread ID: ${threadId}\n\n${marker}` }
  });
}

async function createThread(channelId) {
  const created = await discord(`/channels/${channelId}/threads`, {
    method: "POST",
    body: {
      name: `#${issueNumber} · ${clean(issue.title).slice(0, 85)}`.slice(0, 100),
      auto_archive_duration: 10080,
      message: { content: issueContent(), allowed_mentions: { parse: [] } }
    }
  });
  let starterId = created.message?.id;
  if (created.id && !starterId) {
    const messages = await discord(`/channels/${created.id}/messages?limit=1`);
    starterId = messages[0]?.id;
  }
  if (!created.id || !starterId) throw new Error("Discord did not return a forum thread ID and starter message ID.");
  await saveMapping(created.id, starterId, channelId);
  console.log(`Created Discord forum thread ${created.id} for issue #${issueNumber} in channel ${channelId}.`);
  return { threadId: created.id, starterId, channelId };
}

async function ensureCorrectDestination(mapping) {
  const targetChannelId = destinationForumId();
  if (!mapping) return createThread(targetChannelId);
  if (mapping.channelId === targetChannelId) return mapping;

  const replacement = await createThread(targetChannelId);
  await discord(`/channels/${mapping.threadId}`, { method: "DELETE" });
  console.log(`Moved issue #${issueNumber} to its correct Discord forum channel.`);
  return replacement;
}

let mapping = await findMapping();

if (issue.state !== "open") {
  if ((action === "closed" || action === "deleted") && mapping) {
    await discord(`/channels/${mapping.threadId}`, { method: "DELETE" });
    await github(`${api}/issues/comments/${mapping.commentId}`, { method: "DELETE" });
    console.log(`Deleted Discord thread and mapping for completed issue #${issueNumber}.`);
  } else {
    console.log(`Issue #${issueNumber} is closed; no sync performed.`);
  }
  process.exit(0);
}

mapping = await ensureCorrectDestination(mapping);

if (isCommentEvent) {
  const comment = event.comment;
  if (comment?.body?.includes("<!-- discord-forum-thread:")) {
    console.log("Integration mapping comment; skipping.");
    process.exit(0);
  }
  if (action === "deleted") {
    await discord(`/channels/${mapping.threadId}/messages`, {
      method: "POST",
      body: {
        content: `A GitHub comment by **${clean(comment?.user?.login || "a user")}** was deleted.\n${issueUrl}`,
        allowed_mentions: { parse: [] }
      }
    });
  } else if (action === "created" || action === "edited") {
    const heading = action === "edited" ? "GitHub comment edited" : "New GitHub comment";
    await discord(`/channels/${mapping.threadId}/messages`, {
      method: "POST",
      body: {
        content: truncate(`**${heading} — ${clean(comment.user?.login || "Unknown user")}**\n${issueUrl}\n\n${comment.body || "_Empty comment_"}`, 1950),
        allowed_mentions: { parse: [] }
      }
    });
  }
  console.log(`Synced comment event '${action}' for issue #${issueNumber}.`);
  process.exit(0);
}

if (action === "deleted") {
  await discord(`/channels/${mapping.threadId}`, { method: "DELETE" });
  await github(`${api}/issues/comments/${mapping.commentId}`, { method: "DELETE" });
  console.log(`Deleted Discord thread and mapping for deleted issue #${issueNumber}.`);
  process.exit(0);
}
if (action === "reopened") {
  await discord(`/channels/${mapping.threadId}`, { method: "PATCH", body: { archived: false, locked: false } });
}
if (action === "edited") {
  await discord(`/channels/${mapping.threadId}`, {
    method: "PATCH",
    body: { name: `#${issueNumber} · ${clean(issue.title).slice(0, 85)}`.slice(0, 100) }
  });
}
if (action !== "opened" && action !== "sync") {
  await discord(`/channels/${mapping.threadId}/messages/${mapping.starterId}`, {
    method: "PATCH",
    body: { content: issueContent(), allowed_mentions: { parse: [] } }
  });
  await discord(`/channels/${mapping.threadId}/messages`, {
    method: "POST",
    body: {
      content: `GitHub issue updated: **${clean(action)}**\n${issueUrl}\n${labelsText()}`,
      allowed_mentions: { parse: [] }
    }
  });
} else if (action === "sync") {
  await discord(`/channels/${mapping.threadId}/messages/${mapping.starterId}`, {
    method: "PATCH",
    body: { content: issueContent(), allowed_mentions: { parse: [] } }
  });
}
console.log(`Synced open issue #${issueNumber} action '${action}'.`);
