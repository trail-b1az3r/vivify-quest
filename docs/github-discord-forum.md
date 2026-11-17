# GitHub Issues → Discord Forums

The workflow mirrors **open GitHub issues only** into Discord Forum channels. Each issue has one linked forum post; comments are synchronized into that post's thread.

## Required GitHub Actions secrets

Add these under **Settings → Secrets and variables → Actions**:

| Secret | Value |
|---|---|
| `DISCORD_BOT_TOKEN` | Discord bot token |
| `DISCORD_FORUM_CHANNEL_ID` | Forum channel for ordinary issues and bugs |
| `DISCORD_FEATURE_REQUESTS_CHANNEL_ID` | Separate Forum channel for feature requests |

The feature-request channel secret is required when syncing an issue labelled `feature-request` or `enhancement`. Keep the bot token private.

## Discord permissions

Invite the bot with the `bot` scope and grant access to both Forum channels. It needs View Channel, Send Messages, Create Public Threads, Send Messages in Threads, and Manage Threads. Ensure its role and each channel's permission overrides allow those actions.

## Routing and synchronization

- New/open issues without the `feature-request` or `enhancement` label go to `DISCORD_FORUM_CHANNEL_ID`.
- Issues labelled `feature-request` or `enhancement` go to `DISCORD_FEATURE_REQUESTS_CHANNEL_ID`.
- If an issue's labels change and it belongs in the other forum, the integration creates its post in the correct forum, updates the GitHub mapping, and deletes the old Discord post.
- Issue title, description, labels, assignees, and relevant issue updates are synchronized.
- GitHub comments created, edited, or deleted while the issue is open are synchronized into its Discord thread.
- Closing an issue deletes its Discord forum thread and removes the stored mapping. Reopening it creates a fresh forum post. Closed issues and their comments are not synchronized. Reopening an issue resumes synchronization.
- Pull requests are ignored.
- A manual workflow run fetches **only open issues**. It does not import closed issues or historical comments.

The integration stores a hidden mapping comment on each GitHub issue. It records the Discord channel, thread, and starter-message IDs so later events can update the same post.

## Feature request template

The **Feature request** template automatically applies the `feature-request` label, which routes it to the separate feature-request forum. If you manually create an issue instead, apply that label to use the feature-request channel.

## Manual sync

Run **Actions → GitHub Issues to Discord Forum → Run workflow** to sync all currently open issues. Existing posts are updated rather than duplicated.


## Duplicate prevention and completed issues

All workflow events for this repository are serialized so a manual sync cannot race an issue-open/comment event while both are creating the same forum post. If older runs already left multiple mapping comments, the script keeps the newest mapping, deletes the extra Discord threads, and removes the stale mapping comments. When an issue is closed, its Discord post is deleted (the GitHub issue itself is never deleted).
