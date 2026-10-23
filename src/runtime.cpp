#include "config_internal.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <netioapi.h>
#include <sddl.h>
using tg_socket = SOCKET;
using tg_pollfd = WSAPOLLFD;
static const tg_socket bad_socket = INVALID_SOCKET;
#define tg_poll WSAPoll
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
using tg_socket = int;
using tg_pollfd = pollfd;
static const tg_socket bad_socket = -1;
#define tg_poll poll
#endif
#include <ares.h>
#include <openssl/ssl.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <errno.h>
#include <time.h>

uint64_t tg_monotonic_ms(void)
{
#ifdef _WIN32
	return GetTickCount64();
#else
	timespec t{};
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}
uint64_t tg_utc_ms(void)
{
#ifdef _WIN32
	FILETIME f;
	GetSystemTimeAsFileTime(&f);
	ULARGE_INTEGER t;
	t.LowPart = f.dwLowDateTime;
	t.HighPart = f.dwHighDateTime;
	return t.QuadPart / 10000 - UINT64_C(11644473600000);
#else
	timespec t{};
	clock_gettime(CLOCK_REALTIME, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}
static void close_socket(tg_socket *s)
{
	if (*s == bad_socket)
		return;
#ifdef _WIN32
	closesocket(*s);
#else
	close(*s);
#endif
	*s = bad_socket;
}
static bool would_block(void)
{
#ifdef _WIN32
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK ||
	       errno == EINPROGRESS || errno == EINTR;
#endif
}
static bool nonblocking(tg_socket s)
{
#ifdef _WIN32
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK))
		return false;
	fcntl(s, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	int on = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
	return true;
#endif
}
#ifndef _WIN32
struct signal_mask {
	sigset_t previous;
	bool pending;
	bool blocked;
};
static signal_mask block_pipe(void)
{
	signal_mask m{};
	sigset_t mask, pending;
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	m.blocked = pthread_sigmask(SIG_BLOCK, &mask, &m.previous) == 0;
	sigpending(&pending);
	m.pending = sigismember(&pending, SIGPIPE) == 1;
	return m;
}
static void restore_pipe(signal_mask *m)
{
	if (!m->blocked)
		return;
	if (!m->pending) {
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGPIPE);
		timespec zero{};
		while (sigtimedwait(&mask, nullptr, &zero) >= 0) {
		}
	}
	pthread_sigmask(SIG_SETMASK, &m->previous, nullptr);
}
#endif
static int socket_send(tg_socket s, const uint8_t *p, size_t n)
{
#ifdef MSG_NOSIGNAL
	return (int)send(s, (const char *)p, (int)n, MSG_NOSIGNAL);
#else
	return (int)send(s, (const char *)p, (int)n, 0);
#endif
}
struct resolver {
	ares_channel_t *channel;
	tg_pollfd sockets[16];
	size_t count;
	bool pending;
	bool ready;
	int result;
	sockaddr_storage address;
	int length;
	uint64_t rotation;
};
static void dns_socket(void *user, ares_socket_t fd, int read, int write)
{
	resolver *r = static_cast<resolver *>(user);
	size_t i = 0;
	while (i < r->count && r->sockets[i].fd != (tg_socket)fd)
		++i;
	if (!read && !write) {
		if (i < r->count)
			r->sockets[i] = r->sockets[--r->count];
		return;
	}
	if (i == r->count) {
		if (r->count == 16)
			return;
		++r->count;
	}
	r->sockets[i] = {(tg_socket)fd,
		(short)((read ? POLLIN : 0) | (write ? POLLOUT : 0)), 0};
}
static void dns_result(void *user, int status, int, ares_addrinfo *addresses)
{
	resolver *r = static_cast<resolver *>(user);
	r->pending = false;
	r->ready = true;
	r->result = status;
	if (status == ARES_SUCCESS && addresses) {
		size_t count = 0;
		for (ares_addrinfo_node *n = addresses->nodes; n;
			n = n->ai_next)
			++count;
		size_t selected = count ? (size_t)(r->rotation++ % count) : 0;
		ares_addrinfo_node *n = addresses->nodes;
		while (n && selected--)
			n = n->ai_next;
		if (n && n->ai_addrlen <= sizeof(r->address)) {
			memcpy(&r->address, n->ai_addr, n->ai_addrlen);
			r->length = (int)n->ai_addrlen;
		} else
			r->result = ARES_ENODATA;
	}
	if (addresses)
		ares_freeaddrinfo(addresses);
}
static bool resolve_start(
	resolver *r, const char *host, uint16_t port, int type)
{
	r->ready = false;
	r->pending = true;
	if (!r->channel) {
		ares_options options{};
		options.sock_state_cb = dns_socket;
		options.sock_state_cb_data = r;
		options.timeout = 2000;
		options.tries = 2;
		if (ares_init_options(&r->channel, &options,
			    ARES_OPT_SOCK_STATE_CB | ARES_OPT_TIMEOUTMS |
				    ARES_OPT_TRIES) != ARES_SUCCESS)
			return false;
	}
	ares_addrinfo_hints hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = type;
	char service[8];
	snprintf(service, sizeof(service), "%u", port);
	ares_getaddrinfo(r->channel, host, service, &hints, dns_result, r);
	return true;
}
static void resolve_tick(resolver *r)
{
	if (!r->channel || !r->pending)
		return;
	tg_pollfd fds[16];
	size_t count = r->count;
	memcpy(fds, r->sockets, count * sizeof(*fds));
	if (count && tg_poll(fds, (unsigned long)count, 0) > 0)
		for (size_t i = 0; i < count; ++i)
			if (fds[i].revents)
				ares_process_fd(r->channel,
					(fds[i].revents &
						(POLLIN | POLLERR | POLLHUP))
						? fds[i].fd
						: ARES_SOCKET_BAD,
					(fds[i].revents & POLLOUT)
						? fds[i].fd
						: ARES_SOCKET_BAD);
	ares_process_fd(r->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
}
static bool numeric_address(const char *host, uint16_t port, int type,
	sockaddr_storage *address, int *length)
{
	addrinfo hints{}, *list = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = type;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	char service[8];
	snprintf(service, sizeof(service), "%u", port);
	if (getaddrinfo(host, service, &hints, &list))
		return false;
	bool ok = list && list->ai_addrlen <= sizeof(*address);
	if (ok) {
		memcpy(address, list->ai_addr, list->ai_addrlen);
		*length = (int)list->ai_addrlen;
	}
	if (list)
		freeaddrinfo(list);
	return ok;
}
struct runtime_source {
	tg_socket socket;
	resolver dns;
	SSL_CTX *tls;
	SSL *ssl;
	tg_session *session;
	tg_buffer password;
	tg_buffer key_password;
	tg_buffer identity;
	int phase;
	uint64_t phase_time;
	uint64_t retry;
	uint64_t delay;
	uint64_t reconnects;
	uint64_t accepted;
	uint64_t rejected;
	const char *failure;
	unsigned interface_index;
};
struct runtime_sink {
	tg_delivery_info history;
	tg_socket socket;
	resolver dns;
	tg_output_id id;
	int phase;
	uint64_t retry;
	uint64_t phase_time;
#ifndef _WIN32
	int pipe;
#else
	HANDLE pipe;
	OVERLAPPED connect_io;
	OVERLAPPED write_io;
	bool connecting;
	bool writing;
#endif
};
struct runtime_client {
	tg_socket socket;
	tg_output_id id;
	size_t sink;
	uint64_t progress;
	runtime_client *next;
};
struct tg_runtime {
	tg_config *config;
	tg_runtime_state state;
	tg_delivery *delivery;
	runtime_source *sources;
	runtime_sink *sinks;
	runtime_client *clients;
	uint64_t drain_start;
	uint32_t drain_ms;
	uint64_t status_time;
	uint64_t now;
	bool initialized;
	bool ares_ready;
#ifdef _WIN32
	bool winsock;
#endif
};
static bool same(const char *a, const char *b)
{
	return a && !strcmp(a, b);
}
static void buffer_secret_free(tg_buffer *b)
{
	if (b->data) {
		volatile uint8_t *p = b->data;
		for (size_t i = 0; i < b->size; ++i)
			p[i] = 0;
	}
	tg_buffer_release(b);
}
static tg_status load_secret(const char *file, const char *env, tg_buffer *out)
{
	if (!file && !env)
		return TG_OK;
	if (env) {
		const char *s = getenv(env);
		if (!s || !*s)
			return TG_IO;
		tg_vec v{};
		if (!tg_text(&v, s))
			return TG_NO_MEMORY;
		*out = {v.data, v.size};
		return TG_OK;
	}
	tg_buffer raw{};
	tg_status status = tg_read_file(file, 65536, true, &raw);
	if (status < 0)
		return status;
	if (raw.size && raw.data[raw.size - 1] == '\n')
		--raw.size;
	if (raw.size && raw.data[raw.size - 1] == '\r')
		--raw.size;
	if (!raw.size || !tg_utf8({raw.data, raw.size})) {
		buffer_secret_free(&raw);
		return TG_INVALID;
	}
	tg_vec v{};
	if (!tg_append(&v, raw.data, raw.size))
		status = TG_NO_MEMORY;
	else
		*out = {v.data, v.size};
	buffer_secret_free(&raw);
	return status;
}
static int key_password(char *buffer, int size, int, void *user)
{
	tg_buffer *b = static_cast<tg_buffer *>(user);
	if (b->size >= (size_t)size)
		return 0;
	if (b->size)
		memcpy(buffer, b->data, b->size);
	buffer[b->size] = 0;
	return (int)b->size;
}
static tg_status source_tls_create(const tg_input_config *in, runtime_source *s)
{
	tg_status status =
		load_secret(in->password_file, in->password_env, &s->password);
	if (status < 0)
		return status;
	status = load_secret(in->pkcs12_file ? in->pkcs12_password_file
					     : in->key_password_file,
		in->pkcs12_file ? in->pkcs12_password_env
				: in->key_password_env,
		&s->key_password);
	if (status < 0)
		return status;
	if (in->identity_file) {
		status = tg_read_file(
			in->identity_file, 8388608, false, &s->identity);
		if (status < 0)
			return status;
	}
	s->tls = SSL_CTX_new(TLS_client_method());
	if (!s->tls)
		return TG_NO_MEMORY;
	SSL_CTX_set_min_proto_version(s->tls, TLS1_2_VERSION);
	SSL_CTX_set_verify(s->tls, SSL_VERIFY_PEER, nullptr);
	if (!SSL_CTX_load_verify_locations(s->tls, in->ca_file, nullptr))
		return TG_IO;
	SSL_CTX_set_default_passwd_cb(s->tls, key_password);
	SSL_CTX_set_default_passwd_cb_userdata(s->tls, &s->key_password);
	if (in->pkcs12_file) {
		tg_buffer data{};
		status = tg_read_file(in->pkcs12_file, 8388608, true, &data);
		if (status < 0)
			return status;
		const unsigned char *p = data.data;
		PKCS12 *bundle = d2i_PKCS12(nullptr, &p, (long)data.size);
		EVP_PKEY *key = nullptr;
		X509 *certificate = nullptr;
		STACK_OF(X509) *chain = nullptr;
		int ok = bundle &&
			 PKCS12_parse(bundle,
				 s->key_password.data
					 ? (const char *)s->key_password.data
					 : "",
				 &key, &certificate, &chain);
		if (ok)
			ok = SSL_CTX_use_certificate(s->tls, certificate) &&
			     SSL_CTX_use_PrivateKey(s->tls, key);
		while (chain && sk_X509_num(chain)) {
			X509 *extra = sk_X509_shift(chain);
			if (!SSL_CTX_add_extra_chain_cert(s->tls, extra)) {
				X509_free(extra);
				ok = 0;
			}
		}
		sk_X509_free(chain);
		X509_free(certificate);
		EVP_PKEY_free(key);
		PKCS12_free(bundle);
		buffer_secret_free(&data);
		if (!ok)
			return TG_IO;
	} else if (in->cert_file &&
		   (!SSL_CTX_use_certificate_chain_file(
			    s->tls, in->cert_file) ||
			   !SSL_CTX_use_PrivateKey_file(
				   s->tls, in->key_file, SSL_FILETYPE_PEM)))
		return TG_IO;
	if ((in->cert_file || in->pkcs12_file) &&
		!SSL_CTX_check_private_key(s->tls))
		return TG_IO;
	return TG_OK;
}
static void source_disconnect(runtime_source *s)
{
	if (s->ssl) {
		SSL_free(s->ssl);
		s->ssl = nullptr;
	}
	close_socket(&s->socket);
	if (s->session) {
		tg_session_destroy(s->session);
		s->session = nullptr;
	}
}
static void source_retry(runtime_source *s, uint64_t now, const char *failure)
{
	if (s->phase == 4 && now - s->phase_time >= 60000)
		s->delay = 1000;
	source_disconnect(s);
	if (!s->delay)
		s->delay = 1000;
	unsigned random = 0;
	RAND_bytes((unsigned char *)&random, sizeof(random));
	s->retry = now + s->delay + random % (s->delay / 4 + 1);
	s->delay = s->delay < 30000 ? s->delay * 2 : 48000;
	s->phase = 0;
	s->failure = failure;
	++s->reconnects;
}
static tg_status publish(tg_runtime *r, runtime_source *source,
	tg_encoding encoding, tg_bytes bytes)
{
	tg_buffer xml{};
	tg_normalize_report report{};
	tg_status status = tg_normalize(encoding, bytes,
		r->config->preservation, &r->config->limits, &xml, &report);
	if (status == TG_NO_EVENT)
		return TG_OK;
	if (status != TG_OK) {
		++source->rejected;
		return status == TG_NO_MEMORY ? status : TG_OK;
	}
	tg_publish_report delivery{};
	status = tg_delivery_publish(
		r->delivery, {xml.data, xml.size}, &delivery);
	tg_buffer_release(&xml);
	++source->accepted;
	if (status == TG_LIMIT && r->config->overflow == TG_OVERFLOW_FAIL)
		return TG_LIMIT;
	return status == TG_NO_MEMORY ? status : TG_OK;
}
static void source_messages(tg_runtime *r, runtime_source *s, tg_bytes bytes)
{
	for (size_t at = 0; at < bytes.size;) {
		tg_message message{};
		size_t used = 0;
		tg_status status = tg_session_receive(s->session,
			{bytes.data + at, bytes.size - at}, r->now, &used,
			&message);
		at += used;
		if (status == TG_OK) {
			status = publish(r, s, message.encoding,
				{message.bytes.data, message.bytes.size});
			if (status < 0)
				r->state = TG_RUNTIME_FAILED;
		}
		tg_buffer_release(&message.bytes);
		if (status < 0 || !used) {
			source_retry(s, r->now, "protocol");
			return;
		}
	}
}
static int socket_connected(tg_socket fd)
{
	tg_pollfd p{fd, POLLOUT, 0};
	if (tg_poll(&p, 1, 0) <= 0)
		return false;
	int error = 0;
#ifdef _WIN32
	int size = sizeof(error);
#else
	socklen_t size = sizeof(error);
#endif
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) ||
		error)
		return -1;
	return 1;
}
static void source_tls_poll(tg_runtime *r, size_t index)
{
	runtime_source *s = &r->sources[index];
	const tg_input_config *in = &r->config->inputs[index];
	if (r->now < s->retry)
		return;
	if (!s->phase) {
		s->phase = 1;
		s->phase_time = r->now;
		if (!resolve_start(&s->dns, in->host, in->port, SOCK_STREAM)) {
			source_retry(s, r->now, "dns");
			return;
		}
	}
	if (s->phase == 1) {
		resolve_tick(&s->dns);
		if (!s->dns.ready)
			return;
		if (s->dns.result != ARES_SUCCESS) {
			source_retry(s, r->now, "dns");
			return;
		}
		s->socket = socket(s->dns.address.ss_family, SOCK_STREAM, 0);
		if (s->socket == bad_socket || !nonblocking(s->socket)) {
			source_retry(s, r->now, "socket");
			return;
		}
		int on = 1;
		setsockopt(s->socket, SOL_SOCKET, SO_KEEPALIVE,
			(const char *)&on, sizeof(on));
		int result = connect(
			s->socket, (sockaddr *)&s->dns.address, s->dns.length);
		if (result && !would_block()) {
			source_retry(s, r->now, "connect");
			return;
		}
		s->phase = 2;
		s->phase_time = r->now;
	}
	if (s->phase == 2) {
		int connected = socket_connected(s->socket);
		if (connected != 1) {
			if (connected < 0 || r->now - s->phase_time >= 10000)
				source_retry(s, r->now,
					connected < 0 ? "connect"
						      : "connect-timeout");
			return;
		}
		s->ssl = SSL_new(s->tls);
		if (!s->ssl || !SSL_set_fd(s->ssl, (int)s->socket)) {
			source_retry(s, r->now, "tls-init");
			return;
		}
		unsigned char address[16];
		bool ip = inet_pton(AF_INET, in->server_name, address) == 1 ||
			  inet_pton(AF_INET6, in->server_name, address) == 1;
		int ok = ip ? X509_VERIFY_PARAM_set1_ip_asc(
				      SSL_get0_param(s->ssl), in->server_name)
			    : SSL_set1_host(s->ssl, in->server_name);
		if (!ok || (!ip && !SSL_set_tlsext_host_name(
					   s->ssl, in->server_name))) {
			source_retry(s, r->now, "tls-identity");
			return;
		}
		SSL_set_connect_state(s->ssl);
		s->phase = 3;
		s->phase_time = r->now;
	}
#ifndef _WIN32
	signal_mask mask = block_pipe();
#endif
	if (s->phase == 3) {
		ERR_clear_error();
		int result = SSL_connect(s->ssl);
		if (result != 1) {
			int error = SSL_get_error(s->ssl, result);
			if ((error != SSL_ERROR_WANT_READ &&
				    error != SSL_ERROR_WANT_WRITE) ||
				r->now - s->phase_time >= 10000)
				source_retry(s, r->now, "tls-handshake");
#ifndef _WIN32
			restore_pipe(&mask);
#endif
			return;
		}
		tg_session_options options = in->session;
		options.username = in->username;
		options.password = (const char *)s->password.data;
		options.identity_xml = {s->identity.data, s->identity.size};
		if (tg_session_create(&options, &r->config->limits, r->now,
			    &s->session) != TG_OK)
			source_retry(s, r->now, "session-init");
		else {
			s->phase = 4;
			s->phase_time = r->now;
			s->failure = nullptr;
		}
	}
	if (s->phase == 4) {
		if (tg_session_tick(s->session, r->now, tg_utc_ms()) < 0)
			source_retry(s, r->now, "protocol-timeout");
		else {
			tg_bytes write{};
			if (tg_session_peek_write(s->session, &write) ==
				TG_OK) {
				size_t submitted = 0;
				ERR_clear_error();
				int result = SSL_write_ex(s->ssl, write.data,
					write.size, &submitted);
				if (result == 1)
					tg_session_consume_write(
						s->session, submitted, r->now);
				else {
					int error =
						SSL_get_error(s->ssl, result);
					if (error != SSL_ERROR_WANT_READ &&
						error != SSL_ERROR_WANT_WRITE)
						source_retry(
							s, r->now, "tls-write");
				}
			}
			if (s->phase == 4) {
				uint8_t bytes[65536];
				size_t n = 0;
				ERR_clear_error();
				int result = SSL_read_ex(
					s->ssl, bytes, sizeof(bytes), &n);
				if (result == 1) {
					source_messages(r, s, {bytes, n});
				} else {
					int error =
						SSL_get_error(s->ssl, result);
					if (error != SSL_ERROR_WANT_READ &&
						error != SSL_ERROR_WANT_WRITE)
						source_retry(
							s, r->now, "tls-read");
				}
			}
		}
	}
#ifndef _WIN32
	restore_pipe(&mask);
#endif
}
static bool peer_allowed(
	const tg_input_config *in, const sockaddr_storage *peer)
{
	if (!in->peer_count)
		return true;
	const uint8_t *source =
		peer->ss_family == AF_INET
			? (const uint8_t *)&((const sockaddr_in *)peer)
				  ->sin_addr
			: (const uint8_t *)&((const sockaddr_in6 *)peer)
				  ->sin6_addr;
	int family = peer->ss_family;
	unsigned maximum = family == AF_INET ? 32 : 128;
	for (size_t i = 0; i < in->peer_count; ++i) {
		char text[256];
		snprintf(text, sizeof(text), "%s", in->allow_peers[i]);
		char *slash = strchr(text, '/');
		unsigned prefix = maximum;
		if (slash) {
			*slash++ = 0;
			char *end;
			unsigned long n = strtoul(slash, &end, 10);
			if (*end || n > maximum)
				continue;
			prefix = (unsigned)n;
		}
		uint8_t address[16];
		if (inet_pton(family, text, address) != 1)
			continue;
		unsigned whole = prefix / 8, tail = prefix % 8;
		if (memcmp(source, address, whole))
			continue;
		if (!tail || (source[whole] & (0xff << (8 - tail))) ==
				     (address[whole] & (0xff << (8 - tail))))
			return true;
	}
	return false;
}
static tg_status udp_initialize(tg_runtime *r, size_t index)
{
	runtime_source *s = &r->sources[index];
	const tg_input_config *in = &r->config->inputs[index];
	bool multicast = same(in->kind, "multicast");
	sockaddr_storage address{};
	int length = 0;
	const char *bind_host = in->bind;
	if (multicast && strchr(in->group, ':'))
		bind_host = "::";
	if (!numeric_address(
		    bind_host, in->port, SOCK_DGRAM, &address, &length))
		return TG_IO;
	s->socket = socket(address.ss_family, SOCK_DGRAM, 0);
	if (s->socket == bad_socket || !nonblocking(s->socket))
		return TG_IO;
	if (multicast) {
		int yes = 1;
		setsockopt(s->socket, SOL_SOCKET, SO_REUSEADDR,
			(const char *)&yes, sizeof(yes));
	}
	if (bind(s->socket, (sockaddr *)&address, length))
		return TG_IO;
	if (multicast) {
#ifdef __linux__
		int yes = 1;
		int level = address.ss_family == AF_INET ? IPPROTO_IP
							 : IPPROTO_IPV6;
		int option = address.ss_family == AF_INET ? IP_PKTINFO
							  : IPV6_RECVPKTINFO;
		if (setsockopt(s->socket, level, option, &yes, sizeof(yes)))
			return TG_IO;
#endif
		s->interface_index = if_nametoindex(in->interface_name);
		if (!s->interface_index)
			return TG_IO;
		if (address.ss_family == AF_INET6) {
			ipv6_mreq request{};
			if (inet_pton(AF_INET6, in->group,
				    &request.ipv6mr_multiaddr) != 1)
				return TG_IO;
			request.ipv6mr_interface = s->interface_index;
			if (setsockopt(s->socket, IPPROTO_IPV6, IPV6_JOIN_GROUP,
				    (const char *)&request, sizeof(request)))
				return TG_IO;
		} else {
#ifdef __linux__
			ip_mreqn request{};
			request.imr_ifindex = (int)s->interface_index;
			if (inet_pton(AF_INET, in->group,
				    &request.imr_multiaddr) != 1)
				return TG_IO;
			if (setsockopt(s->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				    &request, sizeof(request)))
				return TG_IO;
			int no = 0;
			setsockopt(s->socket, IPPROTO_IP, IP_MULTICAST_ALL, &no,
				sizeof(no));
#else
			ip_mreq request{};
			if (inet_pton(AF_INET, in->group,
				    &request.imr_multiaddr) != 1 ||
				inet_pton(AF_INET, in->interface_name,
					&request.imr_interface) != 1)
				return TG_IO;
			if (setsockopt(s->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				    (const char *)&request, sizeof(request)))
				return TG_IO;
#endif
		}
	}
	s->phase = 4;
	return TG_OK;
}
#ifdef __linux__
static bool multicast_packet(
	const tg_input_config *in, unsigned interface_index, msghdr *packet)
{
	if (!same(in->kind, "multicast"))
		return true;
	if (packet->msg_flags & MSG_CTRUNC)
		return false;
	for (cmsghdr *c = CMSG_FIRSTHDR(packet); c;
		c = CMSG_NXTHDR(packet, c)) {
		if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO &&
			c->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
			const in_pktinfo *info =
				(const in_pktinfo *)CMSG_DATA(c);
			in_addr group{};
			return inet_pton(AF_INET, in->group, &group) == 1 &&
			       (unsigned)info->ipi_ifindex == interface_index &&
			       group.s_addr == info->ipi_addr.s_addr;
		}
		if (c->cmsg_level == IPPROTO_IPV6 &&
			c->cmsg_type == IPV6_PKTINFO &&
			c->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
			const in6_pktinfo *info =
				(const in6_pktinfo *)CMSG_DATA(c);
			in6_addr group{};
			return inet_pton(AF_INET6, in->group, &group) == 1 &&
			       info->ipi6_ifindex == interface_index &&
			       !memcmp(&group, &info->ipi6_addr, sizeof(group));
		}
	}
	return false;
}
#endif
static tg_status udp_poll(tg_runtime *r, size_t index)
{
	runtime_source *s = &r->sources[index];
	const tg_input_config *in = &r->config->inputs[index];
	for (unsigned work = 0; work < 16; ++work) {
		uint8_t bytes[65536];
		sockaddr_storage peer{};
		int n;
		bool truncated = false;
#ifdef _WIN32
		int length = sizeof(peer);
		n = recvfrom(s->socket, (char *)bytes, sizeof(bytes), 0,
			(sockaddr *)&peer, &length);
		if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) {
			++s->rejected;
			continue;
		}
#else
		iovec data{bytes, sizeof(bytes)};
		msghdr packet{};
		packet.msg_name = &peer;
		packet.msg_namelen = sizeof(peer);
		packet.msg_iov = &data;
		packet.msg_iovlen = 1;
#ifdef __linux__
		union {
			cmsghdr alignment;
			uint8_t bytes[256];
		} metadata{};
		packet.msg_control = metadata.bytes;
		packet.msg_controllen = sizeof(metadata.bytes);
#endif
		n = (int)recvmsg(s->socket, &packet, 0);
		truncated = packet.msg_flags & MSG_TRUNC;
#endif
		if (n < 0) {
			if (would_block())
				return TG_OK;
			return TG_IO;
		}
#ifdef __linux__
		if (!multicast_packet(in, s->interface_index, &packet)) {
			++s->rejected;
			continue;
		}
#endif
		if (!peer_allowed(in, &peer)) {
			++s->rejected;
			continue;
		}
		tg_message message{};
		tg_status status =
			tg_datagram_decode(in->encoding, {bytes, (size_t)n},
				truncated, &r->config->limits, &message);
		if (status == TG_OK)
			status = publish(r, s, message.encoding,
				{message.bytes.data, message.bytes.size});
		else
			++s->rejected;
		tg_buffer_release(&message.bytes);
		if (status == TG_NO_MEMORY ||
			(status == TG_LIMIT &&
				r->config->overflow == TG_OVERFLOW_FAIL))
			return status;
	}
	return TG_OK;
}
static void retire_output(tg_runtime *r, size_t sink, tg_output_id id)
{
	if (!id)
		return;
	tg_delivery_info info{};
	tg_delivery_snapshot(r->delivery, id, &info);
	tg_delivery_info *history = &r->sinks[sink].history;
	history->records_completed += info.records_completed;
	history->records_dropped += info.records_dropped + info.queued_records;
	history->udp_oversize += info.udp_oversize;
	tg_delivery_remove_output(r->delivery, id);
}
static void sink_disconnect(tg_runtime *r, size_t index)
{
	runtime_sink *s = &r->sinks[index];
	close_socket(&s->socket);
	if (s->id) {
		retire_output(r, index, s->id);
		s->id = 0;
	}
#ifndef _WIN32
	if (s->pipe >= 0) {
		close(s->pipe);
		s->pipe = -1;
	}
#else
	if (s->pipe != INVALID_HANDLE_VALUE) {
		CancelIoEx(s->pipe, nullptr);
		DisconnectNamedPipe(s->pipe);
		CloseHandle(s->pipe);
		s->pipe = INVALID_HANDLE_VALUE;
	}
	if (s->connect_io.hEvent)
		CloseHandle(s->connect_io.hEvent);
	if (s->write_io.hEvent)
		CloseHandle(s->write_io.hEvent);
	s->connect_io = {};
	s->write_io = {};
	s->connecting = false;
	s->writing = false;
#endif
	s->phase = 0;
	s->retry = r->now + 1000;
}
static tg_status pipe_initialize(const tg_output_config *o)
{
#ifndef _WIN32
	struct stat st{};
	if (lstat(o->path, &st)) {
		if (errno != ENOENT || !o->create ||
			mkfifo(o->path, o->permissions))
			return TG_IO;
		if (lstat(o->path, &st))
			return TG_IO;
	}
	if (!S_ISFIFO(st.st_mode))
		return TG_IO;
#else
	if (strncmp(o->path, "\\\\.\\pipe\\", 9))
		return TG_IO;
#endif
	return TG_OK;
}
static tg_status initialize(tg_runtime *r)
{
	for (size_t i = 0; i < r->config->output_count; ++i) {
		const tg_output_config *o = &r->config->outputs[i];
		runtime_sink *s = &r->sinks[i];
		if (!o->enabled)
			continue;
		if (same(o->kind, "pipe")) {
			tg_status status = pipe_initialize(o);
			if (status < 0)
				return status;
		} else if (same(o->kind, "tcp") && same(o->mode, "listen")) {
			sockaddr_storage address{};
			int length = 0;
			if (!numeric_address(o->bind, o->port, SOCK_STREAM,
				    &address, &length))
				return TG_IO;
			s->socket = socket(address.ss_family, SOCK_STREAM, 0);
			if (s->socket == bad_socket || !nonblocking(s->socket))
				return TG_IO;
			if (bind(s->socket, (sockaddr *)&address, length) ||
				listen(s->socket, (int)o->max_clients))
				return TG_IO;
			s->phase = 4;
		}
	}
	for (size_t i = 0; i < r->config->input_count; ++i)
		if (r->config->inputs[i].enabled &&
			!same(r->config->inputs[i].kind, "tls")) {
			tg_status status = udp_initialize(r, i);
			if (status < 0)
				return status;
		}
	r->initialized = true;
	r->state = TG_RUNTIME_RUNNING;
	return TG_OK;
}
static void accept_clients(tg_runtime *r, size_t index)
{
	const tg_output_config *o = &r->config->outputs[index];
	for (unsigned work = 0; work < 16; ++work) {
		tg_socket socket =
			accept(r->sinks[index].socket, nullptr, nullptr);
		if (socket == bad_socket)
			return;
		size_t count = 0;
		for (runtime_client *c = r->clients; c; c = c->next)
			if (c->sink == index)
				++count;
		if (count >= o->max_clients || !nonblocking(socket)) {
			close_socket(&socket);
			continue;
		}
		runtime_client *c =
			static_cast<runtime_client *>(calloc(1, sizeof(*c)));
		if (!c) {
			close_socket(&socket);
			r->state = TG_RUNTIME_FAILED;
			return;
		}
		c->socket = socket;
		c->sink = index;
		c->progress = r->now;
		if (tg_delivery_add_output(r->delivery, &o->delivery, &c->id) !=
			TG_OK) {
			close_socket(&socket);
			free(c);
			continue;
		}
		c->next = r->clients;
		r->clients = c;
	}
}
static void client_writes(tg_runtime *r)
{
	runtime_client **link = &r->clients;
	while (*link) {
		runtime_client *c = *link;
		char incoming;
		int got = (int)recv(c->socket, &incoming, 1, MSG_PEEK);
		bool failed = got >= 0 || !would_block();
		tg_bytes bytes{};
		if (!failed &&
			tg_delivery_peek(r->delivery, c->id, &bytes) == TG_OK) {
			int n = socket_send(c->socket, bytes.data,
				bytes.size > 65536 ? 65536 : bytes.size);
			if (n > 0) {
				tg_delivery_consume(
					r->delivery, c->id, (size_t)n);
				c->progress = r->now;
			} else if (!would_block() ||
				   r->now - c->progress >= 30000)
				failed = true;
		} else if (!bytes.size)
			c->progress = r->now;
		if (failed) {
			*link = c->next;
			close_socket(&c->socket);
			retire_output(r, c->sink, c->id);
			free(c);
		} else
			link = &c->next;
	}
}
static tg_status pipe_poll(tg_runtime *r, size_t index)
{
	runtime_sink *s = &r->sinks[index];
	const tg_output_config *o = &r->config->outputs[index];
#ifndef _WIN32
	if (s->pipe < 0) {
		if (r->now < s->retry)
			return TG_OK;
		struct stat before{}, after{};
		if (lstat(o->path, &before) || !S_ISFIFO(before.st_mode))
			return TG_IO;
		s->pipe = open(o->path,
			O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (s->pipe < 0) {
			if (errno == ENXIO || errno == EINTR) {
				s->retry = r->now + 100;
				return TG_OK;
			}
			return TG_IO;
		}
		if (fstat(s->pipe, &after) || before.st_dev != after.st_dev ||
			before.st_ino != after.st_ino ||
			!S_ISFIFO(after.st_mode))
			return TG_IO;
		if (tg_delivery_add_output(r->delivery, &o->delivery, &s->id) !=
			TG_OK)
			return TG_LIMIT;
		s->phase = 4;
		s->phase_time = r->now;
	}
	tg_bytes bytes{};
	if (tg_delivery_peek(r->delivery, s->id, &bytes) == TG_OK) {
		signal_mask mask = block_pipe();
		ssize_t n = write(s->pipe, bytes.data,
			bytes.size > 65536 ? 65536 : bytes.size);
		int saved = errno;
		restore_pipe(&mask);
		errno = saved;
		if (n > 0) {
			tg_delivery_consume(r->delivery, s->id, (size_t)n);
			s->phase_time = r->now;
		} else if (!would_block() || r->now - s->phase_time >= 30000) {
			sink_disconnect(r, index);
		}
	} else {
		s->phase_time = r->now;
	}
#else
	if (s->pipe == INVALID_HANDLE_VALUE) {
		if (r->now < s->retry)
			return TG_OK;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
			    "D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1,
			    &descriptor, nullptr))
			return TG_IO;
		SECURITY_ATTRIBUTES attributes{
			sizeof(attributes), descriptor, FALSE};
		s->pipe = CreateNamedPipeA(o->path,
			PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
				FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
				PIPE_REJECT_REMOTE_CLIENTS,
			1, 65536, 65536, 0, &attributes);
		LocalFree(descriptor);
		if (s->pipe == INVALID_HANDLE_VALUE)
			return TG_IO;
		s->connect_io.hEvent =
			CreateEvent(nullptr, TRUE, FALSE, nullptr);
		s->write_io.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!s->connect_io.hEvent || !s->write_io.hEvent)
			return TG_IO;
		BOOL connected = ConnectNamedPipe(s->pipe, &s->connect_io);
		DWORD error = connected ? ERROR_SUCCESS : GetLastError();
		if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED &&
			error != ERROR_IO_PENDING)
			return TG_IO;
		s->connecting = error == ERROR_IO_PENDING;
	}
	if (s->connecting) {
		DWORD done;
		if (!GetOverlappedResult(
			    s->pipe, &s->connect_io, &done, FALSE)) {
			if (GetLastError() == ERROR_IO_INCOMPLETE)
				return TG_OK;
			sink_disconnect(r, index);
			return TG_OK;
		}
		s->connecting = false;
	}
	if (!s->id) {
		if (tg_delivery_add_output(r->delivery, &o->delivery, &s->id) !=
			TG_OK)
			return TG_LIMIT;
		s->phase = 4;
	}
	if (s->writing) {
		DWORD done;
		if (!GetOverlappedResult(s->pipe, &s->write_io, &done, FALSE)) {
			if (GetLastError() == ERROR_IO_INCOMPLETE)
				return TG_OK;
			sink_disconnect(r, index);
			return TG_OK;
		}
		tg_delivery_consume(r->delivery, s->id, done);
		s->writing = false;
	}
	tg_bytes bytes{};
	if (tg_delivery_peek(r->delivery, s->id, &bytes) == TG_OK) {
		ResetEvent(s->write_io.hEvent);
		DWORD done;
		if (WriteFile(s->pipe, bytes.data,
			    (DWORD)(bytes.size > 65536 ? 65536 : bytes.size),
			    &done, &s->write_io))
			tg_delivery_consume(r->delivery, s->id, done);
		else if (GetLastError() == ERROR_IO_PENDING)
			s->writing = true;
		else
			sink_disconnect(r, index);
	}
#endif
	return TG_OK;
}
static tg_status sink_poll(tg_runtime *r, size_t index)
{
	runtime_sink *s = &r->sinks[index];
	const tg_output_config *o = &r->config->outputs[index];
	if (same(o->kind, "pipe"))
		return pipe_poll(r, index);
	if (same(o->kind, "tcp") && same(o->mode, "listen")) {
		if (r->state == TG_RUNTIME_RUNNING)
			accept_clients(r, index);
		return TG_OK;
	}
	if (r->now < s->retry)
		return TG_OK;
	bool udp = same(o->kind, "udp");
	if (!s->phase) {
		s->phase = 1;
		s->phase_time = r->now;
		if (!resolve_start(&s->dns, o->host, o->port,
			    udp ? SOCK_DGRAM : SOCK_STREAM)) {
			sink_disconnect(r, index);
			return TG_OK;
		}
	}
	if (s->phase == 1) {
		resolve_tick(&s->dns);
		if (!s->dns.ready)
			return TG_OK;
		if (s->dns.result != ARES_SUCCESS) {
			sink_disconnect(r, index);
			return TG_OK;
		}
		s->socket = socket(s->dns.address.ss_family,
			udp ? SOCK_DGRAM : SOCK_STREAM, 0);
		if (s->socket == bad_socket || !nonblocking(s->socket)) {
			sink_disconnect(r, index);
			return TG_OK;
		}
		int result = connect(
			s->socket, (sockaddr *)&s->dns.address, s->dns.length);
		if (result && !would_block()) {
			sink_disconnect(r, index);
			return TG_OK;
		}
		s->phase = udp ? 4 : 2;
		s->phase_time = r->now;
	}
	if (s->phase == 2) {
		int connected = socket_connected(s->socket);
		if (connected != 1) {
			if (connected < 0 || r->now - s->phase_time >= 10000)
				sink_disconnect(r, index);
			return TG_OK;
		}
		s->phase = 4;
	}
	if (!s->id && tg_delivery_add_output(
			      r->delivery, &o->delivery, &s->id) != TG_OK)
		return TG_LIMIT;
	tg_bytes bytes{};
	if (tg_delivery_peek(r->delivery, s->id, &bytes) == TG_OK) {
		int n = socket_send(s->socket, bytes.data,
			bytes.size > 65536 ? 65536 : bytes.size);
		if (n > 0) {
			tg_delivery_consume(r->delivery, s->id, (size_t)n);
			s->phase_time = r->now;
		} else if (!would_block() || r->now - s->phase_time >= 30000)
			sink_disconnect(r, index);
	} else
		s->phase_time = r->now;
	if (!udp && s->socket != bad_socket) {
		char byte;
		int n = (int)recv(s->socket, &byte, 1, MSG_PEEK);
		if (n >= 0 || !would_block())
			sink_disconnect(r, index);
	}
	return TG_OK;
}
static void close_intake(tg_runtime *r)
{
	for (size_t i = 0; i < r->config->input_count; ++i) {
		if (r->sources[i].dns.channel)
			ares_cancel(r->sources[i].dns.channel);
		source_disconnect(&r->sources[i]);
		r->sources[i].phase = 0;
	}
	for (size_t i = 0; i < r->config->output_count; ++i)
		if (same(r->config->outputs[i].kind, "tcp") &&
			same(r->config->outputs[i].mode, "listen"))
			close_socket(&r->sinks[i].socket);
}
static void stop_outputs(tg_runtime *r)
{
	while (r->clients) {
		runtime_client *c = r->clients;
		r->clients = c->next;
		close_socket(&c->socket);
		retire_output(r, c->sink, c->id);
		free(c);
	}
	for (size_t i = 0; i < r->config->output_count; ++i)
		sink_disconnect(r, i);
}
static tg_status write_status(tg_runtime *r)
{
	if (!r->config->status_file)
		return TG_OK;
	tg_buffer snapshot{};
	tg_status status = tg_runtime_snapshot(r, &snapshot);
	if (status < 0)
		return status;
	tg_vec name{};
	if (!tg_printf(&name, "%s.tmp-%llu", r->config->status_file,
		    (unsigned long long)tg_monotonic_ms())) {
		tg_buffer_release(&snapshot);
		return TG_NO_MEMORY;
	}
#ifdef _WIN32
	HANDLE f = CreateFileA((const char *)name.data, GENERIC_WRITE, 0,
		nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD done = 0;
	bool ok = f != INVALID_HANDLE_VALUE &&
		  WriteFile(f, snapshot.data, (DWORD)snapshot.size, &done,
			  nullptr) &&
		  done == snapshot.size;
	if (f != INVALID_HANDLE_VALUE)
		CloseHandle(f);
	if (ok)
		ok = MoveFileExA((const char *)name.data,
			r->config->status_file,
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (!ok)
		DeleteFileA((const char *)name.data);
#else
	int f = open((const char *)name.data,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	bool ok = f >= 0;
	size_t offset = 0;
	while (ok && offset < snapshot.size) {
		ssize_t n = write(
			f, snapshot.data + offset, snapshot.size - offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			ok = false;
		else
			offset += (size_t)n;
	}
	if (f >= 0 && close(f))
		ok = false;
	if (ok)
		ok = rename((const char *)name.data, r->config->status_file) ==
		     0;
	if (!ok)
		unlink((const char *)name.data);
#endif
	tg_vec_free(&name);
	tg_buffer_release(&snapshot);
	return ok ? TG_OK : TG_IO;
}
tg_status tg_runtime_create(
	const tg_config *config, tg_runtime **out, tg_diagnostic *diagnostic)
{
	/*
	 * Revalidate/load secrets and create a READY runtime using owned
	 * config. Compose public components, opening no network endpoints yet.
	 */
	if (diagnostic)
		*diagnostic = {};
	if (!out)
		return TG_INVALID;
	*out = nullptr;
	if (!config)
		return TG_INVALID;
	tg_status status = tg_config_validate(config, diagnostic);
	if (status < 0)
		return status;
	tg_runtime *r = static_cast<tg_runtime *>(calloc(1, sizeof(*r)));
	if (!r)
		return TG_NO_MEMORY;
	r->now = tg_monotonic_ms();
	r->state = TG_RUNTIME_READY;
	status = tg_config_parse(
		{(const uint8_t *)config->original, strlen(config->original)},
		config->base, &r->config, diagnostic);
	if (status < 0) {
		free(r);
		return status;
	}
	r->sources = static_cast<runtime_source *>(
		calloc(config->input_count, sizeof(*r->sources)));
	r->sinks = static_cast<runtime_sink *>(
		calloc(config->output_count, sizeof(*r->sinks)));
	if (!r->sources || !r->sinks) {
		tg_runtime_destroy(r);
		return TG_NO_MEMORY;
	}
	for (size_t i = 0; i < config->input_count; ++i)
		r->sources[i].socket = bad_socket;
	for (size_t i = 0; i < config->output_count; ++i) {
		r->sinks[i].socket = bad_socket;
#ifdef _WIN32
		r->sinks[i].pipe = INVALID_HANDLE_VALUE;
#else
		r->sinks[i].pipe = -1;
#endif
	}
#ifdef _WIN32
	WSADATA data{};
	if (WSAStartup(MAKEWORD(2, 2), &data)) {
		tg_runtime_destroy(r);
		return TG_IO;
	}
	r->winsock = true;
#endif
	if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS) {
		tg_runtime_destroy(r);
		return TG_IO;
	}
	r->ares_ready = true;
	status = tg_delivery_create(
		&config->limits, config->overflow, &r->delivery);
	for (size_t i = 0; status == TG_OK && i < config->input_count; ++i)
		if (config->inputs[i].enabled &&
			same(config->inputs[i].kind, "tls"))
			status = source_tls_create(
				&r->config->inputs[i], &r->sources[i]);
	if (status < 0) {
		if (diagnostic)
			diagnostic->status = status;
		tg_runtime_destroy(r);
		return status;
	}
	*out = r;
	return TG_OK;
}
static void runtime_wait(tg_runtime *r, uint32_t milliseconds)
{
	if (!milliseconds)
		return;
	size_t capacity =
		r->config->input_count * 17 + r->config->output_count * 17;
	for (runtime_client *c = r->clients; c; c = c->next)
		++capacity;
	if (capacity > SIZE_MAX / sizeof(tg_pollfd))
		return;
	tg_pollfd *fds =
		static_cast<tg_pollfd *>(calloc(capacity, sizeof(*fds)));
	if (!fds)
		return;
	size_t count = 0;
	for (size_t i = 0; i < r->config->input_count; ++i) {
		runtime_source *s = &r->sources[i];
		if (s->socket != bad_socket)
			fds[count++] = {s->socket,
				(short)(POLLIN | (s->phase == 2 || s->phase == 3
								 ? POLLOUT
								 : 0)),
				0};
		for (size_t j = 0; j < s->dns.count; ++j)
			fds[count++] = s->dns.sockets[j];
	}
	for (size_t i = 0; i < r->config->output_count; ++i) {
		runtime_sink *s = &r->sinks[i];
		if (s->socket != bad_socket) {
			bool listen =
				same(r->config->outputs[i].mode, "listen") &&
				same(r->config->outputs[i].kind, "tcp");
			tg_bytes pending{};
			bool write =
				!listen &&
				(s->phase == 2 ||
					(s->id && tg_delivery_peek(r->delivery,
							  s->id,
							  &pending) == TG_OK));
			fds[count++] = {s->socket,
				(short)(POLLIN | (write ? POLLOUT : 0)), 0};
		}
		for (size_t j = 0; j < s->dns.count; ++j)
			fds[count++] = s->dns.sockets[j];
	}
	for (runtime_client *c = r->clients; c && count < capacity;
		c = c->next) {
		tg_bytes pending{};
		bool write =
			tg_delivery_peek(r->delivery, c->id, &pending) == TG_OK;
		fds[count++] = {
			c->socket, (short)(POLLIN | (write ? POLLOUT : 0)), 0};
	}
	int wait = (int)(milliseconds > 20 ? 20 : milliseconds);
	tg_poll(fds, (unsigned long)count, wait);
	free(fds);
}
tg_status tg_runtime_poll(tg_runtime *r, uint32_t max_wait_ms)
{
	/*
	 * Drive bounded nonblocking TLS/UDP/multicast/pipe adapters and
	 * the framing/session/normalization/delivery APIs. Advance reconnects
	 * independently, fair scheduling, snapshots and shutdown deadlines.
	 * Separate transient source failure from fatal local runtime errors.
	 */
	if (!r)
		return TG_INVALID;
	if (r->state == TG_RUNTIME_STOPPED)
		return TG_OK;
	if (r->state == TG_RUNTIME_FAILED)
		return TG_BAD_STATE;
	r->now = tg_monotonic_ms();
	if (!r->initialized && r->state == TG_RUNTIME_READY) {
		tg_status status = initialize(r);
		if (status < 0) {
			r->state = TG_RUNTIME_FAILED;
			close_intake(r);
			stop_outputs(r);
			return status;
		}
	}
	for (unsigned pass = 0; pass < 2; ++pass) {
		if (pass)
			r->now = tg_monotonic_ms();
		for (size_t i = 0; i < r->config->output_count; ++i)
			if (r->config->outputs[i].enabled) {
				tg_status status = sink_poll(r, i);
				if (status < 0) {
					r->state = TG_RUNTIME_FAILED;
					close_intake(r);
					stop_outputs(r);
					return status;
				}
			}
		if (r->state == TG_RUNTIME_RUNNING) {
			for (size_t i = 0; i < r->config->input_count; ++i) {
				if (!r->config->inputs[i].enabled)
					continue;
				tg_status status = TG_OK;
				if (same(r->config->inputs[i].kind, "tls"))
					source_tls_poll(r, i);
				else
					status = udp_poll(r, i);
				if (r->state == TG_RUNTIME_FAILED &&
					status == TG_OK)
					status = TG_IO;
				if (status < 0) {
					r->state = TG_RUNTIME_FAILED;
					close_intake(r);
					stop_outputs(r);
					return status;
				}
			}
		}
		client_writes(r);
		for (size_t i = 0; i < r->config->output_count; ++i)
			if (r->config->outputs[i].enabled &&
				!same(r->config->outputs[i].mode, "listen")) {
				tg_status status = sink_poll(r, i);
				if (status < 0) {
					r->state = TG_RUNTIME_FAILED;
					close_intake(r);
					stop_outputs(r);
					return status;
				}
			}
		/* Pipe outputs have no TCP mode; drive them after publication
		 * too. */
		for (size_t i = 0; i < r->config->output_count; ++i)
			if (r->config->outputs[i].enabled &&
				same(r->config->outputs[i].kind, "pipe")) {
				tg_status status = pipe_poll(r, i);
				if (status < 0) {
					r->state = TG_RUNTIME_FAILED;
					close_intake(r);
					stop_outputs(r);
					return status;
				}
			}
		if (r->state == TG_RUNTIME_DRAINING) {
			tg_delivery_info info{};
			tg_delivery_snapshot(r->delivery, 0, &info);
			if (!info.queued_records ||
				r->now - r->drain_start >= r->drain_ms) {
				stop_outputs(r);
				r->state = TG_RUNTIME_STOPPED;
			}
		}
		if (!pass && max_wait_ms && r->state != TG_RUNTIME_STOPPED)
			runtime_wait(r, max_wait_ms);
		else
			break;
	}
	if (r->state == TG_RUNTIME_FAILED) {
		close_intake(r);
		stop_outputs(r);
		return TG_IO;
	}
	if (!r->status_time || r->now - r->status_time >= 100 ||
		r->state == TG_RUNTIME_STOPPED) {
		tg_status status = write_status(r);
		r->status_time = r->now;
		if (status < 0) {
			r->state = TG_RUNTIME_FAILED;
			close_intake(r);
			stop_outputs(r);
			return status;
		}
	}
	return TG_OK;
}
tg_status tg_runtime_stop(tg_runtime *r, uint32_t drain_ms)
{
	/*
	 * Idempotently stop intake/leave groups/close upstreams and set a
	 * monotonic drain deadline. Subsequent polls finish records or count
	 * loss.
	 */
	if (!r)
		return TG_INVALID;
	if (r->state == TG_RUNTIME_FAILED)
		return TG_BAD_STATE;
	if (r->state == TG_RUNTIME_STOPPED || r->state == TG_RUNTIME_DRAINING)
		return TG_OK;
	r->now = tg_monotonic_ms();
	r->drain_start = r->now;
	r->drain_ms = drain_ms;
	close_intake(r);
	r->state = TG_RUNTIME_DRAINING;
	if (!drain_ms || !r->initialized) {
		stop_outputs(r);
		r->state = TG_RUNTIME_STOPPED;
	}
	return TG_OK;
}
static const char *runtime_state(tg_runtime_state s)
{
	switch (s) {
	case TG_RUNTIME_READY:
		return "READY";
	case TG_RUNTIME_RUNNING:
		return "RUNNING";
	case TG_RUNTIME_DRAINING:
		return "DRAINING";
	case TG_RUNTIME_STOPPED:
		return "STOPPED";
	default:
		return "FAILED";
	}
}
tg_status tg_runtime_snapshot(const tg_runtime *r, tg_buffer *json)
{
	/* Copy documented nonsecret JSON status without remote calls. */
	if (!r || !tg_empty(json))
		return TG_INVALID;
	tg_vec out{};
	bool ok =
		tg_printf(&out, "{\"version\":1,\"state\":\"%s\",\"inputs\":[",
			runtime_state(r->state));
	for (size_t i = 0; i < r->config->input_count; ++i) {
		const runtime_source *s = &r->sources[i];
		const tg_input_config *in = &r->config->inputs[i];
		tg_session_info info{};
		if (s->session)
			tg_session_snapshot(s->session, &info);
		const char *state = "DISCONNECTED";
		if (!in->enabled)
			state = "DISABLED";
		else if (s->phase == 4) {
			if (!same(in->kind, "tls"))
				state = "LISTENING";
			else
				switch (info.state) {
				case TG_SESSION_AUTH:
					state = "AUTH_PREAMBLE";
					break;
				case TG_SESSION_XML:
					state = "XML_ACTIVE";
					break;
				case TG_SESSION_PROTOBUF:
					state = "PROTOBUF_ACTIVE";
					break;
				case TG_SESSION_UPGRADE_PENDING:
					state = "UPGRADE_PENDING";
					break;
				default:
					state = "FAILED";
					break;
				}
		} else if (s->phase == 1 || s->phase == 2)
			state = "CONNECTING";
		else if (s->phase == 3)
			state = "TLS_HANDSHAKE";
		else if (s->retry)
			state = "BACKOFF";
		ok = ok && tg_text(&out, i ? ",{\"id\":\"" : "{\"id\":\"") &&
		     tg_escape(&out, in->id, true) &&
		     tg_printf(&out,
			     "\",\"state\":\"%s\",\"credentials_sent\":%s,"
			     "\"protocol_offer_received\":%s,\"controls_"
			     "consumed\":%llu,\"events_received\":%llu,"
			     "\"reconnects\":%llu,\"accepted\":%llu,"
			     "\"rejected\":%llu,\"failure\":\"%s\"}",
			     state, info.credentials_sent ? "true" : "false",
			     info.protocol_offer_received ? "true" : "false",
			     (unsigned long long)info.controls_consumed,
			     (unsigned long long)info.events_received,
			     (unsigned long long)s->reconnects,
			     (unsigned long long)s->accepted,
			     (unsigned long long)s->rejected,
			     s->failure ? s->failure : "");
	}
	ok = ok && tg_text(&out, "],\"outputs\":[");
	for (size_t i = 0; i < r->config->output_count; ++i) {
		const tg_output_config *o = &r->config->outputs[i];
		const runtime_sink *s = &r->sinks[i];
		tg_delivery_info info{};
		size_t clients = 0;
		if (s->id)
			tg_delivery_snapshot(r->delivery, s->id, &info);
		for (runtime_client *c = r->clients; c; c = c->next)
			if (c->sink == i) {
				++clients;
				tg_delivery_info value{};
				tg_delivery_snapshot(
					r->delivery, c->id, &value);
				info.queued_records += value.queued_records;
				info.queued_bytes += value.queued_bytes;
				info.records_completed +=
					value.records_completed;
				info.records_dropped += value.records_dropped;
			}
		info.records_completed += s->history.records_completed;
		info.records_dropped += s->history.records_dropped;
		info.udp_oversize += s->history.udp_oversize;
		bool available = o->enabled && s->phase == 4 &&
				 r->state != TG_RUNTIME_STOPPED;
		ok = ok && tg_text(&out, i ? ",{\"id\":\"" : "{\"id\":\"") &&
		     tg_escape(&out, o->id, true) &&
		     tg_printf(&out,
			     "\",\"available\":%s,\"clients\":%zu,\"queued_"
			     "records\":%zu,\"queued_bytes\":%zu,\"records_"
			     "completed\":%llu,\"records_dropped\":%llu,\"udp_"
			     "oversize\":%llu}",
			     available ? "true" : "false", clients,
			     info.queued_records, info.queued_bytes,
			     (unsigned long long)info.records_completed,
			     (unsigned long long)info.records_dropped,
			     (unsigned long long)info.udp_oversize);
	}
	ok = ok && tg_text(&out, "]}");
	if (!ok) {
		tg_vec_free(&out);
		return TG_NO_MEMORY;
	}
	*json = {out.data, out.size};
	return TG_OK;
}
void tg_runtime_destroy(tg_runtime *r)
{
	/*
	 * Close all adapters immediately, release state and erase
	 * secrets.
	 */
	if (!r)
		return;
	if (r->config && r->sources && r->sinks) {
		close_intake(r);
		stop_outputs(r);
	}
	if (r->sources && r->config)
		for (size_t i = 0; i < r->config->input_count; ++i) {
			runtime_source *s = &r->sources[i];
			if (s->dns.channel)
				ares_destroy(s->dns.channel);
			if (s->tls)
				SSL_CTX_free(s->tls);
			buffer_secret_free(&s->password);
			buffer_secret_free(&s->key_password);
			buffer_secret_free(&s->identity);
		}
	if (r->sinks && r->config)
		for (size_t i = 0; i < r->config->output_count; ++i)
			if (r->sinks[i].dns.channel)
				ares_destroy(r->sinks[i].dns.channel);
	tg_delivery_destroy(r->delivery);
	free(r->sources);
	free(r->sinks);
	tg_config_destroy(r->config);
	if (r->ares_ready)
		ares_library_cleanup();
#ifdef _WIN32
	if (r->winsock)
		WSACleanup();
#endif
	free(r);
}
