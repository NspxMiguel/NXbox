# NXbox over-the-air updates

Push a stable tag such as `v1.0.0` or `v1.0.0.1` to run `release-nxbox.yml`.
Tags map directly to four-part package versions (a missing revision becomes zero).
Every component is 0–65535; major must be at least 1 so releases supersede the
existing 0.3 diagnostic packages. Versions must exceed all published stable
release versions, including releases on older API pages. Prerelease tags are
rejected. Rerunning an already published version intentionally fails.

The workflow calls `build-nxbox.yml`, then `package-nxbox.yml`, at the tagged
commit. The package job consumes that run's artifact, stages the same game/Mesa/
DXIL payload as diagnostic packaging, and uses the same `NXBOX_PFX_BASE64` and
`NXBOX_PFX_PASSWORD` signing secrets. Keep the existing identity and signing
certificate to preserve LocalState. The existing pinned Mesa build remains the
runtime source. The release publishes `NXbox_game_<version>_x64.appx` and
`build-info.json` with package version, source revisions and `diagnosticOnly: false`.
Only the publication job has `contents: write`. Existing diagnostic triggers
and trusted main-build validation remain in place.

At library startup an MTA worker checks GitHub's `/releases/latest` endpoint.
It rejects drafts, prereleases, malformed versions, and missing/mismatched
package assets, and compares all four components with the installed package.
Offline checks, API errors and rate limits leave the library usable. No
credentials are sent to GitHub. There is no automatic installation.

To install, move up to the navigation tabs, right past Settings to the update
pill, and press A. The worker first reads `LocalState/portal.json`, using the
same schema as Nativra:

```json
{"host": "192.168.1.100", "port": 11443, "user": "portal-user", "pass": "portal-password"}
```

The download streams to TemporaryFolder in 256 KiB chunks, reports percentage,
and checks the received size against the release asset size. The temporary
package is removed after an attempt (or replaced on a later attempt if the
console terminated the process). During deployment, game launches and other
library actions are blocked. Errors allow A to retry or B to return.

Installation follows Nativra's `ConsolePortal.InstallAsync`: HTTPS Basic auth,
self-signed certificate exceptions limited to the portal client, GET
`/api/os/machinename`, CSRF from Set-Cookie or the protocol filter's cookie jar,
and multipart POST to `/api/app/packagemanager/package?package=<name>` with both
`X-CSRF-Token` and the CSRF cookie. HTTP 409 retries up to 30 times, five seconds
apart. Redirects are disabled on the credential-bearing client. Worker teardown
cancels pending operations; the render thread performs no network I/O.

The sheet announces restart before deployment and again when the portal accepts
it. Acceptance is not verification of the final deployment or a guaranteed
relaunch: the console owns replacement of the running package. NXbox does not
force a separate restart or offer game launch while that replacement is pending.

## Validation limits

Nativra's current `Updater.cs` explicitly documents that UWP loopback isolation
blocked its console's Device Portal and therefore uses PackageManager first.
NXbox implements the requested portal route only. Its existing
`privateNetworkClientServer` capability does not establish that loopback access
will succeed. Test this on the console; a blocked connection produces a retryable
error and an HRESULT in `eden_uwp_diag.txt`, without logging credentials.

Python version-policy and existing staging tests run without Windows. The C++
version-parser test is wired into the existing CI CTest run. The app and its
WinRT networking calls need the Windows MSVC build and real-console verification;
no local app compilation or device deployment was performed for this change.
