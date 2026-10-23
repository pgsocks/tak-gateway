#ifndef TAK_GATEWAY_RUNTIME_FIXTURES_HPP
#define TAK_GATEWAY_RUNTIME_FIXTURES_HPP

#include "contract_support.hpp"
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace runtime_fixtures {

#ifdef _WIN32
using NativeSocket = SOCKET;
static constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
static constexpr NativeSocket invalid_socket = -1;
#endif

struct SocketEnvironment {
	bool ready = true;

	SocketEnvironment()
	{
#ifdef _WIN32
		WSADATA data{};

		ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
	}

	~SocketEnvironment()
	{
#ifdef _WIN32
		if (ready)
			WSACleanup();
#endif
	}
};

struct Socket {
	NativeSocket value = invalid_socket;

	Socket() = default;
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;
	~Socket() { close(); }

	void close()
	{
		if (value == invalid_socket)
			return;
#ifdef _WIN32
		closesocket(value);
#else
		::close(value);
#endif
		value = invalid_socket;
	}

	bool create(int kind)
	{
		value = socket(AF_INET, kind, 0);
		return value != invalid_socket;
	}

	bool nonblocking()
	{
#ifdef _WIN32
		u_long mode = 1;

		return ioctlsocket(value, FIONBIO, &mode) == 0;
#else
		int flags = fcntl(value, F_GETFL, 0);

		return flags >= 0 && fcntl(value, F_SETFL,
			flags | O_NONBLOCK) == 0;
#endif
	}
};

inline sockaddr_in loopback(uint16_t port)
{
	sockaddr_in address{};

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	return address;
}

inline bool reserve(Socket &socket, int kind, uint16_t &port)
{
	if (!socket.create(kind))
		return false;
	sockaddr_in address = loopback(0);

	if (bind(socket.value, reinterpret_cast<sockaddr *>(&address),
		sizeof(address)) != 0)
		return false;
#ifdef _WIN32
	int size = sizeof(address);
#else
	socklen_t size = sizeof(address);
#endif
	if (getsockname(socket.value, reinterpret_cast<sockaddr *>(&address),
		&size) != 0)
		return false;
	port = ntohs(address.sin_port);
	return port != 0;
}

inline bool would_block()
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

struct GatewayFixture {
	SocketEnvironment environment;
	Socket udp_reservation;
	Socket tcp_reservation;
	Socket sender;
	Socket consumer;
	contract::Config config;
	contract::Runtime runtime;
	uint16_t input_port = 0;
	uint16_t output_port = 0;
	std::string text;
	std::string received;
	tg_diagnostic diagnostic{};

	bool prepare()
	{
		if (!environment.ready ||
		    !reserve(udp_reservation, SOCK_DGRAM, input_port) ||
		    !reserve(tcp_reservation, SOCK_STREAM, output_port))
			return false;
		text = "version = 1\n[[inputs]]\nid = \"source\"\n"
			"kind = \"udp\"\nbind = \"127.0.0.1\"\nport = " +
			std::to_string(input_port) + "\nencoding = \"auto\"\n"
			"[[outputs]]\nid = \"capture\"\nkind = \"tcp\"\n"
			"mode = \"listen\"\nbind = \"127.0.0.1\"\nport = " +
			std::to_string(output_port) + "\nframing = \"u32be\"\n";
		return true;
	}

	tg_status create()
	{
		tg_status result = tg_config_parse(contract::view(text),
			contract::base_directory(),
			&config.value, &diagnostic);

		if (result != TG_OK)
			return result;
		return tg_runtime_create(config.value, &runtime.value,
			&diagnostic);
	}

	tg_status start()
	{
		udp_reservation.close();
		tcp_reservation.close();
		return tg_runtime_poll(runtime.value, 0);
	}

	bool connect_consumer()
	{
		if (!consumer.create(SOCK_STREAM))
			return false;
		sockaddr_in address = loopback(output_port);

		if (connect(consumer.value,
			reinterpret_cast<sockaddr *>(&address),
			sizeof(address)) != 0 || !consumer.nonblocking())
			return false;
		return tg_runtime_poll(runtime.value, 0) == TG_OK;
	}

	bool send(const std::string &xml)
	{
		if (sender.value == invalid_socket &&
		    !sender.create(SOCK_DGRAM))
			return false;
		sockaddr_in address = loopback(input_port);
		int written = static_cast<int>(sendto(sender.value, xml.data(),
			static_cast<int>(xml.size()), 0,
			reinterpret_cast<sockaddr *>(&address),
			sizeof(address)));

		return written >= 0 &&
			static_cast<size_t>(written) == xml.size();
	}

	bool await_bytes(size_t size, uint32_t timeout_ms = 2000)
	{
		auto end = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeout_ms);

		while (std::chrono::steady_clock::now() < end) {
			if (tg_runtime_poll(runtime.value, 5) != TG_OK)
				return false;
			char buffer[4096];
			int count = static_cast<int>(recv(consumer.value,
				buffer,
				sizeof(buffer), 0));

			if (count > 0)
				received.append(buffer,
					static_cast<size_t>(count));
			else if (count == 0 || !would_block())
				return false;
			if (received.size() >= size)
				return true;
		}
		return false;
	}
};

} /* namespace runtime_fixtures */

#endif
