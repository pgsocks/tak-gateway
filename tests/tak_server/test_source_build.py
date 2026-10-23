"""Offline source packaging checks; no Docker or network is required."""

import base64
from pathlib import Path
import tempfile
import unittest
import zipfile

import source_build


class TrustAndProxyTests(unittest.TestCase):
    def test_bundle_deduplicates_same_der_with_different_wrapping(self):
        encoded = base64.b64encode(b"test DER bytes").decode()
        single = ("-----BEGIN CERTIFICATE-----\n" + encoded +
                  "\n-----END CERTIFICATE-----\n")
        wrapped = ("-----BEGIN CERTIFICATE-----\n" + encoded[:8] + "\n" +
                   encoded[8:] + "\n-----END CERTIFICATE-----\n")
        self.assertEqual(len(source_build.certificates(single + wrapped)), 1)

    def test_empty_or_invalid_bundle_is_rejected(self):
        for content in ("", "not a certificate",
                        "-----BEGIN CERTIFICATE-----\n!\n"
                        "-----END CERTIFICATE-----"):
            with self.subTest(content=content), self.assertRaises(ValueError):
                source_build.certificates(content)

    def test_https_proxy_preserves_encoded_credentials_in_private_settings(self):
        result = source_build.proxy_properties({
            "HTTPS_PROXY": "http://u%3Aser:pa%3Dss@proxy.example:8888",
            "NO_PROXY": "localhost,.example.org,192.0.2.0/24",
        })
        self.assertEqual(result["systemProp.https.proxyHost"], "proxy.example")
        self.assertEqual(result["systemProp.https.proxyPort"], 8888)
        self.assertEqual(result["systemProp.https.proxyUser"], "u:ser")
        self.assertEqual(result["systemProp.https.proxyPassword"], "pa=ss")
        self.assertIn("*.example.org", result["systemProp.http.nonProxyHosts"])
        self.assertNotIn("192.0.2", result["systemProp.http.nonProxyHosts"])

    def test_https_and_http_forward_proxy_hosts_are_independent(self):
        result = source_build.proxy_properties({
            "http_proxy": "http://first.example",
            "https_proxy": "http://second.example:8000",
        })
        self.assertEqual(result["systemProp.http.proxyHost"], "first.example")
        self.assertEqual(result["systemProp.http.proxyPort"], 80)
        self.assertEqual(result["systemProp.https.proxyHost"], "second.example")

    def test_unsupported_proxy_protocol_is_rejected(self):
        with self.assertRaises(ValueError):
            source_build.proxy_properties({"HTTPS_PROXY": "socks5://host:123"})

    def test_properties_escape_delimiters_controls_and_unicode(self):
        self.assertEqual(source_build.java_property(" a=b:c\\\n"),
                         "\\ a\\=b\\:c\\\\\\n")
        self.assertEqual(source_build.java_property("\U0001f600"),
                         "\\ud83d\\ude00")


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.private = tempfile.TemporaryDirectory()
        self.addCleanup(self.private.cleanup)
        self.root = Path(self.private.name)
        self.archive = self.root / "distribution.zip"
        self.output = self.root / "output"
        self.output.mkdir()

    def write_archive(self, extras=(), version=source_build.TAG):
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("distribution/tak/takserver.war", "fixture")
            archive.writestr("distribution/tak/version.txt", version + "\n")
            for path, content in extras:
                archive.writestr(path, content)

    def test_valid_release_is_extracted_without_top_level_directory(self):
        self.write_archive()
        source_build.extract_distribution(self.archive, self.output)
        self.assertEqual((self.output / "tak/takserver.war").read_text(),
                         "fixture")

    def test_traversal_is_rejected_before_writing_any_member(self):
        self.write_archive([("distribution/../../escaped", "bad")])
        with self.assertRaises(ValueError):
            source_build.extract_distribution(self.archive, self.output)
        self.assertEqual(list(self.output.iterdir()), [])
        self.assertFalse((self.root / "escaped").exists())

    def test_absolute_or_windows_path_is_rejected(self):
        for path in ("/distribution/absolute", "distribution/..\\escape"):
            with self.subTest(path=path):
                self.write_archive([(path, "bad")])
                with self.assertRaises(ValueError):
                    source_build.extract_distribution(self.archive, self.output)
                self.assertEqual(list(self.output.iterdir()), [])

    def test_symbolic_link_is_rejected_before_extraction(self):
        self.write_archive()
        entry = zipfile.ZipInfo("distribution/tak/link")
        entry.create_system = 3
        entry.external_attr = 0o120777 << 16
        with zipfile.ZipFile(self.archive, "a") as archive:
            archive.writestr(entry, "../../elsewhere")
        with self.assertRaises(ValueError):
            source_build.extract_distribution(self.archive, self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_unexpected_release_is_rejected(self):
        self.write_archive(version="different-release")
        with self.assertRaisesRegex(ValueError, "unexpected release"):
            source_build.extract_distribution(self.archive, self.output)

    def test_missing_server_distribution_is_rejected(self):
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("distribution/not-a-server", "fixture")
        with self.assertRaisesRegex(ValueError, "takserver.war"):
            source_build.extract_distribution(self.archive, self.output)


class UpstreamTestEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.private = tempfile.TemporaryDirectory()
        self.addCleanup(self.private.cleanup)
        self.root = Path(self.private.name)

    def report(self, module="takserver-core", *, tests=3, skipped=1,
               failures=0, errors=0):
        path = self.root / "src" / module / "build/test-results/test/TEST-core.xml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('<testsuite tests="%d" skipped="%d" failures="%d" '
                        'errors="%d"/>' % (tests, skipped, failures, errors))

    def test_records_executed_and_skipped_tests_without_claiming_integration(self):
        self.report()
        result = source_build.test_summary(self.root)
        self.assertEqual(result["tests"], 3)
        self.assertEqual(result["skipped"], 1)
        self.assertEqual(result["core_tests_executed"], 2)
        self.assertFalse(result["integration_tests_run"])

    def test_no_core_results_cannot_claim_a_verified_build(self):
        self.report(module="another-module")
        with self.assertRaisesRegex(ValueError, "no core unit results"):
            source_build.test_summary(self.root)

    def test_all_core_cases_skipped_cannot_claim_a_verified_build(self):
        self.report(tests=3, skipped=3)
        with self.assertRaises(ValueError):
            source_build.test_summary(self.root)

    def test_failed_upstream_case_rejects_build_evidence(self):
        for failures, errors in ((1, 0), (0, 1)):
            with self.subTest(failures=failures, errors=errors):
                self.report(failures=failures, errors=errors)
                with self.assertRaisesRegex(ValueError, "failed unit tests"):
                    source_build.test_summary(self.root)


if __name__ == "__main__":
    unittest.main()
