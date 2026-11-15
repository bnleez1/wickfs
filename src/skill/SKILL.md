---
name: gdrive
description: >
  Manage the local Google Drive smart virtual filesystem (~/Drive) and LFU/LRU
  disk cache budget using the gdrive CLI and desktop app.
---

# gdrive Skill

`gdrive` exposes Google Drive at `~/Drive` via a FUSE3 on-demand virtual filesystem and manages local disk usage with a frequency + recency (LFU/LRU) retention engine.

## Headless CLI Commands

All CLI commands support `--json` for structured output:

```bash
# Check FUSE3, rclone, and virtual drive status
gdrive check --json

# Inspect cache usage, budget ceiling, disk free space, and file counts
gdrive status --json

# List files (filters: all, frequent, pinned, cached, cloud)
gdrive ls --filter frequent --json

# Hydrate a file on-demand and record an access event
gdrive access "Architecture/Distributed-Cache-Spec.pdf" --json

# Pin or unpin a file for permanent offline availability
gdrive pin "Architecture/API-Security-Review.md" --json
gdrive unpin "Architecture/API-Security-Review.md" --json

# Reclaim disk space by evicting a specific file or all cold cached files
gdrive evict --cold --json

# Configure maximum cache size (MB), free disk floor (GB), and frequent access threshold
gdrive budget --max-mb 250 --min-free-gb 15 --threshold 3 --json

# Install or update the Omarchy top-bar sync status widget plugin (ben.gdrive)
gdrive plugin install --json
```
