#ifndef TAK_GATEWAY_WEB_FIXTURES_HPP
#define TAK_GATEWAY_WEB_FIXTURES_HPP

#include "import_fixtures.hpp"
#include "runtime_fixtures.hpp"
#include "../../src/managed_runtime.h"
#include "../../src/web_server.h"
#include <yyjson.h>
#include <atomic>
#include <thread>

namespace web_fixtures {

struct Json {
	yyjson_doc *document = nullptr;

	explicit Json(const std::string &text)
	{
		document = yyjson_read(text.data(), text.size(), 0);
	}

	~Json() { yyjson_doc_free(document); }
	Json(const Json &) = delete;
	Json &operator=(const Json &) = delete;
	yyjson_val *root() const { return yyjson_doc_get_root(document); }
};

inline yyjson_val *field(yyjson_val *object, const char *key)
{
	return yyjson_obj_get(object, key);
}

inline std::string text(yyjson_val *value)
{
	const char *result = yyjson_get_str(value);

	return result ? result : "";
}

inline std::string identifier(yyjson_val *value)
{
	return yyjson_is_uint(value) ?
		std::to_string(yyjson_get_uint(value)) : text(value);
}

inline std::string quote(const std::string &value)
{
	tg_vec escaped{};

	tg_text(&escaped, "\"");
	tg_escape(&escaped, value.c_str(), true);
	tg_text(&escaped, "\"");
	std::string result(reinterpret_cast<const char *>(escaped.data),
		escaped.size);

	tg_vec_free(&escaped);
	return result;
}

struct Response {
	int status = 0;
	std::string headers;
	std::string body;
};

inline bool complete_response(const std::string &wire)
{
	size_t end = wire.find("\r\n\r\n");
	size_t length = wire.find("Content-Length: ");

	if (end == std::string::npos || length >= end)
		return false;
	const char *start = wire.c_str() + length + strlen("Content-Length: ");
	uint64_t size = strtoull(start, nullptr, 10);

	return size <= 4 * 1024 * 1024 &&
		wire.size() >= end + 4 + static_cast<size_t>(size);
}

struct Gateway {
	import_fixtures::Package fixture;
	runtime_fixtures::SocketEnvironment network;
	tg_store *store = nullptr;
	tg_web_app *app = nullptr;
	uint16_t port = 0;
	std::string token;
	uint64_t revision = 0;
	std::thread owner;
	std::atomic<bool> quitting{false};
	std::atomic<int> poll_error{TG_OK};
	tg_diagnostic diagnostic{};

	std::string state_path() const
	{
		return (fixture.directory / "state").string();
	}

	bool prepare(const std::string &initial = "version=1\n")
	{
		if (!network.ready || !fixture.prepare())
			return false;
		if (tg_store_open(state_path().c_str(), true, &store,
			&diagnostic) != TG_OK)
			return false;
		contract::Config config;

		if (tg_config_parse_managed(contract::view(initial),
			fixture.directory.string().c_str(), &config.value,
			&diagnostic) != TG_OK)
			return false;
		tg_web_settings settings{};

		tg_web_settings_default(&settings);
		return start(config.value, settings, 0);
	}

	bool start(tg_config *config, tg_web_settings settings,
		uint64_t selected_revision)
	{
		runtime_fixtures::Socket reservation;

		if (!runtime_fixtures::reserve(reservation, SOCK_STREAM, port))
			return false;
		settings.enabled = true;
		snprintf(settings.bind, sizeof(settings.bind), "127.0.0.1:%u",
			port);
		reservation.close();
		if (tg_web_app_create(store, config, &settings,
			selected_revision, &app, &diagnostic) != TG_OK)
			return false;
		revision = selected_revision;
		quitting = false;
		poll_error = TG_OK;
		owner = std::thread([this] {
			while (!quitting) {
				tg_status result = tg_web_app_poll(app, 5);

				if (result < 0) {
					poll_error = result;
					break;
				}
			}
		});
		return true;
	}

	void stop()
	{
		quitting = true;
		if (owner.joinable())
			owner.join();
		if (app)
			tg_web_app_stop(app, 0);
		tg_web_app_destroy(app);
		app = nullptr;
	}

	bool restart()
	{
		stop();
		tg_store_close(store);
		store = nullptr;
		if (tg_store_open(state_path().c_str(), true, &store,
			&diagnostic) != TG_OK)
			return false;
		contract::Buffer saved;
		tg_web_settings settings{};
		uint64_t selected_revision = 0;

		if (tg_store_load(store, &saved.value, &settings,
			&selected_revision, &diagnostic) != TG_OK)
			return false;
		contract::Config config;

		tg_bytes bytes{saved.value.data, saved.value.size};

		if (tg_config_parse_managed(bytes, state_path().c_str(),
			&config.value, &diagnostic) != TG_OK)
			return false;
		token.clear();
		return start(config.value, settings, selected_revision);
	}

	Response exchange(const std::string &wire)
	{
		runtime_fixtures::Socket client;
		Response response;

		if (!client.create(SOCK_STREAM))
			return response;
#ifdef _WIN32
		DWORD timeout = 5000;
#else
		timeval timeout{5, 0};
#endif
		setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char *>(&timeout),
			sizeof(timeout));
		setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO,
			reinterpret_cast<const char *>(&timeout),
			sizeof(timeout));
		sockaddr_in address = runtime_fixtures::loopback(port);

		if (connect(client.value,
			reinterpret_cast<sockaddr *>(&address),
			sizeof(address)) != 0)
			return response;
		size_t sent = 0;
		int flags = 0;
#ifdef MSG_NOSIGNAL
		flags = MSG_NOSIGNAL;
#endif

		while (sent < wire.size()) {
			int count = static_cast<int>(send(client.value,
				wire.data() + sent,
				static_cast<int>(wire.size() - sent), flags));

			if (count <= 0)
				return response;
			sent += static_cast<size_t>(count);
		}
		std::string received;
		char buffer[16384];

		for (;;) {
			int count = static_cast<int>(recv(client.value, buffer,
				sizeof(buffer), 0));

			if (count <= 0)
				break;
			received.append(buffer, static_cast<size_t>(count));
			if (received.size() > 4 * 1024 * 1024 ||
				complete_response(received))
				break;
		}
		size_t end = received.find("\r\n\r\n");

		if (end == std::string::npos)
			return response;
		sscanf(received.c_str(), "HTTP/%*u.%*u %d", &response.status);
		response.headers = received.substr(0, end);
		response.body = received.substr(end + 4);
		return response;
	}

	Response request(const std::string &method, const std::string &path,
		const std::string &body = "", bool authorized = true,
		const std::string &type = "application/json",
		const std::string &origin = "")
	{
		std::string wire = method + " " + path + " HTTP/1.1\r\nHost: " +
			"127.0.0.1:" + std::to_string(port) +
			"\r\nConnection: close\r\nContent-Type: " + type +
			"\r\nContent-Length: " + std::to_string(body.size()) +
			"\r\n";

		if (authorized && !token.empty())
			wire += "X-TG-Token: " + token + "\r\n";
		if (!origin.empty())
			wire += "Origin: " + origin + "\r\n";
		return exchange(wire + "\r\n" + body);
	}

	Response state()
	{
		Response response = request("GET", "/api/state");
		Json document(response.body);

		if (response.status == 200 && document.document) {
			token = text(field(document.root(), "token"));
			revision = yyjson_get_uint(
				field(document.root(), "revision"));
		}
		return response;
	}

	Response wait(const Response &accepted, const std::string &expected)
	{
		Json submitted(accepted.body);
		std::string id = identifier(field(submitted.root(), "job_id"));
		Response result;

		if (id.empty())
			return result;
		auto end = std::chrono::steady_clock::now() +
			std::chrono::seconds(5);

		do {
			result = request("GET", "/api/jobs/" + id);
			Json job(result.body);
			std::string status = text(field(job.root(), "state"));

			if (status == expected || status == "failed")
				return result;
			std::this_thread::sleep_for(
				std::chrono::milliseconds(5));
		} while (std::chrono::steady_clock::now() < end);
		return result;
	}

	std::string edit(const std::string &kind, const std::string &record,
		uint64_t selected_revision) const
	{
		return "{\"revision\":" + std::to_string(selected_revision) +
			",\"" + kind + "\":" + record + "}";
	}

	~Gateway()
	{
		stop();
		tg_store_close(store);
	}
};

inline uint16_t free_port(int kind)
{
	runtime_fixtures::Socket socket;
	uint16_t result = 0;

	return runtime_fixtures::reserve(socket, kind, result) ? result : 0;
}

inline std::string udp_input(const std::string &id, uint16_t port)
{
	return "{\"id\":" + quote(id) + ",\"name\":\"UDP source\","
		"\"kind\":\"udp\",\"bind\":\"127.0.0.1\",\"port\":" +
		std::to_string(port) + "}";
}

inline std::string tcp_output(const std::string &id, uint16_t port)
{
	return "{\"id\":" + quote(id) + ",\"name\":\"TCP consumer\","
		"\"kind\":\"tcp\",\"bind\":\"127.0.0.1\",\"port\":" +
		std::to_string(port) + ",\"mode\":\"listen\"}";
}

} /* namespace web_fixtures */

#endif
