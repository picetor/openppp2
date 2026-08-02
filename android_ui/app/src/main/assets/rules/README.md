# Android bundled rule assets

> [Android overview](../../../../../../README.md) · [Technical guide](../../../../../../debug.md)

**Status:** Status-bound maintenance note

**Type:** Bundled binary asset inventory

**Last verified:** 2026-08-01

This directory contains the Android APK assets consumed by `PppVpnService.ensureGeoRulesAssets()`.

## Runtime behavior

When `PppVpnService` is created, it ensures that these bundled assets are available in the application's private files directory:

| APK asset | Runtime destination |
|---|---|
| `rules/geoip.dat` | `files/rules/GeoIP.dat` |
| `rules/geosite.dat` | `files/rules/GeoSite.dat` |

The service creates `files/rules` when needed. If a destination is missing, empty, or is mistakenly a directory, it replaces it with the bundled file. A non-empty regular destination file is retained. Failures are logged by the service; this routine does not establish a network download contract.

The service also sets the native root path to Android `filesDir`, allowing relative runtime configuration paths such as `./rules/GeoIP.dat` and `./rules/GeoSite.dat` to resolve to the extracted files.

## Maintenance rules

- Keep the APK filenames lowercase: `geoip.dat` and `geosite.dat`.
- Preserve the binary format expected by the native runtime; do not replace a data file with a directory, text placeholder, or compressed archive.
- Before refreshing either file, record the source, retrieval date/version or revision when available, and a checksum in the change record. The repository does not currently contain a verified provenance/version manifest for these binaries.
- Review the applicable upstream terms before redistributing changed data. This page does not make a licensing or provenance determination.
- Test an Android startup that reaches `PppVpnService.onCreate()` after a refresh. A file's presence in the APK alone does not validate native parsing, routing, or connection behavior.

## Change record

| Date | Asset | Source | Revision / date | Size (bytes) | MD5 |
|---|---|---|---|---|---|
| 2026-08-01 | `geoip.dat` | [v2fly/geoip](https://github.com/v2fly/geoip) @ `release` | 2026-08-01 download | 22,765,206 | `449C8D49E23EC741B8475C7854CDDBB9` |
| 2026-08-01 | `geosite.dat` | [Loyalsoldier/v2ray-rules-dat](https://github.com/Loyalsoldier/v2ray-rules-dat) @ `release` | 2026-08-01 download | 10,540,162 | `E20B8DE1A072F1833C1BB468C7D9E772` |
| 2026-07-22 | `geoip.dat` (previous) | [MetaCubeX/meta-rules-dat](https://github.com/MetaCubeX/meta-rules-dat) @ `release` | — | 19,306,491 | `C5059175CD4ED5360B0F04A276471E24` |

### 2026-08-01 geo data refresh

Replaced both bundled rule assets because the previous MetaCubeX dataset had a
coverage gap in its `cn` GeoIP category that caused domestic traffic to be
misrouted through the tunnel:

- **Root cause:** `MetaCubeX/meta-rules-dat` `geoip.dat` (identical to
  `Loyalsoldier/v2ray-rules-dat` `geoip.dat`) classifies only 5,822 `cn`
  networks and omits the Baidu ranges `180.76.0.0/20` (and `180.76.0.0/14`).
  As a result `180.76.11.230` (baidu.com) fell through to `final: tunnel`,
  so domestic sites appeared to come from a foreign IP.
- **Fix:** switched `geoip.dat` to the official
  [v2fly/geoip](https://github.com/v2fly/geoip) dataset (`cn` = 14,841
  networks, includes `180.76.0.0/14`). `180.76.11.230` and `180.76.0.1` now
  match `geoip,cn`.
- **GeoSite:** switched to
  [Loyalsoldier/v2ray-rules-dat](https://github.com/Loyalsoldier/v2ray-rules-dat)
  `geosite.dat` (`cn` = 111,912 domain entries; the previous MetaCubeX file had
  114,954; both cover baidu.com/qq.com/taobao.com etc.).
- **Verification:** `tests/tools/geo_verify/` scripts parse the v2ray
  protobuf wire format and confirm: `geoip_v2fly.dat` matches `180.76.11.230`
  -> `cn 180.76.0.0/14`; `geosite_loyalsoldier.dlc` `cn` category contains
  111,912 entries including `baidu.com`. Files verified with the native
  `GeoRuleEngine` parser logic (fields 1/2/3, `SiteDomain.type` 0/1 supported).
- **Caution:** Android `PppVpnService.ensureGeoRulesAssets()` keeps an
  existing non-empty `files/rules/GeoIP.dat`/`GeoSite.dat`. After upgrading,
  delete the old copies on the device (or reinstall the APK) so the new
  bundled files are copied on next service start.

For the Android client lifecycle and diagnostics, see the [technical guide](../../../../../../debug.md).
