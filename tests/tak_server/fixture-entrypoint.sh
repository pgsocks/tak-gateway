#!/usr/bin/env bash
# Local interoperability fixture, using official distribution tools/profiles.
set -euo pipefail
umask 077
TR=/opt/tak
mkdir -p "$TR/data/logs"
rm -f "$TR/data/fixture.ready"
children=()
shutdown() {
	trap - EXIT TERM INT
	for child in "${children[@]}"; do
		kill "$child" 2>/dev/null || true
	done
	for child in "${children[@]}"; do
		wait "$child" 2>/dev/null || true
	done
}
trap shutdown EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
for name in POSTGRES_DB POSTGRES_USER POSTGRES_PASSWORD CA_NAME CA_PASS \
	STATE CITY ORGANIZATION ORGANIZATIONAL_UNIT ADMIN_CERT_NAME \
	ADMIN_CERT_PASS TAKSERVER_CERT_PASS CONFIG_MAX_HEAP MESSAGING_MAX_HEAP \
	API_MAX_HEAP PLUGIN_MANAGER_MAX_HEAP; do
	if [[ -z ${!name:-} ]]; then
		echo "Required fixture variable missing: $name" >&2
		exit 1
	fi
done
if [[ ! -d "$TR/data/certs" ]]; then
	cp -a "$TR/certs" "$TR/data/certs"
fi
if [[ ! -L "$TR/certs" ]]; then
	mv "$TR/certs" "$TR/certs.original"
	ln -s "$TR/data/certs" "$TR/certs"
fi
if [[ ! -L "$TR/logs" ]]; then
	if [[ -e "$TR/logs" ]]; then mv "$TR/logs" "$TR/logs.original"; fi
	ln -s "$TR/data/logs" "$TR/logs"
fi
cd "$TR/certs"
if [[ ! -f files/root-ca.pem ]]; then
	CAPASS="$CA_PASS" bash makeRootCa.sh --ca-name "$CA_NAME" \
		>"$TR/data/logs/fixture-pki.log" 2>&1
fi
if [[ ! -f files/intermediate-signing.jks ]]; then
	# makeCert asks confirmation; yes may receive SIGPIPE after completion.
	set +o pipefail
	yes | CAPASS="$CA_PASS" bash makeCert.sh ca intermediate \
		>>"$TR/data/logs/fixture-pki.log" 2>&1
	result=${PIPESTATUS[1]}
	set -o pipefail
	[[ $result == 0 ]]
fi
if [[ ! -f files/takserver.pem ]]; then
	CAPASS="$CA_PASS" PASS="$TAKSERVER_CERT_PASS" \
		bash makeCert.sh server takserver \
		>>"$TR/data/logs/fixture-pki.log" 2>&1
fi
if [[ ! -f "files/$ADMIN_CERT_NAME.pem" ]]; then
	CAPASS="$CA_PASS" PASS="$ADMIN_CERT_PASS" \
		bash makeCert.sh client "$ADMIN_CERT_NAME" \
		>>"$TR/data/logs/fixture-pki.log" 2>&1
fi
python3 "$TR/fixture-configure.py" "$TR/CoreConfig.example.xml" \
	"$TR/data/CoreConfig.xml"
if [[ ! -f "$TR/data/TAKIgniteConfig.xml" ]]; then
	cp "$TR/TAKIgniteConfig.example.xml" "$TR/data/TAKIgniteConfig.xml"
fi
ln -sfn "$TR/data/CoreConfig.xml" "$TR/CoreConfig.xml"
ln -sfn "$TR/data/TAKIgniteConfig.xml" "$TR/TAKIgniteConfig.xml"
chmod -R u=rwX,go= "$TR/data"
java -jar "$TR/db-utils/SchemaManager.jar" \
	-url "jdbc:postgresql://takdb:5432/$POSTGRES_DB" \
	-user "$POSTGRES_USER" -password "$POSTGRES_PASSWORD" upgrade \
	>"$TR/data/logs/fixture-schema.log" 2>&1
cd "$TR"
# Upstream sets the Java opens required by Ignite and TAK Server.
export RETENTION_MAX_HEAP=512
. ./setenv.sh
java -jar -Xmx"${CONFIG_MAX_HEAP}m" -Dspring.profiles.active=config \
	takserver.war >data/logs/fixture-config.log 2>&1 &
children+=("$!")
java -jar -Xmx"${MESSAGING_MAX_HEAP}m" -Dspring.profiles.active=messaging \
	takserver.war >data/logs/fixture-messaging.log 2>&1 &
children+=("$!")
java -jar -Xmx"${API_MAX_HEAP}m" -Dspring.profiles.active=api \
	-Dkeystore.pkcs12.legacy takserver.war >data/logs/fixture-api.log 2>&1 &
children+=("$!")
java -jar -Xmx"${PLUGIN_MANAGER_MAX_HEAP}m" \
	-Dloader.path=WEB-INF/lib-provided,WEB-INF/lib,WEB-INF/classes,file:lib/ \
	takserver-pm.jar >data/logs/fixture-plugins.log 2>&1 &
children+=("$!")
export TAKCL_CORECONFIG_PATH="$TR/CoreConfig.xml"
export TAKCL_TAKIGNITECONFIG_PATH="$TR/TAKIgniteConfig.xml"
for attempt in $(seq 1 90); do
	for child in "${children[@]}"; do kill -0 "$child"; done
	if java -jar utils/UserManager.jar certmod -A \
		"$TR/certs/files/$ADMIN_CERT_NAME.pem" \
		>data/logs/fixture-admin.log 2>&1; then
		touch data/fixture.ready
		echo 'Fixture initialized; authenticated REST readiness checked by runner'
		wait -n "${children[@]}"
		exit 1
	fi
	sleep 3
done
echo 'Fixture admin initialization timed out' >&2
exit 1
