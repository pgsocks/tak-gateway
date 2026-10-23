#include "runtime_fixtures.hpp"
#include <regex>

using namespace contract;
using runtime_fixtures::GatewayFixture;

static bool state(const std::string &json, const char *name)
{
	std::string prefix = "\"state\"[[:space:]]*:[[:space:]]*\"";
	std::regex expression(prefix + name + "\"");

	return std::regex_search(json, expression);
}

TEST(Runtime, CreateCopiesConfigurationAndOpensNoNetworkEndpoints)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	/* Both target ports stay reserved through runtime creation. */
	ASSERT_EQ(fixture.create(), TG_OK);
	tg_config_destroy(fixture.config.value);
	fixture.config.value = nullptr;
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_TRUE(state(string(snapshot.value), "READY"));
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	ASSERT_TRUE(fixture.send(fixtures::extended_xml()));
	ASSERT_TRUE(
		fixture.await_bytes(record(fixtures::extended_xml()).size()));
	EXPECT_EQ(fixture.received, record(fixtures::extended_xml()));
}

TEST(Runtime, UdpToTcpPreservesDuplicateEventsAndUnknownXml)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	std::string xml = fixtures::extended_xml();

	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
	ASSERT_TRUE(fixture.send(xml));
	ASSERT_TRUE(fixture.await_bytes(2 * record(xml).size()));
	EXPECT_EQ(fixture.received, record(xml) + record(xml));
}

TEST(Runtime, MalformedDatagramDoesNotPoisonHealthySource)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	ASSERT_TRUE(fixture.send("<event><broken></event>"));
	ASSERT_TRUE(fixture.send(fixtures::xml()));
	ASSERT_TRUE(fixture.await_bytes(record(fixtures::xml()).size()));
	EXPECT_EQ(fixture.received, record(fixtures::xml()));
}

TEST(Runtime, QuietSourceStillHasNonsecretLocalStatus)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	std::string json = string(snapshot.value);

	EXPECT_TRUE(state(json, "RUNNING"));
	EXPECT_NE(json.find("source"), std::string::npos);
	EXPECT_NE(json.find("LISTENING"), std::string::npos);
	EXPECT_NE(json.find("capture"), std::string::npos);
	EXPECT_EQ(json.find("fixture-1"), std::string::npos);
}

TEST(Runtime, ZeroDeadlineStopsImmediatelyAndRepeatedStopIsHarmless)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_EQ(tg_runtime_stop(fixture.runtime.value, 0), TG_OK);
	EXPECT_EQ(tg_runtime_stop(fixture.runtime.value, 5000), TG_OK);
	EXPECT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_TRUE(state(string(snapshot.value), "STOPPED"));
}

TEST(Runtime, FatalLocalBindFailureIsObservableAndDoesNotMasqueradeAsReconnect)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	/* Keep both reserved descriptors open so initialization cannot bind. */
	EXPECT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_IO);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_TRUE(state(string(snapshot.value), "FAILED"));
	EXPECT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_BAD_STATE);
	EXPECT_EQ(tg_runtime_stop(fixture.runtime.value, 0), TG_BAD_STATE);
}

TEST(Runtime, NonblockingPollDoesNotWaitForQuietSources)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	auto start = std::chrono::steady_clock::now();

	ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	auto elapsed = std::chrono::steady_clock::now() - start;

	/* A generous bound detects accidental connection/partial-read waits. */
	EXPECT_LT(elapsed, std::chrono::seconds(1));
}

#ifndef _WIN32
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>

namespace
{

struct PipeDirectory {
	std::string directory;
	std::string path;
	int reader = -1;

	bool create()
	{
		char pattern[] = "/tmp/tak-gateway-contract-XXXXXX";
		char *created = mkdtemp(pattern);

		if (!created)
			return false;
		directory = created;
		path = directory + "/events.fifo";
		return true;
	}

	~PipeDirectory()
	{
		if (reader >= 0)
			close(reader);
		if (!path.empty())
			unlink(path.c_str());
		if (!directory.empty())
			rmdir(directory.c_str());
	}
};

static void append_pipe(GatewayFixture &fixture, const std::string &path)
{
	fixture.text += "[[outputs]]\nid = \"pipe\"\nkind = \"pipe\"\n"
			"path = '" +
			path + "'\ncreate = false\nframing = \"u32be\"\n";
}

} /* namespace */

TEST(PosixRuntime, FifoMissingReaderDoesNotBlockHealthyTcpDelivery)
{
	GatewayFixture fixture;
	PipeDirectory pipe;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_TRUE(pipe.create());
	ASSERT_EQ(mkfifo(pipe.path.c_str(), 0600), 0);
	append_pipe(fixture, pipe.path);
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	ASSERT_TRUE(fixture.send(fixtures::xml()));
	ASSERT_TRUE(fixture.await_bytes(record(fixtures::xml()).size()));
	EXPECT_EQ(fixture.received, record(fixtures::xml()));
}

TEST(PosixRuntime, FifoReceivesSameExactRecordsAsTcpConsumer)
{
	GatewayFixture fixture;
	PipeDirectory pipe;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_TRUE(pipe.create());
	ASSERT_EQ(mkfifo(pipe.path.c_str(), 0600), 0);
	pipe.reader = open(pipe.path.c_str(), O_RDONLY | O_NONBLOCK);
	ASSERT_GE(pipe.reader, 0);
	append_pipe(fixture, pipe.path);
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	std::string expected = record(fixtures::extended_xml());

	ASSERT_TRUE(fixture.send(fixtures::extended_xml()));
	ASSERT_TRUE(fixture.await_bytes(expected.size()));
	std::string collected;
	auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(2);

	while (collected.size() < expected.size() &&
		std::chrono::steady_clock::now() < deadline) {
		ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 5), TG_OK);
		char buffer[7];
		ssize_t count = read(pipe.reader, buffer, sizeof(buffer));

		if (count > 0)
			collected.append(buffer, static_cast<size_t>(count));
		else if (count < 0) {
			ASSERT_TRUE(runtime_fixtures::would_block());
		}
	}
	EXPECT_EQ(collected, expected);
	EXPECT_EQ(collected, fixture.received);
}

TEST(PosixRuntime, FifoReaderClosureDoesNotTerminateHealthyOutputs)
{
	GatewayFixture fixture;
	PipeDirectory pipe;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_TRUE(pipe.create());
	ASSERT_EQ(mkfifo(pipe.path.c_str(), 0600), 0);
	pipe.reader = open(pipe.path.c_str(), O_RDONLY | O_NONBLOCK);
	ASSERT_GE(pipe.reader, 0);
	append_pipe(fixture, pipe.path);
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	close(pipe.reader);
	pipe.reader = -1;
	ASSERT_TRUE(fixture.send(fixtures::xml()));
	ASSERT_TRUE(fixture.await_bytes(record(fixtures::xml()).size()));
	EXPECT_EQ(fixture.received, record(fixtures::xml()));
}

TEST(PosixRuntime, RefusesRegularFileAndSymlinkAtPipeOutputPath)
{
	for (bool symlink : {false, true}) {
		GatewayFixture fixture;
		PipeDirectory pipe;

		ASSERT_TRUE(fixture.prepare());
		ASSERT_TRUE(pipe.create());
		if (symlink) {
			ASSERT_EQ(::symlink("/dev/null", pipe.path.c_str()), 0);
		} else {
			FILE *file = fopen(pipe.path.c_str(), "wb");

			ASSERT_NE(file, nullptr);
			ASSERT_EQ(fwrite("unchanged", 1, 9, file), 9u);
			ASSERT_EQ(fclose(file), 0);
		}
		append_pipe(fixture, pipe.path);
		tg_status result = fixture.create();

		ASSERT_TRUE(result == TG_OK || result == TG_IO);
		if (result == TG_OK) {
			EXPECT_EQ(fixture.start(), TG_IO);
		}
		struct stat info{};

		ASSERT_EQ(lstat(pipe.path.c_str(), &info), 0);
		EXPECT_TRUE(symlink ? S_ISLNK(info.st_mode)
				    : S_ISREG(info.st_mode));
		if (!symlink) {
			FILE *file = fopen(pipe.path.c_str(), "rb");
			char content[10] = {};

			ASSERT_NE(file, nullptr);
			EXPECT_EQ(fread(content, 1, 9, file), 9u);
			EXPECT_EQ(fclose(file), 0);
			EXPECT_STREQ(content, "unchanged");
		}
	}
}
#endif

#if defined(__linux__)
#include <arpa/inet.h>

TEST(LinuxRuntime, PassiveMulticastUsesExplicitInterface)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	size_t begin = fixture.text.find("kind = \"udp\"");
	size_t end = fixture.text.find("[[outputs]]");

	fixture.text.replace(begin, end - begin,
		"kind = \"multicast\"\ngroup = \"239.255.42.42\"\n"
		"interface = \"lo\"\nport = " +
			std::to_string(fixture.input_port) +
			"\nencoding = \"auto\"\n");
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	ASSERT_TRUE(fixture.sender.create(SOCK_DGRAM));
	in_addr local{};

	local.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(setsockopt(fixture.sender.value, IPPROTO_IP, IP_MULTICAST_IF,
			  &local, sizeof(local)),
		0);
	unsigned char enabled = 1;

	ASSERT_EQ(setsockopt(fixture.sender.value, IPPROTO_IP,
			  IP_MULTICAST_LOOP, &enabled, sizeof(enabled)),
		0);
	sockaddr_in group = runtime_fixtures::loopback(fixture.input_port);

	ASSERT_EQ(inet_pton(AF_INET, "239.255.42.42", &group.sin_addr), 1);
	std::string xml = fixtures::extended_xml();

	ASSERT_EQ(sendto(fixture.sender.value, xml.data(), xml.size(), 0,
			  reinterpret_cast<sockaddr *>(&group), sizeof(group)),
		static_cast<ssize_t>(xml.size()));
	ASSERT_TRUE(fixture.await_bytes(record(xml).size()));
	EXPECT_EQ(fixture.received, record(xml));
}
#endif

TEST(Runtime, StopBeforeFirstPollNeverOpensEndpoints)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(tg_runtime_stop(fixture.runtime.value, 1000), TG_OK);
	ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 0), TG_OK);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_TRUE(state(string(snapshot.value), "STOPPED"));
}

TEST(Runtime, CompletionTotalsSurviveConsumerDisconnect)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	ASSERT_TRUE(fixture.connect_consumer());
	ASSERT_TRUE(fixture.send(fixtures::xml()));
	ASSERT_TRUE(fixture.await_bytes(record(fixtures::xml()).size()));
	fixture.consumer.close();
	for (unsigned i = 0; i < 5; ++i)
		ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 5), TG_OK);
	Buffer snapshot;

	ASSERT_EQ(tg_runtime_snapshot(fixture.runtime.value, &snapshot.value),
		TG_OK);
	EXPECT_NE(string(snapshot.value).find("\"records_completed\":1"),
		std::string::npos);
	EXPECT_NE(string(snapshot.value).find("\"clients\":0"),
		std::string::npos);
}

TEST(Runtime, RefusedTcpOutputStaysUnavailableAndReconnects)
{
	GatewayFixture fixture;

	ASSERT_TRUE(fixture.prepare());
	size_t output = fixture.text.find("[[outputs]]");
	std::string prefix = fixture.text.substr(0, output);

	fixture.text = prefix +
		       "[[outputs]]\nid='capture'\nkind='tcp'\n"
		       "mode='connect'\nhost='127.0.0.1'\nport=" +
		       std::to_string(fixture.output_port) + "\n";
	ASSERT_EQ(fixture.create(), TG_OK);
	ASSERT_EQ(fixture.start(), TG_OK);
	for (unsigned i = 0; i < 5; ++i) {
		ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 5), TG_OK);
		Buffer snapshot;

		ASSERT_EQ(tg_runtime_snapshot(
				  fixture.runtime.value, &snapshot.value),
			TG_OK);
		EXPECT_NE(string(snapshot.value).find("\"available\":false"),
			std::string::npos);
	}
	runtime_fixtures::Socket listener;

	ASSERT_TRUE(listener.create(SOCK_STREAM));
	sockaddr_in address = runtime_fixtures::loopback(fixture.output_port);

	ASSERT_EQ(bind(listener.value, reinterpret_cast<sockaddr *>(&address),
			  sizeof(address)),
		0);
	ASSERT_EQ(listen(listener.value, 1), 0);
	ASSERT_TRUE(listener.nonblocking());
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);

	while (std::chrono::steady_clock::now() < end) {
		ASSERT_EQ(tg_runtime_poll(fixture.runtime.value, 10), TG_OK);
		fixture.consumer.value =
			accept(listener.value, nullptr, nullptr);
		if (fixture.consumer.value != runtime_fixtures::invalid_socket)
			break;
	}
	ASSERT_NE(fixture.consumer.value, runtime_fixtures::invalid_socket);
	ASSERT_TRUE(fixture.consumer.nonblocking());
	ASSERT_TRUE(fixture.send(fixtures::xml()));
	ASSERT_TRUE(fixture.await_bytes(record(fixtures::xml()).size()));
	EXPECT_EQ(fixture.received, record(fixtures::xml()));
}
