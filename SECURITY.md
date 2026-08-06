# Security policy

## Reporting a vulnerability

Do not open a public issue for a vulnerability that could expose telemetry, local files, credentials, or arbitrary code execution.

Report it through GitHub's private vulnerability reporting for [`tobi/omatrack`](https://github.com/tobi/omatrack/security/advisories/new). Include the affected revision, platform, reproduction steps, impact, and any proposed mitigation.

You should receive an acknowledgement within seven days. Please allow time for a fix and coordinated disclosure before publishing details.

## Supported versions

Omatrack is currently under active development without versioned stable releases. Security fixes target the latest `main` revision.

## Data handling

Telemetry and video inputs are treated as immutable local evidence. Omatrack does not require network access to start. Track Atlas refreshes are the only current application-managed network requests; cached metadata is used when the service is unavailable.
