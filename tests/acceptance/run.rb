require 'rbconfig'

ROOT = File.expand_path('../..', __dir__)
STARTUP_TIMEOUT = 10
STOP_TIMEOUT = 2
WINDOWS = RbConfig::CONFIG['host_os'].match?(/mswin|mingw/)

def monotonic_time
	Process.clock_gettime(Process::CLOCK_MONOTONIC)
end

def spawn_child(*command, **options)
	group = WINDOWS ? { new_pgroup: true } : { pgroup: true }
	if WINDOWS
		wrapper = File.join(__dir__, 'windows_child.rb')
		command = [RbConfig.ruby, wrapper, *command]
	end
	Process.spawn(*command, **group, **options)
end

def child_status(pid)
	exit = Process.waitpid2(pid, Process::WNOHANG)
	exit[1] if exit
rescue Errno::ECHILD
	nil
end

def stop_child(pid, status = nil)
	status ||= child_status(pid)
	cleanup_signals = []
	target = WINDOWS ? pid : -pid
	signal = WINDOWS ? 'INT' : 'TERM'
	unless WINDOWS && status
		begin
			Process.kill(signal, target)
			cleanup_signals << Signal.list[signal] unless status
		rescue Errno::ESRCH
		rescue Errno::EINVAL, Errno::EPERM
			# Windows console events may be unavailable in CI.
			raise unless WINDOWS
		end
	end
	deadline = monotonic_time + STOP_TIMEOUT
	until status
		status = child_status(pid)
		break if status || monotonic_time >= deadline
		sleep 0.05
	end
	# Also stop descendants if their group leader has already exited.
	# Windows descendants are stopped when the wrapper's job closes.
	unless WINDOWS && status
		begin
			Process.kill('KILL', target)
			cleanup_signals << Signal.list['KILL'] unless status
		rescue Errno::ESRCH
		end
	end
	begin
		status ||= Process.waitpid2(pid)[1]
	rescue Errno::ECHILD
	end
	# Ignore only signals sent by cleanup, retaining ordinary exit failures.
	cleanup_signals.each do |number|
		return nil if status && status.termsig == number
		if WINDOWS && status && status.exitstatus == 128 + number
			return nil
		end
	end
	status
end

def wait_for_port(output)
	deadline = monotonic_time + STARTUP_TIMEOUT
	buffer = +''
	loop do
		remaining = deadline - monotonic_time
		ready = remaining.positive? && IO.select([output], nil, nil,
			remaining)
		unless ready
			raise 'Wire server startup timed out after 10 seconds.'
		end
		chunk = output.read_nonblock(4096, exception: false)
		if chunk.nil?
			raise 'Wire server exited before reporting readiness.'
		end
		next if chunk == :wait_readable
		STDERR.write(chunk)
		buffer << chunk
		while (newline = buffer.index("\n"))
			line = buffer.slice!(0, newline + 1).strip
			pattern = /\AListening on 127\.0\.0\.1:(\d+)\z/
			match = pattern.match(line)
			next unless match
			port = Integer(match[1])
			return port if (1..65_535).cover?(port)
		end
		if buffer.bytesize > 65_536
			raise 'Wire server readiness output is too long.'
		end
	end
end

server = ARGV.shift
unless server
	abort 'Usage: ruby tests/acceptance/run.rb SERVER [CUCUMBER_ARGUMENTS]'
end
server = File.expand_path(server)
unless File.file?(server) && File.executable?(server)
	abort "Native runner unavailable: #{server}. " \
		'Build acceptance_steps first.'
end

ENV['BUNDLE_GEMFILE'] = File.join(ROOT, 'Gemfile')
ENV['BUNDLE_FROZEN'] = 'true'
begin
	require 'bundler/setup'
	cucumber = Gem.bin_path('cucumber', 'cucumber', '7.1.0')
rescue LoadError, StandardError => error
	abort "Acceptance gems unavailable: #{error.message}\n" \
		"Install Bundler and run 'bundle install' in #{ROOT}."
end

children = {}
output = writer = log_thread = nil
result = 1
%w[INT TERM].each do |signal|
	Signal.trap(signal) { raise SignalException, signal }
end

begin
	output, writer = IO.pipe
	server_arguments = ['--listen', '127.0.0.1', '--port', '0', '--verbose']
	server_pid = spawn_child(server, *server_arguments,
		out: writer, err: writer)
	children[server_pid] = nil
	writer.close
	ENV['CUCUMBER_WIRE_PORT'] = wait_for_port(output).to_s
	log_thread = Thread.new do
		begin
			loop { STDERR.write(output.readpartial(4096)) }
		rescue EOFError, IOError
		end
	end

	features = File.join(ROOT, 'features')
	arguments = ARGV.empty? ? [features] : ARGV
	cucumber_pid = spawn_child(RbConfig.ruby, cucumber, '--require',
		features, *arguments, '--strict', chdir: ROOT)
	children[cucumber_pid] = nil
	loop do
		children[server_pid] ||= child_status(server_pid)
		server_status = children[server_pid]
		if server_status && !server_status.success?
			raise "Wire server failed: #{server_status}."
		end
		cucumber_exit = Process.waitpid2(cucumber_pid, Process::WNOHANG)
		if cucumber_exit
			status = children[cucumber_pid] = cucumber_exit[1]
			result = status.exitstatus || 128 + status.termsig
			break
		end
		sleep 0.05
	end
rescue SignalException => error
	warn "Acceptance run interrupted by #{error.signm}."
	result = 128 + error.signo
rescue StandardError => error
	warn "Acceptance runner failed: #{error.message}"
ensure
	%w[INT TERM].each { |signal| Signal.trap(signal, 'IGNORE') }
	children.reverse_each do |pid, status|
		status = stop_child(pid, status)
		next unless pid == server_pid && status && !status.success?
		next unless result.zero?
		warn "Acceptance runner failed: Wire server failed: #{status}."
		result = 1
	end
	writer.close if writer && !writer.closed?
	output.close if output && !output.closed?
	log_thread&.join
end

exit result
