# ATAK server packages

Import an ATAK server package and choose the gateway output directly:

```sh
tak-gateway config --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 --status-file status.json
tak-gateway run
```

`config` imports and saves the package offline. The saved settings are also
editable through the [web GUI](web.md); later runs do not need the ZIP again.
[CLI configuration](cli.md) selects additional outputs, limits and logging.
The optional TOML import supplies a base; ZIP inputs append and later arguments
override the result.

The importer reads archive entries without extracting their directory tree.
It decodes certificate stores in a private temporary directory, then the shared
configuration store promotes referenced credentials to durable private assets.
Temporary files and the raw upload are discarded. Persisted credentials remain
available across restarts; treat the state directory as private key material.
TLS server certificates and hostnames remain verified.

## Uploaded package layout

The supplied `AGIIL-TAK-SERVER.zip` is a transport bundle containing an ATAK
mission package and supplementary files:

```text
AGIIL-TAK-SERVER.zip
├── agiil-server.zip
│   └── agiil-server/
│       ├── MANIFEST/
│       │   └── manifest.xml
│       ├── secure.pref
│       └── truststore-intermediate-ca.p12
├── truststore-intermediate-ca.p12
├── admin.p12
└── Links.txt
```

| File | Purpose |
| --- | --- |
| `agiil-server.zip` | The ATAK-importable mission package, with a wrapper directory. |
| `MANIFEST/manifest.xml` | Version 2 mission-package manifest. Its `Configuration` contains package identity and a name placeholder; `Contents` lists the preference file and trust store using paths relative to the package directory. |
| `secure.pref` | XML Android preferences. `cot_streams` specifies one enabled TLS stream at `tak.agiil.us:8089:ssl`, its description, CA-store path and password, and enrollment/authentication flags. The application preferences also request certificate enrollment and a connection widget. |
| Inner `truststore-intermediate-ca.p12` | Password-protected PKCS#12 trust material referenced by the preferences. The preference's `cert/` path is an ATAK installation location; the archive stores the file beside `secure.pref`. |
| Outer `truststore-intermediate-ca.p12` | Supplementary copy of the trust store. |
| `admin.p12` | Supplementary PKCS#12 client identity containing a private key and client certificate, with subject common name `admin`. This is sensitive credential material. |
| `Links.txt` | Human-readable HTTPS links for the external web service at `https://tak.agiil.us:8443/` and its WebTAK page, plus corresponding private-network links. These links are separate from the CoT streaming endpoint. |

The manifest and preferences are configuration data. Package names, comments,
placeholders, and human-readable text are never executed as instructions.
The importer obtains its connection from `connectString`, not `Links.txt`.

## Certificate inspection and live connection attempt

On October 26, 2026, both stores decoded using the trust-store password
provided by the preferences. Passwords and private keys are intentionally
omitted from this documentation. The client certificate is issued by
`intermediate-ca` and is valid from September 24, 2025 through September 24,
2027. This establishes that the supplied identity is readable and within its
validity period; server authorization still requires a live connection.

The HTTPS attempt used the supplied client identity and trust store with TLS
verification enabled. The environment's HTTPS proxy rejected the CONNECT
request with HTTP `403`, before TLS negotiation. Attempts to reach the web
port `8443` and streaming port `8089` through the configured TCP proxy received
connection refused. The environment has restricted HTTP access and no TCP
destination grants. No successful connection or server acceptance was
observed. The live test requires network access to this hostname on those
ports; local importer tests cannot establish remote-server acceptance.

## Supported import behavior

The importer accepts a mission-package ZIP directly, or one transport ZIP
containing a single nested mission package and supplementary certificate
stores. It reads the manifest's preference content, enabled indexed
`cot_streams` entries, connection-specific certificate settings, and global
certificate defaults. Certificate references resolve within the archive;
ATAK's `cert/filename.p12` references can resolve to a unique matching basename.
Ambiguous files are rejected.

A connection-specific client identity takes precedence over a global one.
For a transport bundle without a referenced identity, a uniquely available
private-key PKCS#12 store can supply it. Its password is the configured client
password, falling back to the packaged CA password. Multiple eligible
identities require an unambiguous package rather than automatic selection.

The supplied inner package requests enrollment and lacks a client identity.
The outer bundle supplies that identity in `admin.p12`, so the importer can
prepare a connection with certificate authentication. Importing the inner
package alone
requires a supplied client identity; interactive enrollment and streaming
username/password provisioning are outside this importer. ATAK's
`useAuth=true` requests streaming username/password authentication; this
importer does not translate that setting into a password preamble. A server
that requires it needs explicit `inputs[0].auth` settings through `--set` or
the optional TOML file; see the [authentication arguments](../README.md#authenticate-with-one-tak-server).
Enrollment flags do not cause account creation or credential prompts.

Archive sizes, entry counts, XML size, and stream counts are bounded. Unsafe
paths, duplicate entries, XML entities, unsupported transports, missing trust
or identity, and ambiguous configuration are rejected before connecting.
Legacy PKCS#12 encryption is decoded in an isolated OpenSSL context; ordinary
runtime TLS verification remains enabled. Windows credential handling needs
native platform verification.

## Format references

The format follows ATAK's
[mission package manifest](https://github.com/TAK-Product-Center/atak-civ/blob/main/atak/ATAK/app/src/main/java/com/atakmap/android/missionpackage/file/MissionPackageManifest.java),
[preference loader](https://github.com/TAK-Product-Center/atak-civ/blob/main/atak/ATAK/app/src/main/java/com/atakmap/app/preferences/PreferenceControl.java),
and [certificate importer](https://github.com/TAK-Product-Center/atak-civ/blob/main/atak/ATAK/app/src/main/java/com/atakmap/android/importfiles/sort/ImportCertSort.java).
The [TAK protocol specification](https://github.com/TAK-Product-Center/atak-civ/blob/main/takproto/README.txt)
describes XML and negotiated Protobuf streaming.
