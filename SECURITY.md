# Security policy

## Reporting a vulnerability

Do not open a public issue for a vulnerability that could expose telemetry, local files, credentials, or arbitrary code execution.

Report it through GitHub's private vulnerability reporting for [`tobi/omatrack`](https://github.com/tobi/omatrack/security/advisories/new). Include the affected revision, platform, reproduction steps, impact, and any proposed mitigation.

You should receive an acknowledgement within seven days. Please allow time for a fix and coordinated disclosure before publishing details.

## Supported versions

Omatrack is currently under active development without versioned stable releases. Security fixes target the latest `main` revision.

## Data handling

Telemetry and video inputs are treated as immutable local evidence: Omatrack never rewrites, renames, deletes or writes beside them. Omatrack 2.0 makes no application-managed network requests; the Track Atlas catalog is embedded in the build. Configuration lives in `omatrack.yml` under `$XDG_CONFIG_HOME/omatrack/` and caches under `$XDG_CACHE_HOME/omatrack/`.

Untrusted telemetry files are parsed in-process by the pinned `motorsport-telemetry-rs` crates, and video is decoded by the system libmpv. Parser errors are returned as errors, never panics; a crash or hang on a crafted input file is in scope for a report.
