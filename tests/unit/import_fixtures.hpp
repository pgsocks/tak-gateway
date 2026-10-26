#ifndef TAK_GATEWAY_IMPORT_FIXTURES_HPP
#define TAK_GATEWAY_IMPORT_FIXTURES_HPP

#include "contract_support.hpp"
#include "../../src/import_zip.h"
#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace import_fixtures {

using Bytes = fixtures::Bytes;
using Entries = std::vector<std::pair<std::string, Bytes>>;

inline void little(Bytes &out, uint32_t value, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

inline uint32_t crc32(const Bytes &data)
{
	uint32_t result = UINT32_MAX;

	for (uint8_t byte : data) {
		result ^= byte;
		for (unsigned bit = 0; bit < 8; ++bit)
			result = (result >> 1) ^
				((result & 1) ? UINT32_C(0xedb88320) : 0);
	}
	return ~result;
}

/* Stored ZIP records keep fixture generation independent of the importer. */
inline Bytes zip(const Entries &entries)
{
	Bytes result, central;

	for (const auto &entry : entries) {
		uint32_t offset = static_cast<uint32_t>(result.size());
		uint32_t size = static_cast<uint32_t>(entry.second.size());
		uint32_t crc = crc32(entry.second);

		little(result, 0x04034b50, 4);
		little(result, 20, 2);
		little(result, 0, 4);
		little(result, 0, 4);
		little(result, crc, 4);
		little(result, size, 4);
		little(result, size, 4);
		little(result, entry.first.size(), 2);
		little(result, 0, 2);
		result.insert(result.end(), entry.first.begin(),
			entry.first.end());
		result.insert(result.end(), entry.second.begin(),
			entry.second.end());
		little(central, 0x02014b50, 4);
		little(central, 20, 2);
		little(central, 20, 2);
		little(central, 0, 4);
		little(central, 0, 4);
		little(central, crc, 4);
		little(central, size, 4);
		little(central, size, 4);
		little(central, entry.first.size(), 2);
		little(central, 0, 4);
		little(central, 0, 4);
		little(central, 0, 4);
		little(central, offset, 4);
		central.insert(central.end(), entry.first.begin(),
			entry.first.end());
	}
	uint32_t offset = static_cast<uint32_t>(result.size());

	result.insert(result.end(), central.begin(), central.end());
	little(result, 0x06054b50, 4);
	little(result, 0, 4);
	little(result, entries.size(), 2);
	little(result, entries.size(), 2);
	little(result, central.size(), 4);
	little(result, offset, 4);
	little(result, 0, 2);
	return result;
}

inline std::string entry(const std::string &key, const std::string &value)
{
	return "<entry key='" + key + "' class='class java.lang.String'>" +
		value + "</entry>";
}

inline std::string preferences(bool direct = true,
	const std::string &password = "test-only-secret")
{
	std::string result = "<preferences><preference version='1' "
		"name='cot_streams'>" + entry("count", "1") +
		entry("connectString0", "tak.example.invalid:28089:ssl") +
		entry("description0", "Example &amp; test server") +
		entry("enabled0", "true") +
		entry("caLocation0", "cert/trust.p12") +
		entry("caPassword0", password);

	if (direct)
		result += entry("certificateLocation0", "cert/client.p12") +
			entry("clientPassword0", password);
	else
		result += entry("useAuth0", "true") +
			entry("enrollForCertificateWithTrust0", "true");
	return result + "</preference></preferences>";
}

inline std::string manifest(bool direct = true)
{
	return "<MissionPackageManifest version='2'><Configuration>"
		"<Parameter name='uid' value='fixture-package'/>"
		"</Configuration>"
		"<Contents><Content ignore='false' zipEntry='secure.pref'/>"
		"<Content ignore='false' zipEntry='trust.p12'/>" +
		std::string(direct ?
			"<Content ignore='false' zipEntry='client.p12'/>" :
			"") +
		"</Contents></MissionPackageManifest>";
}

inline EVP_PKEY *key()
{
	EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
	EVP_PKEY *result = nullptr;

	if (!context || EVP_PKEY_keygen_init(context) <= 0 ||
		EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context,
			NID_X9_62_prime256v1) <= 0 ||
		EVP_PKEY_keygen(context, &result) <= 0) {
		EVP_PKEY_free(result);
		result = nullptr;
	}
	EVP_PKEY_CTX_free(context);
	return result;
}

inline X509 *certificate(EVP_PKEY *subject, const char *name,
	EVP_PKEY *signer, X509 *issuer, bool ca)
{
	X509 *result = X509_new();

	if (!result)
		return nullptr;
	X509_set_version(result, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(result), ca ? 1 : 2);
	X509_gmtime_adj(X509_getm_notBefore(result), -60);
	X509_gmtime_adj(X509_getm_notAfter(result), 86400);
	X509_set_pubkey(result, subject);
	X509_NAME *dn = X509_get_subject_name(result);

	X509_NAME_add_entry_by_txt(dn, "CN", MBSTRING_ASC,
		reinterpret_cast<const unsigned char *>(name), -1, -1, 0);
	X509_set_issuer_name(result,
		issuer ? X509_get_subject_name(issuer) : dn);
	X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, nullptr,
		NID_basic_constraints, const_cast<char *>(ca ?
			"critical,CA:TRUE" : "critical,CA:FALSE"));

	if (extension) {
		X509_add_ext(result, extension, -1);
		X509_EXTENSION_free(extension);
	}
	if (!X509_sign(result, signer, EVP_sha256())) {
		X509_free(result);
		return nullptr;
	}
	return result;
}

inline Bytes encode(PKCS12 *bundle)
{
	if (!bundle)
		return {};
	int size = i2d_PKCS12(bundle, nullptr);
	Bytes result(size > 0 ? static_cast<size_t>(size) : 0);
	unsigned char *out = result.data();

	if (size > 0 && i2d_PKCS12(bundle, &out) != size)
		result.clear();
	PKCS12_free(bundle);
	return result;
}

struct Package {
	std::filesystem::path directory;
	Bytes trust;
	Bytes client;

	bool prepare(bool legacy = false,
		const char *client_password = "test-only-secret",
		int mac_iterations = 0)
	{
		unsigned char random[12];

		if (RAND_bytes(random, sizeof(random)) != 1)
			return false;
		std::string suffix;
		const char hex[] = "0123456789abcdef";

		for (unsigned char value : random) {
			suffix += hex[value >> 4];
			suffix += hex[value & 15];
		}
		directory = std::filesystem::temp_directory_path() /
			("tak-import-contract-" + suffix);
		if (!std::filesystem::create_directory(directory))
			return false;
		std::filesystem::permissions(directory,
			std::filesystem::perms::owner_all);
		EVP_PKEY *ca_key = key();
		EVP_PKEY *client_key = key();
		X509 *ca = ca_key ? certificate(ca_key, "Fixture CA", ca_key,
			nullptr, true) : nullptr;
		X509 *cert = ca && client_key ? certificate(client_key,
			"Fixture client", ca_key, ca, false) : nullptr;
		STACK_OF(X509) *chain = sk_X509_new_null();
		OSSL_LIB_CTX *context = legacy ? OSSL_LIB_CTX_new() : nullptr;
		OSSL_PROVIDER *normal = context ?
			OSSL_PROVIDER_load(context, "default") : nullptr;
		OSSL_PROVIDER *old = context ?
			OSSL_PROVIDER_load(context, "legacy") : nullptr;
		int key_cipher = legacy ?
			NID_pbe_WithSHA1And3_Key_TripleDES_CBC : 0;
		int cert_cipher = legacy ?
			NID_pbe_WithSHA1And40BitRC2_CBC : 0;

		if (ca && cert && chain && sk_X509_push(chain, ca) &&
			(!legacy || (context && normal && old))) {
			trust = encode(PKCS12_create_ex("test-only-secret",
				"trust", nullptr, nullptr, chain, key_cipher,
				cert_cipher, 0, mac_iterations, 0,
				context, nullptr));
			client = encode(PKCS12_create_ex(client_password,
				"client", client_key, cert, chain, key_cipher,
				cert_cipher, 0, mac_iterations, 0,
				context, nullptr));
		}
		OSSL_PROVIDER_unload(old);
		OSSL_PROVIDER_unload(normal);
		OSSL_LIB_CTX_free(context);
		sk_X509_free(chain);
		X509_free(cert);
		X509_free(ca);
		EVP_PKEY_free(client_key);
		EVP_PKEY_free(ca_key);
		return !trust.empty() && !client.empty();
	}

	Entries entries(bool direct = true, const std::string &pref = "")
	{
		Entries result = {
			{"package/MANIFEST/manifest.xml", fixtures::bytes(
				manifest(direct))},
			{"package/secure.pref", fixtures::bytes(pref.empty() ?
				preferences(direct) : pref)},
			{"package/trust.p12", trust}};

		if (direct)
			result.emplace_back("package/client.p12", client);
		return result;
	}

	std::string write(const Bytes &data,
		const std::string &name = "config.zip")
	{
		std::filesystem::path path = directory / name;
		std::ofstream output(path, std::ios::binary);

		output.write(reinterpret_cast<const char *>(data.data()),
			data.size());
		output.close();
		return output ? path.string() : std::string();
	}

	std::string wrapped()
	{
		return write(zip({{"server.zip", zip(entries(false))},
			{"trust.p12", trust}, {"client.p12", client},
			{"Links.txt", fixtures::bytes(
				"https://tak.example.invalid/")}}));
	}

	~Package()
	{
		if (!directory.empty()) {
			std::error_code ignored;

			std::filesystem::remove_all(directory, ignored);
		}
	}
};

struct Imported {
	contract::Buffer toml;
	tg_import_files *files = nullptr;

	~Imported() { tg_import_zip_destroy(files); }

	tg_status read(const std::string &path,
		tg_diagnostic *diagnostic = nullptr)
	{
		return tg_import_zip(path.c_str(), &toml.value, &files,
			diagnostic);
	}
};

inline std::string configuration(const tg_buffer &inputs)
{
	return "version = 1\n[[outputs]]\nid = 'capture'\nkind = 'tcp'\n"
		"bind = '127.0.0.1'\nport = 19000\n" + contract::string(inputs);
}

inline std::string read_file(const std::string &path)
{
	std::ifstream input(path, std::ios::binary);

	return {std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()};
}

} /* namespace import_fixtures */

#endif
