# Optional legacy snapshot import

Normal startup creates only the application profile. It never reads the WPF profile.

`--import-legacy <snapshot-directory>` accepts an existing sealed legacy snapshot in this compatibility layout beneath LocalAppData:

```text
MabiTools-CC-Rehearsals/
  r-<32 lowercase hexadecimal characters>/
    readiness-result.json
    source-snapshot/
      channels.csv                    (optional)
      history-preferences.json         (optional)
      history/scan-<32 hex digits>.json (optional)
```

The historical directory and verification filename are retained for compatibility with existing snapshots. The verification JSON must have a `profile` object with `SourceUnchanged: true`, `SnapshotUnchanged: true` and the actual integer `ArchivedScans` count. Import checks paths, sizes, schemas, record count and unchanged file bytes. It rejects links and existing destination data. Do not manufacture a verification record for a live profile or unverified copy.

The destination is `MabiTools-CC-Legacy-Import/MabiTools-ChannelChecker` under LocalAppData. `--legacy-profile` opens that separate profile. Original reports in `wpf-history-archive` are read-only, and do not enter native scan recommendations or retention. Neither import nor uninstall deletes the source snapshot. Snapshot creation tooling is intentionally outside this C++ application.
