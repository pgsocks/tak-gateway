"""Offline contracts for the combined Compose stack's bootstrap helpers."""
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import ssl
import tempfile
import unittest
from unittest import mock

import bootstrap as b


VERSION = json.dumps({'major': 5, 'minor': 7, 'patch': 14}).encode()


def settings(path):
    return dict(shlex.split(line, posix=True)[0].split('=', 1)
                for line in path.read_text().splitlines() if line)


class BootstrapFiles(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.server = self.root / 'server-secrets'
        self.database = self.root / 'database-secrets'
        self.server_data = self.root / 'server-data'
        self.database_data = self.root / 'database-data'

    def initialize(self):
        return b.initialize_secrets(self.server, self.database,
                                    self.server_data, self.database_data)

    def test_fresh_secrets_match_the_server_and_database_configuration(self):
        self.initialize()
        values = settings(self.server / 'fixture.env')
        for name, value in {
                'POSTGRES_DB': 'cot', 'POSTGRES_USER': 'martiuser',
                'POSTGRES_URL': 'jdbc:postgresql://takdb:5432/cot',
                'ADMIN_CERT_NAME': 'gateway-admin',
                'CONFIG_MAX_HEAP': '512', 'MESSAGING_MAX_HEAP': '1024',
                'API_MAX_HEAP': '1024', 'PLUGIN_MANAGER_MAX_HEAP': '512',
        }.items():
            self.assertEqual(values[name], value)
        passwords = [values[name] for name in (
            'POSTGRES_PASSWORD', 'CA_PASS', 'TAKSERVER_CERT_PASS',
            'ADMIN_CERT_PASS')]
        self.assertTrue(all(re.fullmatch('[a-f0-9]{48}', password)
                            for password in passwords))
        self.assertEqual(len(set(passwords)), 4)
        self.assertEqual((self.database / 'password').read_text().strip(),
                         values['POSTGRES_PASSWORD'])
        self.assertEqual(self.server.stat().st_mode & 0o777, 0o700)
        self.assertEqual((self.server / 'fixture.env').stat().st_mode & 0o777,
                         0o600)
        self.assertEqual(self.database.stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.database / 'password').stat().st_mode & 0o777,
                         0o444)

    def test_restart_preserves_secrets_and_repairs_a_missing_database_file(self):
        self.initialize()
        original = (self.server / 'fixture.env').read_bytes()
        password = (self.database / 'password').read_bytes()
        self.initialize()
        self.assertEqual((self.server / 'fixture.env').read_bytes(), original)
        (self.database / 'password').unlink()
        self.initialize()
        self.assertEqual((self.server / 'fixture.env').read_bytes(), original)
        self.assertEqual((self.database / 'password').read_bytes(), password)

    def test_database_password_is_restored_from_the_authoritative_state(self):
        self.initialize()
        password = (self.database / 'password').read_bytes()
        (self.database / 'password').chmod(0o600)
        (self.database / 'password').write_text('incorrect-password\n')
        self.initialize()
        self.assertEqual((self.database / 'password').read_bytes(), password)
        self.assertEqual((self.database / 'password').stat().st_mode & 0o777,
                         0o444)

    def test_corrupt_existing_state_is_not_replaced(self):
        self.initialize()
        state = self.server / 'fixture.env'
        corrupt = state.read_text().replace('POSTGRES_DB=cot',
                                            'POSTGRES_DB=other')
        state.write_text(corrupt)
        password = (self.database / 'password').read_bytes()
        with self.assertRaises(b.BootstrapError):
            self.initialize()
        self.assertEqual(state.read_text(), corrupt)
        self.assertEqual((self.database / 'password').read_bytes(), password)

    def test_missing_and_duplicate_fields_are_rejected(self):
        self.initialize()
        state = self.server / 'fixture.env'
        original = state.read_text()
        for corrupt in (
                '\n'.join(line for line in original.splitlines()
                          if not line.startswith('CA_PASS=')) + '\n',
                original + 'POSTGRES_DB=cot\n',
                original + 'UNEXPECTED=value\n'):
            with self.subTest(corrupt=corrupt[:20]):
                state.write_text(corrupt)
                with self.assertRaises(b.BootstrapError):
                    self.initialize()

    def test_missing_secrets_with_retained_pki_or_database_fail(self):
        for relative, is_server in (
                ('certs/files/root-ca.pem', True), ('PG_VERSION', False)):
            with self.subTest(retained=relative):
                data = self.server_data if is_server else self.database_data
                marker = data / relative
                marker.parent.mkdir(parents=True, exist_ok=True)
                marker.write_text('retained-data')
                with self.assertRaises(b.BootstrapError):
                    self.initialize()
                self.assertFalse((self.server / 'fixture.env').exists())
                marker.unlink()

    def test_nonregular_persistent_state_is_rejected_without_waiting(self):
        self.server.mkdir()
        os.mkfifo(self.server / 'fixture.env', 0o600)
        with self.assertRaises(b.BootstrapError):
            self.initialize()

    def test_empty_existing_data_directories_allow_first_start(self):
        self.server_data.mkdir()
        self.database_data.mkdir()
        self.initialize()
        self.assertTrue((self.server / 'fixture.env').is_file())

    def prepare_certificates(self):
        self.initialize()
        files = self.server_data / 'certs/files'
        files.mkdir(parents=True)
        for name, data in {
                'root-ca.pem': b'public CA',
                'gateway-admin.pem': b'public client certificate',
                'gateway-admin.key': b'encrypted client key',
                'root-ca.key': b'CA signing key never exported',
                'takserver.key': b'server key never exported',
        }.items():
            (files / name).write_bytes(data)
        return files

    def test_export_shares_only_the_client_identity_with_gateway_ownership(self):
        files = self.prepare_certificates()
        output = self.root / 'client'
        uid, gid = max(1, os.getuid()), os.getgid()
        b.export_credentials(self.server_data, self.server, output, uid, gid)
        self.assertEqual({path.name for path in output.iterdir()},
                         {'ca.pem', 'client.pem', 'client.key', 'key-password'})
        for source, target in (
                ('root-ca.pem', 'ca.pem'), ('gateway-admin.pem', 'client.pem'),
                ('gateway-admin.key', 'client.key')):
            self.assertEqual((output / target).read_bytes(),
                             (files / source).read_bytes())
        self.assertEqual((output / 'key-password').read_text(),
                         settings(self.server / 'fixture.env')['ADMIN_CERT_PASS']
                         + '\n')
        for path in (output, *output.iterdir()):
            self.assertEqual(path.stat().st_uid, uid)
            self.assertEqual(path.stat().st_gid, gid)
            self.assertEqual(path.stat().st_mode & 0o777,
                             0o700 if path == output else 0o600)

    def test_root_or_invalid_gateway_identity_is_rejected_before_export(self):
        self.prepare_certificates()
        for uid, gid in ((0, 0), (-1, 1000), (1000, -1)):
            with self.subTest(uid=uid, gid=gid):
                output = self.root / 'invalid-client'
                with self.assertRaises(b.BootstrapError):
                    b.export_credentials(self.server_data, self.server, output,
                                         uid, gid)
                self.assertFalse(output.exists())

    def test_export_fails_if_any_client_credential_is_missing(self):
        files = self.prepare_certificates()
        (files / 'gateway-admin.key').unlink()
        with self.assertRaises((b.BootstrapError, OSError)):
            b.export_credentials(self.server_data, self.server,
                                 self.root / 'client', max(1, os.getuid()),
                                 os.getgid())


class Readiness(unittest.TestCase):
    def test_correct_authenticated_version_completes_readiness(self):
        context = object()
        with mock.patch.object(b, '_client_context', return_value=context), \
                mock.patch.object(b, '_request_version',
                                  return_value=(200, VERSION)) as request:
            b.wait_for_server('takserver', 8443, Path('/credentials'))
        self.assertEqual(request.call_count, 1)
        self.assertEqual(request.call_args.args[:3],
                         (context, 'takserver', 8443))

    def test_transient_connection_and_server_errors_retry(self):
        for initial in (OSError('not ready'), (503, b'starting')):
            with self.subTest(initial=type(initial).__name__), \
                    mock.patch.object(b, '_client_context'), \
                    mock.patch.object(b, '_request_version',
                                      side_effect=[initial, (200, VERSION)]) \
                    as request, mock.patch.object(b.time, 'sleep'):
                b.wait_for_server('takserver', 8443, Path('/credentials'))
                self.assertEqual(request.call_count, 2)

    def test_authorization_failures_are_not_retried(self):
        for status in (401, 403):
            with self.subTest(status=status), \
                    mock.patch.object(b, '_client_context'), \
                    mock.patch.object(b, '_request_version',
                                      return_value=(status, b'private body')) \
                    as request, mock.patch.object(b.time, 'sleep') as sleep:
                with self.assertRaises(b.BootstrapError):
                    b.wait_for_server('takserver', 8443, Path('/credentials'))
                self.assertEqual(request.call_count, 1)
                sleep.assert_not_called()

    def test_tls_identity_failures_are_not_retried(self):
        with mock.patch.object(b, '_client_context'), \
                mock.patch.object(b, '_request_version', side_effect=
                                  ssl.SSLCertVerificationError('private host')) \
                as request, mock.patch.object(b.time, 'sleep') as sleep:
            with self.assertRaises(b.BootstrapError) as failure:
                b.wait_for_server('takserver', 8443, Path('/credentials'))
        self.assertNotIn('private host', str(failure.exception))
        self.assertEqual(request.call_count, 1)
        sleep.assert_not_called()

    def test_https_readiness_connects_directly_and_bounds_its_response(self):
        connection = mock.Mock()
        response = connection.getresponse.return_value
        response.status, response.length = 200, len(VERSION)
        response.read.return_value = VERSION
        context = object()
        with mock.patch.dict(os.environ, {'HTTPS_PROXY': 'http://unused:8080'}), \
                mock.patch.object(b.http.client, 'HTTPSConnection',
                                  return_value=connection) as create:
            self.assertEqual(b._request_version(context, 'takserver', 8443, 5),
                             (200, VERSION))
        create.assert_called_once_with('takserver', 8443, timeout=5,
                                       context=context)
        connection.request.assert_called_once_with('GET',
                                                    '/Marti/api/version/info')
        response.read.assert_called_once_with(65537)
        connection.close.assert_called_once_with()

    def test_oversized_readiness_responses_fail_and_close_the_connection(self):
        for length, body in ((65537, b''), (None, b'x' * 65537)):
            with self.subTest(length=length):
                connection = mock.Mock()
                response = connection.getresponse.return_value
                response.status, response.length = 200, length
                response.read.return_value = body
                with mock.patch.object(b.http.client, 'HTTPSConnection',
                                       return_value=connection):
                    with self.assertRaises(b.BootstrapError):
                        b._request_version(object(), 'takserver', 8443, 5)
                connection.close.assert_called_once_with()

    def test_invalid_or_unexpected_server_versions_fail(self):
        for body in (b'not JSON', b'{}', b'[]',
                     b'{"major":5,"minor":8,"patch":14}'):
            with self.subTest(body=body), \
                    mock.patch.object(b, '_client_context'), \
                    mock.patch.object(b, '_request_version',
                                      return_value=(200, body)):
                with self.assertRaises(b.BootstrapError):
                    b.wait_for_server('takserver', 8443, Path('/credentials'))

    def test_connection_attempts_and_retries_obey_the_deadline(self):
        clock = [0.0]

        def failed_request(*args, **kwargs):
            clock[0] += 0.01
            raise OSError('unreachable private address')

        def sleep(delay):
            clock[0] += delay

        with mock.patch.object(b, '_client_context'), \
                mock.patch.object(b, '_request_version',
                                  side_effect=failed_request) as request, \
                mock.patch.object(b.time, 'monotonic',
                                  side_effect=lambda: clock[0]), \
                mock.patch.object(b.time, 'sleep', side_effect=sleep):
            with self.assertRaises(b.BootstrapError):
                b.wait_for_server('takserver', 8443, Path('/credentials'),
                                  timeout=1.0)
        self.assertLessEqual(clock[0], 1.01)
        for call in request.call_args_list:
            timeout = call.kwargs.get('timeout', call.args[-1])
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 1.0)

    def test_cli_sanitizes_io_failures(self):
        errors = io.StringIO()
        with mock.patch.object(b, 'export_credentials',
                               side_effect=OSError('secret-value')), \
                contextlib.redirect_stderr(errors):
            with self.assertRaises(SystemExit) as failure:
                b.main(['credentials', '--server-data', '/server-data',
                        '--server-secrets', '/server-secrets',
                        '--output', '/client', '--uid', '1000',
                        '--gid', '1000'])
        self.assertEqual(failure.exception.code, 1)
        self.assertTrue(errors.getvalue())
        self.assertNotIn('secret-value', errors.getvalue())


if __name__ == '__main__':
    unittest.main()
