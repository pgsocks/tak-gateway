"""Offline contracts for the real-server interoperability harness."""
import json
import pathlib
import socket
import struct
import tempfile
import unittest
from unittest import mock

import harness as h


OFFER = (b'<event type="t-x-takp-v"><detail><TakControl>'
         b'<TakProtocolSupport version="1"/></TakControl></detail></event>')
MISSION = (b'<event type="t-x-m-n" uid="server-random"><detail>'
           b'<mission name="fixture-1" type="CREATE"/></detail></event>')


class XmlTests(unittest.TestCase):
    def test_every_split_and_coalesced_documents(self):
        xml = b'<?xml version="1.0"?><event><detail><![CDATA[</event>]]>' \
              b'</detail><!-- </event> --></event>'
        for split in range(len(xml) + 1):
            stream = h.XmlEvents()
            events = stream.feed(xml[:split]) + stream.feed(xml[split:])
            self.assertEqual(events, [xml])
        self.assertEqual(h.XmlEvents().feed(OFFER + MISSION), [OFFER, MISSION])

    def test_whitespace_and_utf8(self):
        xml = '<event><detail>é</detail></event>'.encode()
        stream = h.XmlEvents()
        events = []
        for byte in b' \n' + xml:
            events.extend(stream.feed(bytes([byte])))
        self.assertEqual(events, [xml])

    def test_reject_non_event_dtd_entity_malformed_depth_and_size(self):
        for xml in (b'<foo/>', b'<!DOCTYPE event><event/>',
                    b'<!DOCTYPE event [<!ENTITY a "x">]><event>&a;</event>',
                    b'<event><x></event>'):
            with self.subTest(xml=xml), self.assertRaises(h.HarnessError):
                h.XmlEvents().feed(xml)
        with self.assertRaises(h.HarnessError):
            h.XmlEvents(max_bytes=7).feed(b'<event/>')
        with self.assertRaises(h.HarnessError):
            h.XmlEvents(max_depth=1).feed(b'<event><x/></event>')

    def test_offer_requires_exact_type_and_structure(self):
        self.assertTrue(h.is_offer(OFFER))
        self.assertFalse(h.is_offer(MISSION))
        self.assertFalse(h.is_offer(b'<event type="t-x-takp-v"/>'))

    def test_match_uses_mission_name_and_create_type_not_uid(self):
        self.assertTrue(h.is_mission_create(MISSION, 'fixture-1'))
        self.assertFalse(h.is_mission_create(MISSION, 'other'))
        self.assertFalse(h.is_mission_create(
            MISSION.replace(b'CREATE', b'DELETE'), 'fixture-1'))
        self.assertFalse(h.is_mission_create(
            MISSION.replace(b't-x-m-n', b't-x-m-d'), 'fixture-1'))


class FakeSocket:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.timeouts = []
    def recv(self, size):
        if not self.chunks:
            return b''
        data = self.chunks.pop(0)
        if len(data) > size:
            self.chunks.insert(0, data[size:])
        return data[:size]
    def settimeout(self, value):
        self.timeouts.append(value)


class CaptureTests(unittest.TestCase):
    def test_u32be_partial_reads_and_multiple_records(self):
        wire = struct.pack('!I', len(MISSION)) + MISSION
        sock = FakeSocket([bytes([byte]) for byte in wire + wire])
        self.assertEqual(h.read_record(sock, h.deadline(1)), MISSION)
        self.assertEqual(h.read_record(sock, h.deadline(1)), MISSION)
        self.assertTrue(all(x > 0 for x in sock.timeouts))

    def test_zero_oversize_truncated_and_invalid_xml(self):
        for wire in (b'\0\0\0\0', struct.pack('!I', 99999999),
                     b'\0\0', b'\0\0\0\x08<event',
                     b'\0\0\0\x03bad'):
            with self.subTest(wire=wire), self.assertRaises(h.HarnessError):
                h.read_record(FakeSocket([wire]), h.deadline(1))

    def test_deadline_and_socket_timeout_are_reported(self):
        with self.assertRaises(h.HarnessError):
            h.read_exact(FakeSocket([b'x']), 1, h.deadline(-1))
        sock = mock.Mock()
        sock.recv.side_effect = socket.timeout()
        with self.assertRaises(h.HarnessError):
            h.read_exact(sock, 1, h.deadline(1))

    def test_xml_wait_retains_parser_and_ignores_other_events(self):
        subscriber = h.XmlSubscriber(FakeSocket([OFFER + MISSION]))
        self.assertEqual(subscriber.wait(h.is_offer, h.deadline(1)), OFFER)
        self.assertEqual(subscriber.wait(
            lambda xml: h.is_mission_create(xml, 'fixture-1'),
            h.deadline(1)), MISSION)


class RestTests(unittest.TestCase):
    def test_create_no_json_body_url_escaped_and_cleanup(self):
        rest = mock.Mock()
        with h.mission(rest, 'name /?', 'creator'):
            pass
        create, delete = rest.request.call_args_list
        self.assertEqual(create.args[0], 'PUT')
        self.assertIn('name%20%2F%3F', create.args[1])
        self.assertIn('group=__ANON__', create.args[1])
        self.assertEqual(delete.args[0], 'DELETE')
        self.assertEqual(len(create.args), 2)

    def test_cleanup_after_uncertain_create_and_capture_failure(self):
        for failing in ('create', 'capture'):
            rest = mock.Mock()
            if failing == 'create':
                rest.request.side_effect = [h.HarnessError('uncertain'), None]
            with self.subTest(failing=failing), self.assertRaisesRegex(
                    h.HarnessError, 'uncertain|capture'):
                with h.mission(rest, 'unique', 'creator'):
                    raise h.HarnessError('capture')
            self.assertEqual(rest.request.call_args_list[-1].args[0], 'DELETE')

    def test_cleanup_failure_is_not_success_or_primary_error_loss(self):
        rest = mock.Mock()
        rest.request.side_effect = [None, h.HarnessError('delete failed')]
        with self.assertRaisesRegex(h.HarnessError, 'delete failed'):
            with h.mission(rest, 'unique', 'creator'):
                pass
        rest.request.side_effect = [None, h.HarnessError('delete failed')]
        with self.assertRaisesRegex(h.HarnessError, 'capture.*delete failed'):
            with h.mission(rest, 'unique', 'creator'):
                raise h.HarnessError('capture')

    def test_rest_status_redirect_and_response_bounds(self):
        connection = mock.Mock()
        response = connection.getresponse.return_value
        response.status = 200
        response.read.return_value = b'{"data": {}}'
        client = h.RestClient('127.0.0.1', 8443, mock.Mock())
        with mock.patch.object(client, '_connection', return_value=connection):
            self.assertEqual(client.request('GET', '/x'), b'{"data": {}}')
            for status in (301, 400, 401, 403, 500):
                response.status = status
                with self.assertRaisesRegex(h.HarnessError, str(status)):
                    client.request('GET', '/x')
            response.status = 404
            self.assertEqual(client.request('DELETE', '/x'), b'')
            response.status = 200
            response.read.return_value = b'x' * (h.MAX_REST_BYTES + 1)
            with self.assertRaises(h.HarnessError):
                client.request('GET', '/x')
        self.assertEqual(connection.request.call_args.args[:2], ('GET', '/x'))
        self.assertNotIn('Content-Type', connection.request.call_args.kwargs.get(
            'headers', {}))
        self.assertTrue(connection.close.called)


class StateTests(unittest.TestCase):
    def test_state_private_random_and_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / 'state'
            state = h.new_state(root, 'local:test')
            self.assertRegex(state['project'], r'^takgw-[a-f0-9]{12}$')
            self.assertEqual(root.stat().st_mode & 0o777, 0o700)
            self.assertEqual((root / 'fixture.env').stat().st_mode & 0o777, 0o600)
            self.assertEqual(h.load_state(root), state)
            with self.assertRaises(h.HarnessError):
                h.new_state(root, 'local:other')
            values = h.read_settings(root / 'fixture.env')
            self.assertNotEqual(values['CA_PASS'], values['ADMIN_CERT_PASS'])
            self.assertGreater(len(values['POSTGRES_PASSWORD']), 32)

    def test_refuse_unowned_project(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'state.json').write_text(json.dumps({'project': 'production'}))
            with self.assertRaises(h.HarnessError):
                h.load_state(path)

    def test_compose_explicit_project_and_local_daemon(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'state'
            state = h.new_state(path, 'local:test')
            with mock.patch.object(h, 'command', return_value='') as command:
                h.compose(path, state, ['down', '--volumes'])
            args = command.call_args.args[0]
            self.assertIn('--host=unix:///var/run/docker.sock', args)
            self.assertIn(state['project'], args)
            self.assertEqual(args[-2:], ['down', '--volumes'])

    def test_port_mapping_ipv4_ipv6_and_invalid(self):
        self.assertEqual(h.mapped_port('127.0.0.1:34567\n'), 34567)
        self.assertEqual(h.mapped_port('[::1]:34567\n'), 34567)
        with self.assertRaises(h.HarnessError):
            h.mapped_port('')


class GatewayTests(unittest.TestCase):
    def test_config_exact_tls_name_no_secret_literal(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            state = {'tls_port': 12345}
            config = h.gateway_config(path, state, 23456)
            text = config.read_text()
            self.assertIn('server_name = "takserver"', text)
            self.assertIn('protocol = "xml-only"', text)
            self.assertIn('framing = "u32be"', text)
            self.assertIn('key_password_file', text)
            self.assertNotIn('ADMIN_CERT_PASS', text)
            self.assertIn('port = 23456', text)

    def test_gateway_readiness_requires_source_progress_and_output(self):
        state = {'inputs': [{'id': 'fixture', 'state': 'XML_ACTIVE',
                             'protocol_offer_received': True}],
                 'outputs': [{'id': 'capture', 'available': True}]}
        self.assertTrue(h.gateway_ready(state))
        state['inputs'][0]['protocol_offer_received'] = False
        self.assertFalse(h.gateway_ready(state))
        state['inputs'][0]['protocol_offer_received'] = True
        state['outputs'][0]['available'] = False
        self.assertFalse(h.gateway_ready(state))
        self.assertFalse(h.gateway_ready({}))

    def test_protobuf_config_requires_negotiation_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            config = h.gateway_config(pathlib.Path(directory),
                                      {'tls_port': 12345}, 23456, 'protobuf')
            self.assertIn('protocol = "negotiated"', config.read_text())
            self.assertIn('prefer = "protobuf"', config.read_text())
        state = {'inputs': [{'id': 'fixture', 'state': 'XML_ACTIVE',
                             'protocol_offer_received': True}],
                 'outputs': [{'id': 'capture', 'available': True}]}
        self.assertFalse(h.gateway_ready(state, 'protobuf'))
        state['inputs'][0]['state'] = 'PROTOBUF_ACTIVE'
        self.assertTrue(h.gateway_ready(state, 'protobuf'))
        self.assertFalse(h.gateway_ready(state, 'xml'))

    def test_missing_executable_fails_before_start(self):
        with self.assertRaises(h.HarnessError):
            h.validate_gateway('/nonexistent/tak-gateway')


class LifecycleTests(unittest.TestCase):
    def test_disposable_run_always_stops_own_project(self):
        with mock.patch.object(h, 'up', return_value={}) as up, \
                mock.patch.object(h, 'smoke', side_effect=h.HarnessError('capture')), \
                mock.patch.object(h, 'down') as down, \
                mock.patch.object(h.signal, 'signal'), \
                mock.patch('sys.stderr'):
            self.assertEqual(h.main(['run', '--skip-build']), 1)
            self.assertEqual(up.call_args.args[0], down.call_args.args[0])
            self.assertEqual(down.call_count, 1)
            self.assertFalse(up.call_args.args[0].exists())

    def test_run_reports_cleanup_failure(self):
        with mock.patch.object(h, 'up', return_value={}), \
                mock.patch.object(h, 'smoke'), \
                mock.patch.object(h, 'down', side_effect=h.HarnessError('cleanup')), \
                mock.patch.object(h.signal, 'signal'), \
                mock.patch('sys.stderr') as stderr:
            self.assertEqual(h.main(['run', '--skip-build']), 1)
            self.assertIn('cleanup failed', str(stderr.write.call_args_list))

    def test_up_failure_removes_partial_project(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'state'
            with mock.patch.object(h, 'command', return_value=''), \
                    mock.patch.object(h, 'compose', side_effect=h.HarnessError('up')), \
                    mock.patch.object(h, 'down') as down:
                with self.assertRaisesRegex(h.HarnessError, 'up'):
                    h.up(path, 'test:local', skip_build=True)
                down.assert_called_once_with(path)

    def test_signal_becomes_failure_for_finally_cleanup(self):
        with self.assertRaisesRegex(h.HarnessError, 'signal'):
            h.interrupt(15, None)

    def test_stop_gateway_graceful_and_forced_failure(self):
        process = mock.Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        h.stop_gateway(process)
        process.terminate.assert_called_once()
        process.kill.assert_not_called()
        process.wait.side_effect = [h.subprocess.TimeoutExpired('gateway', 10), 0]
        with self.assertRaisesRegex(h.HarnessError, 'forced'):
            h.stop_gateway(process)
        process.kill.assert_called_once()

    def test_gateway_exit_is_detected_and_cleaned(self):
        process = mock.Mock()
        process.poll.return_value = 7
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(h, 'validate_gateway', return_value='/binary'), \
                    mock.patch.object(h.subprocess, 'run', return_value=mock.Mock(returncode=0)), \
                    mock.patch.object(h.subprocess, 'Popen', return_value=process), \
                    mock.patch.object(h.socket, 'socket') as socket_factory, \
                    mock.patch.object(h, 'stop_gateway') as stop:
                socket_factory.return_value.__enter__.return_value.getsockname.return_value = ('127.0.0.1', 12345)
                with self.assertRaisesRegex(h.HarnessError, 'exited'):
                    with h.managed_gateway('/binary', pathlib.Path(directory),
                                           {'tls_port': 1234}):
                        self.fail('unreachable')
                stop.assert_called_once_with(process)

    def test_docker_inherited_remote_selectors_removed(self):
        with mock.patch.dict(h.os.environ, {'DOCKER_HOST': 'remote',
                                          'DOCKER_CONTEXT': 'remote',
                                          'DOCKER_CONFIG': '/preserved'}):
            values = h.docker_environment()
            self.assertNotIn('DOCKER_HOST', values)
            self.assertNotIn('DOCKER_CONTEXT', values)
            self.assertEqual(values['DOCKER_CONFIG'], '/preserved')

    def test_provenance_required_for_cached_image(self):
        provenance = {'revision': h.REVISION, 'tag': '5.7-RELEASE-14',
                      'gradle_version': '8.10.2', 'archive_sha256': 'a' * 64}
        h.verify_provenance(provenance)
        for key in provenance:
            altered = dict(provenance)
            altered.pop(key)
            with self.subTest(key=key), self.assertRaises(h.HarnessError):
                h.verify_provenance(altered)

    def test_rest_readiness_validates_version_and_auth(self):
        rest = mock.Mock()
        rest.request.return_value = b'{"major":5,"minor":7,"patch":14}'
        self.assertEqual(h.wait_rest(rest, h.deadline(1))['patch'], 14)
        for response in (b'{}', b'[]', b'bad',
                         b'{"major":5,"minor":6,"patch":14}'):
            rest.request.return_value = response
            with self.subTest(response=response), self.assertRaises(h.HarnessError):
                h.wait_rest(rest, h.deadline(1))
        rest.request.side_effect = h.HarnessError('HTTP 403')
        with self.assertRaisesRegex(h.HarnessError, '403'):
            h.wait_rest(rest, h.deadline(1))

    def test_rest_readiness_retries_transport_failure_only(self):
        rest = mock.Mock()
        rest.request.side_effect = [h.HarnessError('transport'),
                                   b'{"major":5,"minor":7,"patch":14}']
        with mock.patch.object(h.time, 'sleep'):
            h.wait_rest(rest, h.deadline(1))
        self.assertEqual(rest.request.call_count, 2)

    def test_tls_connect_uses_verified_server_identity(self):
        context = mock.Mock()
        with mock.patch.object(h.socket, 'create_connection') as connect:
            raw = connect.return_value
            h.connect_tls({'tls_port': 12345}, context)
            context.wrap_socket.assert_called_once_with(
                raw, server_hostname='takserver')
            connect.assert_called_once_with(('127.0.0.1', 12345), timeout=10)

    def test_https_wrap_failure_closes_raw_socket(self):
        context = mock.Mock(spec=h.ssl.SSLContext)
        context.wrap_socket.side_effect = h.ssl.SSLError('bad cert')
        connection = h._VerifiedHTTPS('127.0.0.1', 8443, context=context)
        with mock.patch.object(h.socket, 'create_connection') as connect:
            with self.assertRaises(h.ssl.SSLError):
                connection.connect()
            connect.return_value.close.assert_called_once()


class ConfigureTests(unittest.TestCase):
    def test_upstream_namespace_comments_and_private_file_preserved(self):
        import configure
        xml = ('<Configuration xmlns="' + configure.NS + '">'
               '<!-- keep --><repository><connection/></repository>'
               '<security><tls/></security></Configuration>')
        settings = {key: 'private-value' for key in (
            'POSTGRES_URL', 'POSTGRES_USER', 'POSTGRES_PASSWORD',
            'TAKSERVER_CERT_PASS', 'CA_PASS')}
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.xml'
            target = pathlib.Path(directory) / 'config.xml'
            source.write_text(xml)
            configure.configure(source, target, settings)
            self.assertIn('<!-- keep -->', target.read_text())
            self.assertIn('password="private-value"', target.read_text())
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_missing_upstream_element_fails(self):
        import configure
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.xml'
            source.write_text('<Configuration/>')
            with self.assertRaisesRegex(ValueError, 'missing'):
                configure.configure(source, source.with_suffix('.new'), {})

    def test_bootstrap_sets_retention_before_sourcing_strict_upstream(self):
        text = (h.ROOT / 'fixture-entrypoint.sh').read_text()
        self.assertLess(text.index('export RETENTION_MAX_HEAP='),
                        text.index('. ./setenv.sh'))

class CompleteLifecycleTests(unittest.TestCase):
    def test_up_exports_private_credentials_and_records_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'state'
            def fake_compose(root, state, args, **kwargs):
                if args[0] == 'cp':
                    target = pathlib.Path(args[2])
                    if 'provenance' in args[1]:
                        target.write_text(json.dumps({
                            'revision': h.REVISION, 'tag': '5.7-RELEASE-14',
                            'gradle_version': '8.10.2',
                            'archive_sha256': 'a' * 64}))
                    else:
                        target.write_text('fixture test certificate')
                if args[0] == 'ps':
                    return 'container-id'
                if args[0] == 'port':
                    return '127.0.0.1:23456'
                return ''
            with mock.patch.object(h, 'compose', side_effect=fake_compose), \
                    mock.patch.object(h, 'command', return_value='sha256:123'), \
                    mock.patch.object(h, 'clients', return_value=(mock.Mock(), None)), \
                    mock.patch.object(h, 'wait_rest'):
                state = h.up(path, 'local:test', skip_build=True)
            self.assertEqual(state['image_id'], 'sha256:123')
            self.assertEqual(state['tls_port'], 23456)
            self.assertEqual(h.load_state(path), state)
            for file in (path / 'client').iterdir():
                self.assertEqual(file.stat().st_mode & 0o777, 0o600)

    def test_managed_gateway_readiness_connects_before_yield(self):
        ready = {'inputs': [{'id': 'fixture', 'state': 'XML_ACTIVE',
                             'protocol_offer_received': True}],
                 'outputs': [{'id': 'capture', 'available': True}]}
        process = mock.Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            with mock.patch.object(h, 'validate_gateway', return_value='/gateway'), \
                    mock.patch.object(h.subprocess, 'run', return_value=mock.Mock(returncode=0)) as configure, \
                    mock.patch.object(h.subprocess, 'Popen', return_value=process) as launch, \
                    mock.patch.object(h.socket, 'socket') as socket_factory, \
                    mock.patch.object(h.socket, 'create_connection') as connect, \
                    mock.patch.object(h.Path, 'read_text', return_value=json.dumps(ready)):
                reservation = socket_factory.return_value.__enter__.return_value
                reservation.getsockname.return_value = ('127.0.0.1', 23456)
                with h.managed_gateway('/gateway', path, {'tls_port': 12345}):
                    connect.assert_called_once_with(('127.0.0.1', 23456), timeout=1)
                    configure.assert_called_once()
                    configure_args = configure.call_args.args[0]
                    self.assertEqual(configure_args[:3],
                                     ['/gateway', 'config', '--state-dir'])
                    storage = pathlib.Path(configure_args[3])
                    self.assertEqual(configure_args[4:],
                                     ['--config', str(path / 'gateway.toml')])
                    self.assertEqual(launch.call_args.args[0],
                                     ['/gateway', 'run', '--state-dir', str(storage)])
                    self.assertTrue(storage.is_dir())
                    self.assertEqual(storage.stat().st_mode & 0o777, 0o700)
                process.terminate.assert_called_once()
                self.assertFalse(storage.exists())

    def test_managed_gateway_cleanup_preserves_capture_error(self):
        process = mock.Mock()
        process.poll.return_value = 7
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(h, 'validate_gateway', return_value='/gateway'), \
                    mock.patch.object(h.subprocess, 'run', return_value=mock.Mock(returncode=0)), \
                    mock.patch.object(h.subprocess, 'Popen', return_value=process), \
                    mock.patch.object(h.socket, 'socket') as socket_factory, \
                    mock.patch.object(h, 'stop_gateway',
                                      side_effect=h.HarnessError('cleanup')):
                reservation = socket_factory.return_value.__enter__.return_value
                reservation.getsockname.return_value = ('127.0.0.1', 23456)
                with self.assertRaisesRegex(h.HarnessError, 'exited.*cleanup'):
                    with h.managed_gateway('/gateway', pathlib.Path(directory),
                                           {'tls_port': 12345}):
                        pass

    def test_managed_gateway_does_not_run_after_configuration_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            with mock.patch.object(h, 'validate_gateway', return_value='/gateway'), \
                    mock.patch.object(h.subprocess, 'run',
                                      return_value=mock.Mock(returncode=9)) as configure, \
                    mock.patch.object(h.subprocess, 'Popen') as launch, \
                    mock.patch.object(h.socket, 'socket') as socket_factory:
                reservation = socket_factory.return_value.__enter__.return_value
                reservation.getsockname.return_value = ('127.0.0.1', 23456)
                with self.assertRaisesRegex(h.HarnessError, 'configuration.*9'):
                    with h.managed_gateway('/gateway', path, {'tls_port': 12345}):
                        self.fail('unreachable')
                launch.assert_not_called()
                storage = pathlib.Path(configure.call_args.args[0][3])
                self.assertFalse(storage.exists())

    def test_managed_gateway_configuration_timeout_removes_its_store(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            with mock.patch.object(h, 'validate_gateway', return_value='/gateway'), \
                    mock.patch.object(h.subprocess, 'run',
                                      side_effect=h.subprocess.TimeoutExpired(
                                          'configuration', 30)) as configure, \
                    mock.patch.object(h.subprocess, 'Popen') as launch, \
                    mock.patch.object(h.socket, 'socket') as socket_factory:
                reservation = socket_factory.return_value.__enter__.return_value
                reservation.getsockname.return_value = ('127.0.0.1', 23456)
                with self.assertRaisesRegex(h.HarnessError, 'configuration.*timed out'):
                    with h.managed_gateway('/gateway', path, {'tls_port': 12345}):
                        self.fail('unreachable')
                launch.assert_not_called()
                storage = pathlib.Path(configure.call_args.args[0][3])
                self.assertFalse(storage.exists())


if __name__ == '__main__':
    unittest.main()
