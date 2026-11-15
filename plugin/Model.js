.pragma library

function sanitizeText(raw, maxLen) {
  var limit = (typeof maxLen === "number" && maxLen > 0) ? maxLen : 120
  var s = String(raw === undefined || raw === null ? "" : raw)
  // Neutralize HTML/rich-text markup characters and control characters.
  s = s.replace(/[<>&"'`\r\n\t]/g, " ").replace(/\s+/g, " ").trim()
  if (s.length > limit) {
    s = s.slice(0, limit - 1) + "…"
  }
  return s
}

function clampNum(v, lo, hi, fallback) {
  var n = Number(v)
  if (!isFinite(n)) return fallback
  return Math.max(lo, Math.min(hi, n))
}

function formatMb(bytes, formatNumberFn) {
  var b = clampNum(bytes, 0, 1e15, 0)
  var fmt = typeof formatNumberFn === "function"
    ? formatNumberFn
    : function(val, dec) { return Number(val).toFixed(dec) }
  var gb = 1024 * 1024 * 1024
  var mb = 1024 * 1024
  if (b >= gb) return fmt(b / gb, 1) + " GB"
  return fmt(b / mb, 0) + " MB"
}

function parseStateJson(rawText, nowMs) {
  var empty = {
    ok: true,
    ready: true,
    syncing: false,
    syncState: "synced",
    activeSyncFile: "",
    syncProgress: 1.0,
    syncPercent: 100,
    pendingSyncCount: 0,
    sourceMode: "catalog",
    remoteName: "gdrive",
    maxCacheBytes: 250 * 1024 * 1024,
    cachedBytes: 196 * 1024 * 1024,
    pinnedBytes: 46 * 1024 * 1024,
    hotCachedBytes: 150 * 1024 * 1024,
    totalCloudBytes: 675 * 1024 * 1024,
    savedBytes: 479 * 1024 * 1024,
    savedPct: 71,
    totalFiles: 10,
    cachedFiles: 5,
    pinnedFiles: 2,
    frequentFiles: 4,
    cloudOnlyFiles: 5,
    lastSyncedSecs: Math.floor((nowMs || Date.now()) / 1000),
    files: []
  }

  if (!rawText || typeof rawText !== "string") return empty
  var trimmed = rawText.trim()
  if (!trimmed || trimmed.length > 262144) return empty

  var obj = null
  try {
    obj = JSON.parse(trimmed)
  } catch (e) {
    return empty
  }
  if (!obj || typeof obj !== "object") return empty

  var maxCache = clampNum(obj.maxCacheBytes, 1024 * 1024, 1e14, empty.maxCacheBytes)
  var threshold = clampNum(obj.frequentThreshold, 1, 100, 3)
  var rawEntries = Array.isArray(obj.entries) ? obj.entries.slice(0, 64) : []

  var totalCloud = 0
  var cachedBytes = 0
  var pinnedBytes = 0
  var hotCachedBytes = 0
  var cachedFiles = 0
  var pinnedFiles = 0
  var frequentFiles = 0
  var cloudOnlyFiles = 0
  var parsedFiles = []

  for (var i = 0; i < rawEntries.length; i++) {
    var it = rawEntries[i]
    if (!it || typeof it !== "object") continue
    var name = sanitizeText(it.name || it.path || "file", 64)
    var folder = sanitizeText(it.folder || "Root", 40)
    var sizeBytes = clampNum(it.sizeBytes, 0, 1e13, 0)
    var pinned = Boolean(it.pinned)
    var state = pinned ? "pinned" : sanitizeText(it.state || "cloud", 16)
    if (state !== "pinned" && state !== "cached" && state !== "cloud") state = "cloud"
    var accessCount = clampNum(it.accessCount, 0, 100000, 0)
    var isFrequent = accessCount >= threshold

    totalCloud += sizeBytes
    if (pinned) {
      pinnedBytes += sizeBytes
      cachedBytes += sizeBytes
      pinnedFiles += 1
      cachedFiles += 1
    } else if (state === "cached") {
      hotCachedBytes += sizeBytes
      cachedBytes += sizeBytes
      cachedFiles += 1
    } else {
      cloudOnlyFiles += 1
    }
    if (isFrequent) frequentFiles += 1

    parsedFiles.push({
      name: name,
      folder: folder,
      sizeBytes: sizeBytes,
      state: state,
      pinned: pinned,
      accessCount: accessCount,
      isFrequent: isFrequent
    })
  }

  parsedFiles.sort(function(a, b) {
    if (a.pinned !== b.pinned) return a.pinned ? -1 : 1
    if ((a.state !== "cloud") !== (b.state !== "cloud")) return a.state !== "cloud" ? -1 : 1
    return b.accessCount - a.accessCount
  })

  var savedBytes = Math.max(0, totalCloud - cachedBytes)
  var savedPct = totalCloud > 0 ? Math.round((savedBytes * 100) / totalCloud) : 0
  var syncing = Boolean(obj.syncing)
  var activeFile = sanitizeText(obj.activeSyncFile || "", 64)
  var progress = clampNum(obj.syncProgress, 0.05, 1.0, syncing ? 0.45 : 1.0)
  var pendingCount = clampNum(obj.pendingSyncCount, 0, 999, syncing ? 1 : 0)

  return {
    ok: true,
    ready: true,
    syncing: syncing,
    syncState: syncing ? "syncing" : "synced",
    activeSyncFile: activeFile,
    syncProgress: progress,
    syncPercent: Math.round(progress * 100),
    pendingSyncCount: pendingCount,
    sourceMode: sanitizeText(obj.sourceMode || "catalog", 24),
    remoteName: sanitizeText(obj.remoteName || "gdrive", 32),
    maxCacheBytes: maxCache,
    cachedBytes: cachedBytes,
    pinnedBytes: pinnedBytes,
    hotCachedBytes: hotCachedBytes,
    totalCloudBytes: totalCloud,
    savedBytes: savedBytes,
    savedPct: savedPct,
    totalFiles: parsedFiles.length,
    cachedFiles: cachedFiles,
    pinnedFiles: pinnedFiles,
    frequentFiles: frequentFiles,
    cloudOnlyFiles: cloudOnlyFiles,
    lastSyncedSecs: clampNum(obj.lastSyncedSecs, 0, 4e9, Math.floor((nowMs || Date.now()) / 1000)),
    files: parsedFiles.slice(0, 6)
  }
}

function buildTooltip(state) {
  if (!state || typeof state !== "object") return "GDrive Smart Cache"
  var status = state.syncing ? ("Syncing " + sanitizeText(state.activeSyncFile || "files", 48) + " (" + clampNum(state.syncPercent, 0, 100, 50) + "%)") : "All files in sync"
  return sanitizeText("GDrive Smart Cache — " + status, 140)
}

function parseState(rawText, nowMs) {
  return parseStateJson(rawText, nowMs)
}

function sanitizePlainText(raw, maxLen) {
  return sanitizeText(raw, maxLen)
}
