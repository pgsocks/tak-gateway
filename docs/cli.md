# CLI configuration

Run from an ATAK package without a configuration file:

```sh
tak-gateway run --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 --status-file status.json
```

`validate` and `redact` accept the same arguments. Validation checks local
configuration and credential references without connecting. Redaction prints
the resolved defaults and settings without passwords or private-key references.
`--help` lists the available arguments.

An optional `--config FILE` supplies the base TOML. The CLI first loads that
file, or version-1 defaults if omitted; it then appends any imported inputs,
applies the remaining options in command-line order, and validates the final
configuration. CLI settings override the base file. No source file is modified.
At least one enabled input and one enabled output are required in the result.

Without a file, relative setting paths resolve from the working directory.
With a file, all relative setting paths, including CLI credential paths,
resolve beside that TOML file. The `--config` and `--import-zip` filenames
themselves always resolve from the working directory. Absolute paths are
available when those locations differ.

## Outputs

Repeat `--output ID=URI` to select consumers:

| URI | Output behavior |
| --- | --- |
| `tcp://127.0.0.1:9000` | Listen for TCP consumers at the given bind address. Default framing is `u32be`. |
| `tcp-connect://collector.example.com:9000` | Connect to a downstream TCP consumer. |
| `udp://127.0.0.1:9001` | Send complete XML datagrams to the destination. |
| `pipe:///tmp/tak-events` | Create a private platform pipe at the absolute path. |

Output IDs match the TOML `outputs.id` field. A matching ID replaces that
entire output table; other outputs remain. A new ID appends an output.
Repeated IDs on the command line replace the previous output in place.
Replacement restores schema defaults for fields omitted by the URI. Put
advanced `--set` customizations after the corresponding `--output`.

Use brackets for IPv6, for example `local=tcp://[::1]:9000`. Ports must be
between 1 and 65535. Credentials, URL queries, fragments, and percent escapes
are not supported in these URIs. Supply literal paths as one quoted argument;
native Windows pipe names can instead use `--set` with the existing pipe
schema. Plain TCP here describes a downstream consumer, not an unencrypted
TAK-server input.

```sh
tak-gateway run --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 \
  --set 'outputs[0].max_clients=8' \
  --set 'outputs[0].framing="xml-document"' \
  --output monitor=udp://127.0.0.1:9001 \
  --status-file status.json --log-level debug
```

Indices refer to the current array after earlier CLI changes, including any
outputs loaded from TOML. Use the ID to choose replacement through `--output`
and the array index to customize its fields through `--set`.

## Gateway settings

These flags accept plain strings or decimal integers; additional TOML quoting
is unnecessary:

| Argument | Configuration field |
| --- | --- |
| `--status-file PATH` | `process.status_file` |
| `--log-level LEVEL` | `process.log_level` |
| `--log-format FORMAT` | `process.log_format` |
| `--max-message-bytes N` | `limits.max_message_bytes` |
| `--max-xml-depth N` | `limits.max_xml_depth` |
| `--retained-bytes N` | `limits.retained_bytes` |
| `--queue-bytes N` | `limits.queue_bytes` |
| `--queue-records N` | `limits.queue_records` |
| `--max-inputs N` | `limits.max_inputs` |
| `--max-outputs N` | `limits.max_outputs` |
| `--protobuf-extra-fields POLICY` | `normalization.protobuf_extra_fields` |
| `--overflow POLICY` | `delivery.overflow` |

Repeated scalar flags use the last value. The regular
[configuration schema](api.md#version-1-configuration-schema) defines accepted
values, defaults and bounds. For example:

```sh
tak-gateway run --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 \
  --status-file status.json --log-level info --log-format json \
  --max-message-bytes 1048576 --retained-bytes 16777216 \
  --queue-bytes 8388608 --queue-records 4096 --overflow drop-newest
```

## Other schema settings

`--set PATH=TOML_VALUE` provides every setting in the version-1 schema,
including input credentials, per-output queues, framing and maintenance.
Dotted names address tables and zero-based brackets address arrays. Missing
tables and arrays are created. Array elements can be appended at the next
index; sparse indices are rejected. An assignment replaces its whole value,
including a complete table or array, so unrelated sibling fields remain.

The right-hand side must be one TOML value. Quote the complete argument for
the shell, and quote string values inside it:

```sh
tak-gateway run --import-zip server-package.zip \
  --output local=tcp://127.0.0.1:9000 \
  --set 'inputs[0].protocol="negotiated"' \
  --set 'inputs[0].prefer="protobuf"' \
  --set 'inputs[0].maintenance.ping_interval_ms=30000' \
  --set 'inputs[0].maintenance.ping_timeout_ms=10000' \
  --set 'outputs[0].queue_records=1024'
```

Keep passwords in protected files or environment variables referenced through
the schema. Literal password fields are rejected by the regular parser.
Diagnostics identify an option or field without echoing its assignment value.

CLI composition accepts up to 512 option/value pairs and a bounded tree of
8192 nodes at depth 32. Malformed paths, sparse arrays, type conflicts, unknown
options and invalid final schema settings fail before network endpoints open.
