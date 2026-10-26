#!/usr/bin/env python3
"""Initialize persistent development credentials and export a gateway identity."""
import argparse
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import stat
import tempfile
import time


class BootstrapError(Exception):
    """A bootstrap failure whose message contains no secret values."""


SETTINGS = {
    "POSTGRES_DB": "cot",
    "POSTGRES_USER": "martiuser",
    "POSTGRES_URL": "jdbc:postgresql://takdb:5432/cot",
    "CA_NAME": "gateway-fixture-ca",
    "STATE": "Test",
    "CITY": "Test",
    "ORGANIZATION": "GatewayFixture",
    "ORGANIZATIONAL_UNIT": "Development",
    "ADMIN_CERT_NAME": "gateway-admin",
    "CONFIG_MAX_HEAP": "512",
    "MESSAGING_MAX_HEAP": "1024",
    "API_MAX_HEAP": "1024",
    "PLUGIN_MANAGER_MAX_HEAP": "512",
}
PASSWORDS = ("POSTGRES_PASSWORD", "CA_PASS", "TAKSERVER_CERT_PASS",
             "ADMIN_CERT_PASS")


def _directory(path, mode):
    path = Path(path)
    if path.is_symlink():
        raise BootstrapError("Bootstrap directories must not be symlinks")
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(mode)
    return path


def _read_file(path, private=False, maximum=8 * 1024 * 1024):
    flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as source:
        info = os.fstat(source.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_size > maximum or
                (private and info.st_mode & 0o077)):
            raise BootstrapError("Invalid bootstrap credential file")
        data = source.read(maximum + 1)
    if len(data) > maximum:
        raise BootstrapError("Bootstrap credential file exceeds its limit")
    return data


def _atomic_write(path, data, mode, exclusive=False, owner=None):
    descriptor, temporary = tempfile.mkstemp(prefix=".bootstrap-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            os.fchmod(output.fileno(), mode)
            if owner is not None:
                os.fchown(output.fileno(), *owner)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if exclusive:
            try:
                os.link(temporary, path)
            except FileExistsError:
                pass
        else:
            os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def _read_settings(server_dir):
    raw = _read_file(Path(server_dir) / "fixture.env", private=True,
                     maximum=65536)
    try:
        values = {}
        for line in raw.decode("utf-8").splitlines():
            if not line:
                continue
            name, separator, value = line.partition("=")
            if not separator or name in values:
                raise ValueError
            values[name] = value
        if set(values) != set(SETTINGS) | set(PASSWORDS):
            raise ValueError
        if any(values[name] != value for name, value in SETTINGS.items()):
            raise ValueError
        if any(not re.fullmatch(r"[0-9a-f]{48}", values[name])
               for name in PASSWORDS):
            raise ValueError
        return values
    except (UnicodeError, ValueError):
        raise BootstrapError("Persistent server secrets are invalid") from None


def initialize_secrets(server_dir, database_dir, server_data=None,
                       database_data=None):
    server_dir = _directory(server_dir, 0o700)
    state = server_dir / "fixture.env"
    if not state.exists():
        retained = ((server_data is not None and
                     (Path(server_data) / "certs/files/root-ca.pem").exists()) or
                    (database_data is not None and
                     (Path(database_data) / "PG_VERSION").exists()))
        if retained:
            raise BootstrapError("Server secrets are missing for retained data")
        values = dict(SETTINGS)
        values.update((name, secrets.token_hex(24)) for name in PASSWORDS)
        data = "".join(name + "=" + value + "\n"
                       for name, value in values.items()).encode("ascii")
        _atomic_write(state, data, 0o600, exclusive=True)
    values = _read_settings(server_dir)
    database_dir = _directory(database_dir, 0o755)
    # The PostgreSQL entrypoint rereads this file after dropping privileges.
    _atomic_write(database_dir / "password",
                  (values["POSTGRES_PASSWORD"] + "\n").encode("ascii"), 0o444)


def export_credentials(server_data, server_secrets, output, uid, gid):
    if not 1 <= uid < 2 ** 32 - 1 or not 0 <= gid < 2 ** 32 - 1:
        raise BootstrapError("A non-root gateway UID and valid GID are required")
    values = _read_settings(server_secrets)
    files = Path(server_data) / "certs/files"
    name = values["ADMIN_CERT_NAME"]
    copies = {
        "ca.pem": _read_file(files / "root-ca.pem"),
        "client.pem": _read_file(files / (name + ".pem")),
        "client.key": _read_file(files / (name + ".key")),
        "key-password": (values["ADMIN_CERT_PASS"] + "\n").encode("ascii"),
    }
    output = _directory(output, 0o700)
    for target, data in copies.items():
        _atomic_write(output / target, data, 0o600, owner=(uid, gid))
    os.chown(output, uid, gid)


def _client_context(credentials):
    credentials = Path(credentials)
    try:
        password = _read_file(credentials / "key-password", private=True,
                              maximum=65536).decode("ascii").rstrip("\r\n")
        context = ssl.create_default_context(cafile=str(credentials / "ca.pem"))
        context.load_cert_chain(str(credentials / "client.pem"),
                                str(credentials / "client.key"), password)
        return context
    except (OSError, ValueError, UnicodeError):
        raise BootstrapError("Client TLS credentials could not be loaded") from None


def _request_version(context, host, port, timeout):
    # HTTPSConnection connects directly; inherited HTTP proxies are not used.
    connection = http.client.HTTPSConnection(host, port, timeout=timeout,
                                            context=context)
    try:
        connection.request("GET", "/Marti/api/version/info")
        response = connection.getresponse()
        if response.length is not None and response.length > 65536:
            raise BootstrapError("Server readiness response exceeds its limit")
        body = response.read(65537)
        if len(body) > 65536:
            raise BootstrapError("Server readiness response exceeds its limit")
        return response.status, body
    finally:
        connection.close()


def wait_for_server(host, port, credentials, timeout=60.0):
    if not host or not 1 <= port <= 65535 or timeout <= 0:
        raise BootstrapError("Invalid server readiness endpoint or timeout")
    context = _client_context(credentials)
    end = time.monotonic() + timeout
    while True:
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise BootstrapError("Timed out waiting for verified server readiness")
        try:
            status, body = _request_version(context, host, port,
                                            min(5.0, remaining))
        except ssl.SSLCertVerificationError:
            raise BootstrapError("Server TLS identity verification failed") from None
        except (OSError, http.client.HTTPException):
            status, body = 0, b""
        if status in (401, 403):
            raise BootstrapError("Server rejected the client identity")
        if 200 <= status < 300:
            try:
                version = json.loads(body)
                if (not isinstance(version, dict) or
                        tuple(version.get(key) for key in
                              ("major", "minor", "patch")) != (5, 7, 14)):
                    raise ValueError
            except (ValueError, UnicodeError):
                raise BootstrapError("Server version does not match 5.7.14") from None
            return
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise BootstrapError("Timed out waiting for verified server readiness")
        time.sleep(min(1.0, remaining))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    initialize = commands.add_parser("secrets")
    initialize.add_argument("--server-dir", type=Path, required=True)
    initialize.add_argument("--database-dir", type=Path, required=True)
    initialize.add_argument("--server-data", type=Path, default=Path("/server-data"))
    initialize.add_argument("--database-data", type=Path, default=Path("/database"))
    export = commands.add_parser("credentials")
    export.add_argument("--server-data", type=Path, required=True)
    export.add_argument("--server-secrets", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--uid", type=int, required=True)
    export.add_argument("--gid", type=int, required=True)
    export.add_argument("--host", default="takserver")
    export.add_argument("--port", type=int, default=8443)
    args = parser.parse_args(argv)
    try:
        if args.command == "secrets":
            initialize_secrets(args.server_dir, args.database_dir,
                               args.server_data, args.database_data)
        else:
            export_credentials(args.server_data, args.server_secrets,
                               args.output, args.uid, args.gid)
            wait_for_server(args.host, args.port, args.output)
        return 0
    except BootstrapError as error:
        parser.exit(1, "bootstrap: " + str(error) + "\n")
    except (OSError, ValueError, UnicodeError, http.client.HTTPException):
        parser.exit(1, "bootstrap: local credential or readiness operation failed\n")


if __name__ == "__main__":
    raise SystemExit(main())
