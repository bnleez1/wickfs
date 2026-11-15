---
name: wickfs
description: >
  Manage the local Google Drive smart virtual filesystem (~/Drive) and LFU/LRU
  disk cache budget using the wickfs CLI and desktop app.
---

# WickFS Skill

`wickfs` exposes Google Drive at `~/Drive` via a FUSE3 on-demand virtual filesystem and manages local disk usage with a frequency + recency (LFU/LRU) retention engine.

## Headless CLI Commands

All CLI commands support `--json` for structured output:

```bash
# Check FUSE3, rclone, and virtual drive status
wickfs check --json

# Inspect cache usage, budget ceiling, disk free space, and file counts
wickfs status --json

# List files (filters: all, frequent, pinned, cached, cloud)
wickfs ls --filter frequent --json

# Hydrate a file on-demand and record an access event
wickfs access "Architecture/Distributed-Cache-Spec.pdf" --json

# Pin or unpin a file for permanent offline availability
wickfs pin "Architecture/API-Security-Review.md" --json
wickfs unpin "Architecture/API-Security-Review.md" --json

# Reclaim disk space by evicting a specific file or all cold cached files
wickfs evict --cold --json

# Configure maximum cache size (MB), free disk floor (GB), and frequent access threshold
wickfs budget --max-mb 250 --min-free-gb 15 --threshold 3 --json

# Configure Google OAuth Client ID / Secret or verify built-in OAuth token
wickfs auth status --json
wickfs auth setup --builtin --json
wickfs auth setup --import-json ~/Downloads/client_secret.json --json

# Install or update the Omarchy top-bar sync status widget plugin (ben.wickfs)
wickfs plugin install --json

# Enable or inspect automatic startup on reboot and wake from standby
wickfs autostart enable --json

# Run background daemon (mounts ~/Drive, syncs periodically, and syncs on standby wake)
wickfs daemon --once --json
```
