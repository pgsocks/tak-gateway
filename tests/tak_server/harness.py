#!/usr/bin/env python3
"""Build and exercise an official TAK Server with verified mTLS.

Uses only Python's standard library and the Docker CLI. No gateway behavior is
simulated. See README.md for the tested server revision and scope.
"""
import argparse
from collections import deque
from contextlib import contextmanager
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import uuid
import xml.etree.ElementTree as ET
import xml.parsers.expat

ROOT = Path(__file__).resolve().parent
MAX_MESSAGE_BYTES = 8388608
MAX_REST_BYTES = 1048576
DEFAULT_IMAGE = 'tak-gateway-fixture:5187abd46d82'
REVISION = '5187abd46d827d37cfc5708805eced197a837e49'


class HarnessError(RuntimeError):
    """A fixture or verification failure, without payload/secret contents."""


def deadline(seconds):
    return time.monotonic() + seconds


def remaining(end):
    value = end - time.monotonic()
    if value <= 0:
        raise HarnessError('Timed out waiting for fixture progress')
    return value


def parse_event(data):
    if b'<!DOCTYPE' in data or b'<!ENTITY' in data:
        raise HarnessError('DTD/entity declaration in XML event')
    try:
        text = data.decode('utf-8')
        root = ET.fromstring(text)
    except (UnicodeDecodeError, ET.ParseError) as error:
        raise HarnessError('Invalid UTF-8 XML event') from error
    if root.tag != 'event':
        raise HarnessError('XML root is not event')
    return root


class XmlEvents:
    """Bounded incremental framing for a stream of XML event documents."""
    def __init__(self, max_bytes=MAX_MESSAGE_BYTES, max_depth=128):
        self.max_bytes = max_bytes
        self.max_depth = max_depth
        self._reset()

    def _reset(self):
        self.parser = xml.parsers.expat.ParserCreate()
        if hasattr(self.parser, 'SetReparseDeferralEnabled'):
            self.parser.SetReparseDeferralEnabled(False)
        self.parser.StartElementHandler = self._start
        self.parser.EndElementHandler = self._end
        self.parser.StartDoctypeDeclHandler = self._forbidden
        self.parser.EntityDeclHandler = self._forbidden
        self.parser.ExternalEntityRefHandler = self._forbidden
        self.depth = 0
        self.done = False
        self.data = bytearray()

    def _forbidden(self, *args):
        raise HarnessError('DTD/entities forbidden in XML stream')

    def _start(self, name, attrs):
        if self.depth == 0 and name != 'event':
            raise HarnessError('XML stream root is not event')
        self.depth += 1
        if self.depth > self.max_depth:
            raise HarnessError('XML nesting limit exceeded')

    def _end(self, name):
        self.depth -= 1
        self.done = self.depth == 0

    def feed(self, data):
        events = []
        for byte in data:
            if not self.data and byte in b' \t\r\n':
                continue
            self.data.append(byte)
            if len(self.data) > self.max_bytes:
                raise HarnessError('XML message size limit exceeded')
            try:
                self.parser.Parse(bytes([byte]), False)
            except xml.parsers.expat.ExpatError as error:
                raise HarnessError('Malformed XML stream') from error
            if self.done:
                result = bytes(self.data)
                parse_event(result)
                events.append(result)
                self._reset()
        return events


def is_offer(data):
    root = parse_event(data)
    return (root.get('type') == 't-x-takp-v' and
            root.find('./detail/TakControl/TakProtocolSupport') is not None)


def is_mission_create(data, name):
    root = parse_event(data)
    mission_element = root.find('./detail/mission')
    return (root.get('type') == 't-x-m-n' and mission_element is not None and
            mission_element.get('name') == name and
            mission_element.get('type') == 'CREATE')


def read_exact(sock, count, end):
    chunks = bytearray()
    while len(chunks) < count:
        sock.settimeout(remaining(end))
        try:
            data = sock.recv(count - len(chunks))
        except (TimeoutError, OSError) as error:
            raise HarnessError('Socket receive failed or timed out') from error
        if not data:
            raise HarnessError('Unexpected EOF before complete record')
        chunks.extend(data)
    return bytes(chunks)


def read_record(sock, end):
    length, = struct.unpack('!I', read_exact(sock, 4, end))
    if not 0 < length <= MAX_MESSAGE_BYTES:
        raise HarnessError('Invalid u32be XML record length')
    result = read_exact(sock, length, end)
    parse_event(result)
    return result


class XmlSubscriber:
    def __init__(self, sock):
        self.sock = sock
        self.parser = XmlEvents()
        self.pending = deque()

    def wait(self, predicate, end):
        while True:
            while self.pending:
                event = self.pending.popleft()
                if predicate(event):
                    return event
            self.sock.settimeout(remaining(end))
            try:
                data = self.sock.recv(65536)
            except (TimeoutError, OSError) as error:
                raise HarnessError('XML subscriber receive failed') from error
            if not data:
                raise HarnessError('XML subscriber closed before expected event')
            self.pending.extend(self.parser.feed(data))


class _VerifiedHTTPS(http.client.HTTPSConnection):
    def connect(self):
        raw = socket.create_connection((self.host, self.port), self.timeout)
        try:
            self.sock = self._context.wrap_socket(raw, server_hostname='takserver')
        except BaseException:
            raw.close()
            raise


class RestClient:
    def __init__(self, host, port, context):
        self.host, self.port, self.context = host, port, context

    def _connection(self):
        return _VerifiedHTTPS(self.host, self.port, timeout=5,
                              context=self.context)

    def request(self, method, path):
        connection = self._connection()
        try:
            connection.request(method, path, headers={'Accept': 'application/json'})
            response = connection.getresponse()
            body = response.read(MAX_REST_BYTES + 1)
            if len(body) > MAX_REST_BYTES:
                raise HarnessError('REST response exceeds size limit')
            if method == 'DELETE' and response.status == 404:
                return b''
            if not 200 <= response.status < 300:
                raise HarnessError(f'REST {method} failed: HTTP {response.status}')
            return body
        except (OSError, http.client.HTTPException) as error:
            raise HarnessError(f'REST {method} transport failed') from error
        finally:
            connection.close()


@contextmanager
def mission(rest, name, creator):
    path = '/Marti/api/missions/' + urllib.parse.quote(name, safe='')
    query = urllib.parse.urlencode({'creatorUid': creator, 'group': '__ANON__'})
    failure = None
    try:
        # Cleanup remains mandatory if the request committed before a timeout.
        rest.request('PUT', path + '?' + query)
        yield
    except BaseException as error:
        failure = error
        raise
    finally:
        try:
            rest.request('DELETE', path + '?' + urllib.parse.urlencode(
                {'creatorUid': creator}))
        except BaseException as error:
            if failure:
                raise HarnessError(f'{failure}; mission cleanup failed: {error}') \
                    from failure
            raise HarnessError(f'Mission cleanup failed: {error}') from error


def read_settings(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines()
                if line and not line.startswith('#'))


def private_write(path, content):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as file:
        file.write(content)


def new_state(path, image):
    path = path.resolve()
    if path.exists() and any(path.iterdir()):
        raise HarnessError('State directory must be empty; refusing overwrite')
    path.mkdir(parents=True, mode=0o700, exist_ok=True)
    path.chmod(0o700)
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/@:-]*', image):
        raise HarnessError('Invalid fixture image reference')
    state = {'project': 'takgw-' + uuid.uuid4().hex[:12], 'image': image,
             'revision': REVISION}
    values = {
        'POSTGRES_DB': 'cot', 'POSTGRES_USER': 'martiuser',
        'POSTGRES_PASSWORD': secrets.token_hex(24),
        'POSTGRES_URL': 'jdbc:postgresql://takdb:5432/cot',
        'CA_NAME': 'gateway-fixture-ca', 'CA_PASS': secrets.token_hex(24),
        'STATE': 'Test', 'CITY': 'Test', 'ORGANIZATION': 'GatewayFixture',
        'ORGANIZATIONAL_UNIT': 'Development',
        'TAKSERVER_CERT_PASS': secrets.token_hex(24),
        'ADMIN_CERT_NAME': 'gateway-admin',
        'ADMIN_CERT_PASS': secrets.token_hex(24),
        'CONFIG_MAX_HEAP': '512', 'MESSAGING_MAX_HEAP': '1024',
        'API_MAX_HEAP': '1024', 'PLUGIN_MANAGER_MAX_HEAP': '512',
        'TAK_SERVER_IMAGE': image,
    }
    private_write(path / 'fixture.env', ''.join(
        f'{name}={value}\n' for name, value in values.items()))
    private_write(path / 'state.json', json.dumps(state))
    return state


def load_state(path):
    try:
        state = json.loads((path / 'state.json').read_text())
    except (OSError, ValueError) as error:
        raise HarnessError('Missing or invalid fixture state') from error
    if not re.fullmatch(r'takgw-[a-f0-9]{12}', state.get('project', '')):
        raise HarnessError('Refusing unowned Compose project')
    return state


def docker_environment():
    result = os.environ.copy()
    for name in ('DOCKER_HOST', 'DOCKER_CONTEXT', 'DOCKER_TLS',
                 'DOCKER_TLS_VERIFY', 'DOCKER_CERT_PATH'):
        result.pop(name, None)
    return result


def command(args, timeout=60, capture=True, extra_env=None):
    environment = docker_environment()
    environment.update(extra_env or {})
    try:
        result = subprocess.run(args, check=False, timeout=timeout,
                                env=environment, text=True,
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise HarnessError('Docker command failed to execute or timed out') \
            from error
    if result.returncode:
        # Server tooling can print passwords. Do not echo command output.
        raise HarnessError(f'Docker command failed with exit {result.returncode}; '
                           'inspect locally with appropriate secret redaction')
    return result.stdout or ''


def compose(path, state, args, timeout=60):
    return command(['docker', '--host=unix:///var/run/docker.sock', 'compose',
                    '--project-name', state['project'], '--env-file',
                    str(path / 'fixture.env'), '-f', str(ROOT / 'compose.yaml'),
                    *args], timeout=timeout, extra_env={
                        "TAK_FIXTURE_ENV": str(path / "fixture.env"),
                        "TAK_SERVER_IMAGE": state["image"],
                    })


def mapped_port(output):
    try:
        value = int(output.strip().rsplit(':', 1)[1])
    except (ValueError, IndexError) as error:
        raise HarnessError('Missing Docker published port') from error
    if not 1 <= value <= 65535:
        raise HarnessError('Invalid Docker published port')
    return value


def build(image):
    command(['docker', '--host=unix:///var/run/docker.sock', 'info'])
    ca = os.environ.get('TAK_BUILD_CA_BUNDLE',
                        '/etc/ssl/certs/ca-certificates.crt')
    command(['docker', '--host=unix:///var/run/docker.sock', 'build',
             '--secret', 'id=proxy_ca,src=' + ca,
             '--file', str(ROOT / 'Dockerfile.source'), '--tag', image,
             str(ROOT)], timeout=3600, capture=False)


def up(path, image, skip_build=False):
    state = new_state(path, image)
    try:
        if not skip_build:
            build(image)
        else:
            command(['docker', '--host=unix:///var/run/docker.sock', 'info'])
        print('Starting isolated official TAK Server fixture', flush=True)
        compose(path, state, ['up', '-d', '--wait', '--wait-timeout', '600'],
                timeout=660)
        compose(path, state, ['cp',
            'takserver:/opt/tak/fixture-provenance.json',
            str(path / 'provenance.json')])
        verify_provenance(json.loads((path / 'provenance.json').read_text()))
        container_id = compose(path, state, ['ps', '-q', 'takserver']).strip()
        state['image_id'] = command(['docker',
            '--host=unix:///var/run/docker.sock', 'inspect',
            '--format', '{{.Image}}', container_id]).strip()
        client = path / 'client'
        client.mkdir(mode=0o700)
        for source, target in (('root-ca.pem', 'ca.pem'),
                               ('gateway-admin.pem', 'client.pem'),
                               ('gateway-admin.key', 'client.key')):
            compose(path, state, ['cp', 'takserver:/opt/tak/certs/files/' + source,
                                  str(client / target)])
            (client / target).chmod(0o600)
        settings = read_settings(path / 'fixture.env')
        private_write(client / 'key-password', settings['ADMIN_CERT_PASS'])
        state['tls_port'] = mapped_port(compose(
            path, state, ['port', 'takserver', '8089']))
        state['rest_port'] = mapped_port(compose(
            path, state, ['port', 'takserver', '8443']))
        (path / 'state.json').write_text(json.dumps(state))
        rest, context = clients(path, state)
        wait_rest(rest, deadline(60))
        print('Fixture ready: verified mTLS REST; streaming configured',
              flush=True)
        return state
    except BaseException as error:
        try:
            down(path)
        except BaseException as cleanup:
            raise HarnessError(f'{error}; Docker cleanup failed: {cleanup}') \
                from error
        raise


def down(path):
    state = load_state(path)
    compose(path, state, ['down', '--volumes', '--remove-orphans',
                          '--timeout', '30'], timeout=90)


def verify_provenance(provenance):
    if (provenance.get('revision') != REVISION or
            provenance.get('tag') != '5.7-RELEASE-14' or
            provenance.get('gradle_version') != '8.10.2' or
            not re.fullmatch(r'[a-f0-9]{64}',
                             provenance.get('archive_sha256', ''))):
        raise HarnessError('Fixture image lacks expected pinned source provenance')


def clients(path, state):
    settings = read_settings(path / 'fixture.env')
    context = ssl.create_default_context(cafile=str(path / 'client/ca.pem'))
    context.load_cert_chain(str(path / 'client/client.pem'),
                            str(path / 'client/client.key'),
                            password=settings['ADMIN_CERT_PASS'])
    return RestClient('127.0.0.1', state['rest_port'], context), context


def wait_rest(rest, end):
    while True:
        try:
            body = rest.request('GET', '/Marti/api/version/info')
        except HarnessError as error:
            if 'HTTP 401' in str(error) or 'HTTP 403' in str(error):
                raise
            time.sleep(min(1, remaining(end)))
            continue
        try:
            result = json.loads(body)
        except (UnicodeDecodeError, ValueError) as error:
            raise HarnessError('Invalid REST version response') from error
        if (not isinstance(result, dict) or
                tuple(result.get(key) for key in ('major', 'minor', 'patch'))
                != (5, 7, 14)):
            raise HarnessError('REST version does not match pinned TAK release')
        return result


def connect_tls(state, context):
    raw = socket.create_connection(('127.0.0.1', state['tls_port']), timeout=10)
    try:
        return context.wrap_socket(raw, server_hostname='takserver')
    except BaseException:
        raw.close()
        raise


def gateway_config(path, state, port, protocol="xml"):
    if protocol not in ("xml", "protobuf"):
        raise HarnessError("Unsupported gateway protocol")
    def quoted(value):
        return json.dumps(str(value))
    config = path / 'gateway.toml'
    text = f'''version = 1
[process]
status_file = {quoted(path / 'gateway-status.json')}
[[inputs]]
id = "fixture"
kind = "tls"
host = "127.0.0.1"
port = {state['tls_port']}
protocol = "xml-only"
uid = "gateway-fixture-receiver"
[inputs.tls]
server_name = "takserver"
ca_file = {quoted(path / 'client/ca.pem')}
cert_file = {quoted(path / 'client/client.pem')}
key_file = {quoted(path / 'client/client.key')}
key_password_file = {quoted(path / 'client/key-password')}
[inputs.auth]
mode = "none"
[[outputs]]
id = "capture"
kind = "tcp"
mode = "listen"
bind = "127.0.0.1"
port = {port}
framing = "u32be"
'''
    if protocol == 'protobuf':
        text = text.replace('protocol = "xml-only"',
                            'protocol = "negotiated"\nprefer = "protobuf"')
    config.write_text(text)
    config.chmod(0o600)
    return config


def gateway_ready(snapshot, protocol="xml"):
    expected = "PROTOBUF_ACTIVE" if protocol == "protobuf" else "XML_ACTIVE"
    inputs = snapshot.get('inputs', [])
    outputs = snapshot.get('outputs', [])
    return (any(value.get('id') == 'fixture' and
                value.get('state') == expected and
                value.get('protocol_offer_received') is True for value in inputs) and
            any(value.get('id') == 'capture' and
                value.get('available') is True for value in outputs))


def validate_gateway(binary):
    path = Path(binary).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise HarnessError('Gateway executable is absent or not executable')
    return str(path)


def stop_gateway(process):
    status = process.poll()
    if status is not None:
        if status != 0:
            raise HarnessError(f'Gateway exited with status {status}')
        return
    process.terminate()
    try:
        status = process.wait(timeout=10)
        if status != 0:
            raise HarnessError(f'Gateway termination returned status {status}')
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        raise HarnessError('Gateway required forced termination')


@contextmanager
def managed_gateway(binary, path, state, protocol="xml"):
    binary = validate_gateway(binary)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    config = gateway_config(path, state, port, protocol)
    (path / 'gateway-status.json').unlink(missing_ok=True)
    # Logs may contain payloads or secrets from an unfinished gateway: private.
    with open(path / 'gateway.log', 'wb') as logs:
        os.chmod(path / 'gateway.log', 0o600)
        process = subprocess.Popen([binary, 'run', '--config', str(config)],
                                   stdout=logs, stderr=logs)
        failure = None
        try:
            end = deadline(60)
            while True:
                if process.poll() is not None:
                    raise HarnessError('Gateway exited before readiness')
                try:
                    snapshot = json.loads((path / 'gateway-status.json').read_text())
                except (OSError, ValueError):
                    snapshot = {}
                if gateway_ready(snapshot, protocol):
                    try:
                        collector = socket.create_connection(('127.0.0.1', port),
                                                             timeout=1)
                        break
                    except OSError:
                        pass
                time.sleep(min(0.1, remaining(end)))
            with collector:
                yield collector, process
        except BaseException as error:
            failure = error
            raise
        finally:
            try:
                stop_gateway(process)
            except BaseException as cleanup:
                if failure:
                    raise HarnessError(f'{failure}; gateway cleanup failed: '
                                       f'{cleanup}') from failure
                raise


def smoke(path, gateway=None, gateway_protocol="xml"):
    state = load_state(path)
    rest, context = clients(path, state)
    wait_rest(rest, deadline(60))
    name = 'gateway-fixture-' + uuid.uuid4().hex
    creator = 'rest-fixture-' + uuid.uuid4().hex
    if gateway:
        with managed_gateway(gateway, path, state, gateway_protocol) as (collector, process):
            with mission(rest, name, creator):
                end = deadline(30)
                while True:
                    if process.poll() is not None:
                        raise HarnessError('Gateway exited before test event')
                    event = read_record(collector, end)
                    if is_mission_create(event, name):
                        break
        print('PASS: REST mission creation reached gateway u32be XML output; '
              'mission cleaned up', flush=True)
        return
    with connect_tls(state, context) as sock:
        subscriber = XmlSubscriber(sock)
        subscriber.wait(is_offer, deadline(30))
        print('Authenticated XML/TLS subscription ready; triggering mission',
              flush=True)
        with mission(rest, name, creator):
            subscriber.wait(lambda data: is_mission_create(data, name),
                            deadline(30))
    print('PASS: REST mission creation reached authenticated XML/TLS subscriber; '
          'mission cleaned up', flush=True)


def interrupt(signum, frame):
    raise HarnessError(f'Interrupted by signal {signum}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'run', 'up', 'smoke', 'down'))
    parser.add_argument('--state-dir', type=Path)
    parser.add_argument('--image', default=DEFAULT_IMAGE)
    parser.add_argument('--skip-build', action='store_true',
                        help='Use an already source-built fixture image')
    parser.add_argument('--gateway', help='Optional gateway executable to test')
    parser.add_argument('--gateway-protocol', choices=('xml', 'protobuf'),
                        default='xml', help='Gateway ingress mode to verify')
    args = parser.parse_args(argv)
    if args.gateway_protocol != 'xml' and not args.gateway:
        parser.error('--gateway-protocol requires --gateway')
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, interrupt)
    try:
        if args.gateway:
            validate_gateway(args.gateway)
        if args.action == 'build':
            build(args.image)
        elif args.action == 'run':
            if args.state_dir:
                parser.error('run uses disposable state; use up for persistent state')
            with tempfile.TemporaryDirectory(prefix='takgw-') as directory:
                path = Path(directory)
                state = up(path, args.image, args.skip_build)
                failure = None
                try:
                    smoke(path, args.gateway, args.gateway_protocol)
                except BaseException as error:
                    failure = error
                    raise
                finally:
                    try:
                        down(path)
                    except BaseException as error:
                        raise HarnessError(f'{failure or "Verification passed"}; '
                                           f'Docker cleanup failed: {error}') \
                            from error
        else:
            if args.state_dir is None:
                parser.error('--state-dir is required for up, smoke, and down')
            path = args.state_dir.resolve()
            if args.action == 'up':
                up(path, args.image, args.skip_build)
            elif args.action == 'smoke':
                smoke(path, args.gateway, args.gateway_protocol)
            else:
                down(path)
        return 0
    except (HarnessError, OSError, ssl.SSLError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
