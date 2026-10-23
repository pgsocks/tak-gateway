#!/usr/bin/env python3
"""Apply only fixture database and PKI settings to upstream example config."""
import os
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

NS = 'http://bbn.com/marti/xml/config'


def configure(source, destination, settings):
    ET.register_namespace('', NS)
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    tree = ET.parse(source, parser=parser)
    values = [
        ('repository/connection', 'url', 'POSTGRES_URL'),
        ('repository/connection', 'username', 'POSTGRES_USER'),
        ('repository/connection', 'password', 'POSTGRES_PASSWORD'),
        ('security/tls', 'keystorePass', 'TAKSERVER_CERT_PASS'),
        ('security/tls', 'truststorePass', 'CA_PASS'),
    ]
    for path, attribute, variable in values:
        element = tree.find('/'.join('{' + NS + '}' + part
                                     for part in path.split('/')))
        if element is None:
            raise ValueError('Required upstream config element missing: ' + path)
        element.set(attribute, settings[variable])
    tree.write(destination, xml_declaration=True, encoding='UTF-8')
    Path(destination).chmod(0o600)


if __name__ == '__main__':
    configure(sys.argv[1], sys.argv[2], os.environ)
