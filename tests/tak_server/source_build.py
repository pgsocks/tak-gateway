#!/usr/bin/env python3
"""Build the pinned official backend distribution inside Dockerfile.source.

This helper runs only in the builder stage. Proxy settings and trust material
are scoped to this invocation; the runtime receives the distribution and its
non-secret provenance, never the checkout, Gradle home, or build credentials.
"""

import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET
import zipfile


REVISION = "5187abd46d827d37cfc5708805eced197a837e49"
TAG = "5.7-RELEASE-14"
REPOSITORY = "https://github.com/TAK-Product-Center/Server.git"
JAVA_IMAGE = (
    "eclipse-temurin@sha256:"
    "73ed442231ec66cfe48cae0b263ce619a75ab96d162ab325f38fc0a269ee3aab"
)
GRADLE_VERSION = "8.10.2"
GRADLE_SHA256 = (
    "31c55713e40233a8303827ceb42ca48a47267a0ad4bab9177123121e71524c26"
)
EXCLUDED_TASKS = (
    ":federation-hub-ui:bootWar",
    ":federation-hub-ui:bootJar",
    ":takserver-core:deployMetricsDashboard",
    ":takserver-tool-ui:deployToCore",
)
CERTIFICATE = re.compile(
    r"-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----",
    re.DOTALL,
)


def execute(arguments, *, cwd=None, env=None, capture=False):
    """Run without a shell or echoing arguments that could hold secrets."""
    return subprocess.run(
        arguments, cwd=cwd, env=env, check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def certificates(text):
    """Return PEM blocks indexed by their DER hash; reject empty bundles."""
    result = {}
    for match in CERTIFICATE.finditer(text):
        encoded = re.sub(r"\s", "", match.group(1))
        der = base64.b64decode(encoded, validate=True)
        if not der:
            raise ValueError("Empty certificate in trust bundle")
        result[hashlib.sha256(der).hexdigest()] = match.group(0) + "\n"
    if not result:
        raise ValueError("Trust bundle contains no certificates")
    return result


def java_property(value):
    """Escape a value for a Java .properties file, including Unicode."""
    value = str(value)
    result = []
    for character in value:
        if character in "\\:=#! ":
            result.append("\\" + character)
        elif character in "\r\n\t\f":
            result.append({"\r": "\\r", "\n": "\\n", "\t": "\\t",
                           "\f": "\\f"}[character])
        elif not 0x20 <= ord(character) <= 0x7e:
            # Java properties Unicode escapes address UTF-16 code units.
            units = character.encode("utf-16-be")
            result.extend("\\u%04x" % int.from_bytes(units[i:i + 2], "big")
                          for i in range(0, len(units), 2))
        else:
            result.append(character)
    return "".join(result)


def proxy_properties(environment):
    """Translate inherited HTTP proxies for Java without logging secrets."""
    result = {}
    for scheme in ("http", "https"):
        value = (environment.get(scheme.upper() + "_PROXY") or
                 environment.get(scheme + "_proxy"))
        if not value:
            continue
        proxy = urlsplit(value)
        if proxy.scheme != "http" or not proxy.hostname:
            raise ValueError("Java builder requires an HTTP forward proxy")
        result[f"systemProp.{scheme}.proxyHost"] = proxy.hostname
        result[f"systemProp.{scheme}.proxyPort"] = proxy.port or 80
        if proxy.username is not None or proxy.password is not None:
            # Gradle supports proxy credentials in its private properties file.
            from urllib.parse import unquote
            result[f"systemProp.{scheme}.proxyUser"] = unquote(
                proxy.username or "")
            result[f"systemProp.{scheme}.proxyPassword"] = unquote(
                proxy.password or "")
    bypass = (environment.get("NO_PROXY") or environment.get("no_proxy") or "")
    hosts = ["localhost", "127.*", "[::1]"]
    for host in bypass.split(","):
        host = host.strip()
        if not host or "/" in host or ":" in host:
            continue
        hosts.append("*" + host if host.startswith(".") else host)
    result["systemProp.http.nonProxyHosts"] = "|".join(dict.fromkeys(hosts))
    result["systemProp.https.nonProxyHosts"] = result[
        "systemProp.http.nonProxyHosts"]
    return result


def prepare_java_trust(directory, bundle, environment):
    """Extend a temporary copy of the JDK roots with the mounted CA bundle."""
    java_home = Path(environment["JAVA_HOME"])
    keytool = str(java_home / "bin/keytool")
    store = directory / "cacerts"
    shutil.copyfile(java_home / "lib/security/cacerts", store)
    current = execute(
        [keytool, "-list", "-rfc", "-keystore", str(store),
         "-storepass", "changeit"], capture=True,
    ).stdout
    existing = certificates(current)
    for fingerprint, pem in certificates(bundle.read_text()).items():
        if fingerprint in existing:
            continue
        certificate = directory / "additional.pem"
        certificate.write_text(pem)
        execute(
            [keytool, "-importcert", "-noprompt", "-trustcacerts",
             "-alias", "fixture-" + fingerprint, "-file", str(certificate),
             "-keystore", str(store), "-storepass", "changeit"],
            capture=True,
        )
        certificate.unlink()
    return store


def extract_distribution(archive, destination):
    """Extract only regular members below the official ZIP's single root."""
    with zipfile.ZipFile(archive) as contents:
        members = contents.infolist()
        roots = {PurePosixPath(member.filename).parts[0] for member in members
                 if PurePosixPath(member.filename).parts}
        if len(roots) != 1:
            raise ValueError("Distribution must have one top-level directory")
        root = next(iter(roots))
        validated = []
        for member in members:
            path = PurePosixPath(member.filename)
            mode = (member.external_attr >> 16) & 0xffff
            if (not path.parts or path.is_absolute() or ".." in path.parts or
                    "\\" in member.filename or "\x00" in member.filename or
                    path.parts[0] != root):
                raise ValueError("Unsafe distribution member")
            if (mode & 0o170000) not in (0, 0o040000, 0o100000):
                raise ValueError("Non-regular distribution member")
            if len(path.parts) > 1:
                validated.append((member, Path(*path.parts[1:])))
        if not any(str(path) == "tak/takserver.war" for _, path in validated):
            raise ValueError("Distribution lacks tak/takserver.war")
        for member, path in validated:
            target = destination / path
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with contents.open(member) as source, target.open("wb") as out:
                    shutil.copyfileobj(source, out)
                target.chmod((member.external_attr >> 16) & 0o777 or 0o644)
    if (destination / "tak/version.txt").read_text().strip() != TAG:
        raise ValueError("Built distribution has an unexpected release")


def test_summary(source):
    """Require actual successful core unit results, including skipped counts."""
    reports = sorted(source.glob("src/**/build/test-results/test/TEST-*.xml"))
    total = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0}
    modules = set()
    core_reports = 0
    core_executed = 0
    for report in reports:
        suite = ET.parse(report).getroot()
        for key in total:
            total[key] += int(suite.attrib.get(key, 0))
        modules.add(str(report.relative_to(source)).split("/build/")[0])
        if report.is_relative_to(source / "src/takserver-core/build"):
            core_reports += 1
            core_executed += (int(suite.attrib.get("tests", 0)) -
                              int(suite.attrib.get("skipped", 0)))
    if not core_reports or not core_executed:
        raise ValueError("The official build produced no core unit results")
    if total["failures"] or total["errors"]:
        raise ValueError("The official build reports failed unit tests")
    return {**total, "report_files": len(reports), "modules": sorted(modules),
            "core_tests_executed": core_executed,
            "integration_tests_run": False}


def build(workspace, output):
    """Fetch a clean tagged checkout, build/test, and stage its distribution."""
    workspace.mkdir(parents=True, exist_ok=False)
    output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ)
    bundle = Path("/run/secrets/proxy_ca")
    if not bundle.is_file():
        bundle = Path("/etc/ssl/certs/ca-certificates.crt")
    environment["GIT_SSL_CAINFO"] = str(bundle)
    environment["CURL_CA_BUNDLE"] = str(bundle)
    source = workspace / "Server"
    execute(["git", "init", "--quiet", str(source)], env=environment)
    execute(["git", "remote", "add", "origin", REPOSITORY], cwd=source,
            env=environment)
    execute(["git", "fetch", "--depth=1", "origin", "refs/tags/" + TAG +
             ":refs/tags/" + TAG], cwd=source, env=environment)
    execute(["git", "-c", "filter.lfs.smudge=", "-c", "filter.lfs.process=",
             "-c", "filter.lfs.required=false", "checkout", "--detach", TAG],
            cwd=source, env=environment)
    actual = execute(["git", "rev-parse", "HEAD"], cwd=source,
                     capture=True).stdout.strip()
    if actual != REVISION:
        raise ValueError("Fetched release tag does not match the pinned commit")
    actual_tag = execute(["git", "describe", "--tags", "--exact-match", "HEAD"],
                         cwd=source, capture=True).stdout.strip()
    if actual_tag != TAG:
        raise ValueError("Checkout is not at the expected release tag")
    archive = workspace / "gradle.zip"
    execute(["curl", "--fail", "--silent", "--show-error", "--location",
             "--retry", "2", "--output", str(archive),
             "https://services.gradle.org/distributions/gradle-" +
             GRADLE_VERSION + "-bin.zip"], env=environment)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != GRADLE_SHA256:
        raise ValueError("Gradle download does not match its pinned checksum")
    with zipfile.ZipFile(archive) as contents:
        contents.extractall(workspace)
    gradle = workspace / ("gradle-" + GRADLE_VERSION) / "bin/gradle"
    gradle.chmod(0o755)
    with tempfile.TemporaryDirectory(prefix="tak-source-trust-") as private:
        private = Path(private)
        trust = prepare_java_trust(private, bundle, environment)
        gradle_home = private / "gradle-home"
        gradle_home.mkdir(mode=0o700)
        properties = proxy_properties(environment)
        properties.update({
            "systemProp.javax.net.ssl.trustStore": trust,
            "systemProp.javax.net.ssl.trustStorePassword": "changeit",
            "org.gradle.daemon": "false",
        })
        configuration = gradle_home / "gradle.properties"
        configuration.write_text("".join(
            key + "=" + java_property(value) + "\n"
            for key, value in properties.items()))
        configuration.chmod(0o600)
        environment["GRADLE_USER_HOME"] = str(gradle_home)
        arguments = [str(gradle), "--no-daemon", "--max-workers=2",
                     "--no-build-cache", "--console=plain",
                     ":takserver-package:buildFullDocker"]
        for task in EXCLUDED_TASKS:
            arguments.extend(["-x", task])
        execute(arguments, cwd=source / "src", env=environment)
    tests = test_summary(source)
    archives = list((source / "src/takserver-package/build/distributions").glob(
        "takserver-docker-full-*.zip"))
    if len(archives) != 1:
        raise ValueError("Expected exactly one official full-Docker archive")
    distribution = archives[0]
    extract_distribution(distribution, output)
    provenance = {
        "repository": REPOSITORY, "revision": REVISION, "tag": TAG,
        "builder_image": JAVA_IMAGE, "runtime_image": JAVA_IMAGE,
        "gradle_version": GRADLE_VERSION, "gradle_sha256": GRADLE_SHA256,
        "task": ":takserver-package:buildFullDocker",
        "excluded_ui_tasks": list(EXCLUDED_TASKS), "upstream_unit_tests": tests,
        "archive": distribution.name,
        "archive_sha256": hashlib.sha256(distribution.read_bytes()).hexdigest(),
        "byte_identical_reproducibility_claimed": False,
    }
    (output / "provenance.json").write_text(
        json.dumps(provenance, indent=2) + "\n")
    print("Pinned official TAK Server backend distribution built and tested.")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: source_build.py NEW_WORKSPACE NEW_OUTPUT")
    try:
        build(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        # Command arguments and captured output may contain proxy credentials.
        raise SystemExit("TAK source build failed: " + type(error).__name__)
