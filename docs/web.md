# Web management

Initialize a private configuration store, then start the gateway:

```sh
tak-gateway config --web-bind 127.0.0.1:8080
tak-gateway run
```

Open `http://127.0.0.1:8080`. First setup can start with no connections or
outputs. Use Connections to add a TAK source, and Outputs to choose where
received CoT events should go. The same saved settings are used by the offline
`config` command and subsequent gateway starts. `--state-dir DIRECTORY` selects
an independent store; `TAK_GATEWAY_STATE_DIR` supplies its environment default.

The HTTP interface has no login. Prefer localhost; a selected LAN/WAN bind
allows clients reaching it to control configuration. Access it by numeric IP
or `localhost`; unrelated HTTP Host names are rejected. IPv6 binds use bracket
notation, such as `[::1]:8080`.

## Connections

The table shows configured names, endpoints, protocols, status, activity and
actions. Add or edit a connection manually, import an ATAK ZIP, enable/disable
a profile, or remove it. Stable profile IDs identify edits independently of
display names. Saving a connection reconnects that source; unrelated sources
and downstream consumers remain connected. A name-only edit does not reconnect.

The manual form includes endpoint and server-certificate identity, protocol,
UID/callsign, trust material, PEM certificate/key or PKCS#12 identity, streaming
authentication and advanced maintenance settings. Upload credential files or
use the advanced file/environment references for existing server-side files.
Blank password inputs retain their existing protected values. Use explicit
replacement/removal controls to change secrets; their contents are never
returned to the browser.

**Verify server hostname** defaults on for manual connections. ATAK ZIP imports
leave it off to match ATAK certificate trust: the server certificate must pass
CA-chain, validity, and purpose checks without matching its name to the
connection address. **Certificate server name** selects SNI and, when hostname
verification is enabled, the expected certificate identity. Saved profiles keep
this selection through edits and restarts.

ZIP import validates the archive and displays an editable preview before
confirmation. It supports the existing standard and nested ATAK package
formats. Package passwords and client keys are used privately, not displayed.
Enrollment-only packages need a supplied client identity; interactive
certificate enrollment is not implemented. Uploaded ZIPs are not retained.

## Outputs

Configure TCP listener/connect, UDP or platform pipe destinations. The form
shows fields applicable to the selected kind, including framing, client limits,
queue budgets and datagram limits. An output edit restarts only that output
and its clients. Queued records for a replaced or removed output may be lost;
delivery remains live and best effort, without durable replay.

Empty or disabled endpoint collections are valid in managed mode. With no
outputs, incoming events have no consumer; configure an output to forward them.
The public library's normal configuration parser retains its strict enabled
input/output requirement.

## Status and failure evidence

Status refreshes approximately once per second. Loss of the HTTP connection
marks displayed data stale instead of retaining an apparently current success.
Details show the current diagnostic and a bounded history of recent attempts:

- DNS resolution and native socket errors, including refused connections.
- TLS verification results, such as an expired/untrusted server certificate
  or a hostname mismatch.
- Explicit peer TLS alerts rejecting a client certificate or denying access.
- Credential-loading, framing, protocol and timeout errors.
- Attempt, failure and retry timestamps, protocol offers and event counters.

The details distinguish the gateway rejecting a server certificate from the
server rejecting the gateway's client certificate. Sending credentials is
not proof of authentication. Some TAK servers close the stream on rejected
credentials without providing a reason; that is shown as an unconfirmed
authentication/authorization outcome, not a guessed incorrect password.
Quiet streams alone are not failures. No CoT event bodies or secret contents
are exposed by management status.

## Persistence and concurrent edits

Configuration and uploaded credentials live under the private state directory.
Immutable revisions are published through an atomically replaced `CURRENT`
pointer. Failed edits do not replace the last committed revision. Treat the
entire directory as credential material; it contains private keys and protected
password files, even though configuration documents contain references only.

The running process holds a writer lock. Offline CLI edits require it to stop;
read-only inspection can continue. Browser changes carry a revision token, so
an edit from an outdated window is rejected instead of overwriting a newer
change. Review the latest settings and resubmit. The UI distinguishes saved
settings from active settings if runtime application fails.

HTTP handlers use bounded request queues and snapshots. Certificate processing
runs outside the runtime polling thread. The controller serializes endpoint
changes with polling, retaining unrelated sockets and queues. Browser writes
require the same-origin management token; CORS is disabled.

For Docker environment initialization and persistent mounts, see the
[Compose guide](../docker/README.md).
