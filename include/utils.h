#ifndef _ASIO_UTIL_H_
#define _ASIO_UTIL_H_

#include <cstdlib>
#include <iostream>
#include <ranges>
#include <thread>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_simple/executors/SimpleExecutor.h>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

class IoContextPool final {
public:
	explicit IoContextPool(std::size_t);

	void start();
	void stop();

	boost::asio::io_context& getIoContext();

private:
	std::vector<std::shared_ptr<boost::asio::io_context>> m_io_contexts;
	std::list<boost::asio::any_io_executor> m_work;
	std::size_t m_next_io_context;
	std::vector<std::jthread> m_threads;
};

inline IoContextPool::IoContextPool(std::size_t pool_size)
	: m_next_io_context(0) {
	if (pool_size == 0)
		throw std::runtime_error("IoContextPool size is 0");
	for (std::size_t i = 0; i < pool_size; ++i) {
		auto io_context_ptr = std::make_shared<boost::asio::io_context>();
		m_io_contexts.emplace_back(io_context_ptr);
		m_work.emplace_back(boost::asio::require(io_context_ptr->get_executor(), boost::asio::execution::outstanding_work.tracked));
	}
}

inline void IoContextPool::start() {
	for (auto& context : m_io_contexts)
		m_threads.emplace_back(std::jthread([&] { context->run(); }));
}

inline void IoContextPool::stop() {
	for (auto& context_ptr : m_io_contexts)
		context_ptr->stop();
}

inline boost::asio::io_context& IoContextPool::getIoContext() {
	boost::asio::io_context& io_context = *m_io_contexts[m_next_io_context];
	++m_next_io_context;
	if (m_next_io_context == m_io_contexts.size())
		m_next_io_context = 0;
	return io_context;
}

inline std::tuple<std::string, std::string> split(const std::string& endpoint) {
	auto to_vector = [](auto&& r) {
		auto r_common = r | std::views::common;
		return std::vector(r_common.begin(), r_common.end());
	};
	auto ip_port = to_vector(endpoint | std::views::split(':') | std::views::transform([](auto&& rng) {
		return std::string_view(&*rng.begin(), std::ranges::distance(rng.begin(), rng.end()));
	}));
	return std::make_tuple(std::string{ip_port.at(0)}, std::string{ip_port.at(1)});
}

class AsioExecutor : public async_simple::Executor {
public:
	AsioExecutor(boost::asio::io_context& io_context)
		: m_io_context(io_context) {
	}

	virtual bool schedule(Func func) override {
		boost::asio::post(m_io_context, std::move(func));
		return true;
	}

	boost::asio::io_context& m_io_context;
};

class AcceptorAwaiter {
public:
	AcceptorAwaiter(boost::asio::ip::tcp::acceptor& acceptor,
		boost::asio::ip::tcp::socket& socket)
		: m_acceptor(acceptor)
		, m_socket(socket) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_acceptor.async_accept(m_socket, [this, handle](auto ec) mutable {
			m_ec = ec;
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }
	auto coAwait(async_simple::Executor*) noexcept {
		return std::move(*this);
	}

private:
	boost::asio::ip::tcp::acceptor& m_acceptor;
	boost::asio::ip::tcp::socket& m_socket;
	boost::system::error_code m_ec{};
};

inline async_simple::coro::Lazy<boost::system::error_code> async_accept(
	boost::asio::ip::tcp::acceptor& acceptor, boost::asio::ip::tcp::socket& socket) noexcept {
	co_return co_await AcceptorAwaiter{acceptor, socket};
}

template <typename Socket, typename AsioBuffer>
struct ReadSomeAwaiter {
public:
	ReadSomeAwaiter(Socket& socket, AsioBuffer&& buffer)
		: m_socket(socket)
		, m_buffer(std::move(buffer)) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, m_size); }
	void await_suspend(std::coroutine_handle<> handle) {
		m_socket.async_read_some(m_buffer, [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			m_size = size;
			handle.resume();
		});
	}
	auto coAwait(async_simple::Executor*) noexcept {
		return std::move(*this);
	}

private:
	Socket& m_socket;
	AsioBuffer m_buffer;

	boost::system::error_code m_ec{};
	size_t m_size{0};
};

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, size_t>> async_read_some(Socket& socket, AsioBuffer&& buffer) noexcept {
	static_assert(std::is_rvalue_reference<decltype(buffer)>::value, "async_read_some AsioBuffer is not rvalue");
	co_return co_await ReadSomeAwaiter{socket, std::move(buffer)};
}

template <typename Socket, typename AsioBuffer>
struct WriteAwaiter {
public:
	WriteAwaiter(Socket& socket, AsioBuffer&& buffer)
		: m_socket(socket)
		, m_buffer(std::move(buffer)) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, m_size); }
	void await_suspend(std::coroutine_handle<> handle) {
		auto func = [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			m_size = size;
			handle.resume();
		};
		if constexpr (std::is_same_v<AsioBuffer, boost::beast::http::response<boost::beast::http::string_body>>) {
			boost::beast::http::async_write(m_socket, m_buffer, func);
		}
		else {
			boost::asio::async_write(m_socket, std::move(m_buffer), func);
		}
	}
	auto coAwait(async_simple::Executor*) noexcept {
		return std::move(*this);
	}

private:
	Socket& m_socket;
	AsioBuffer m_buffer;

	boost::system::error_code m_ec{};
	size_t m_size{0};
};

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, size_t>> async_write(Socket& socket, AsioBuffer&& buffer) noexcept {
	static_assert(std::is_rvalue_reference<decltype(buffer)>::value, "async_write AsioBuffer is not rvalue");
	co_return co_await WriteAwaiter{socket, std::move(buffer)};
}

template <typename SocketStream>
class ConnectAwaiter {
public:
	ConnectAwaiter(boost::asio::io_context& io_context, SocketStream& socket,
		boost::asio::ip::tcp::resolver::results_type& results_type, int32_t timeout)
		: io_context_(io_context)
		, m_socket(socket)
		, m_results_type(results_type)
		, m_steady_timer(io_context)
		, m_timeout(timeout) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		if constexpr (std::is_same_v<SocketStream, boost::asio::ip::tcp::socket>) {
			auto done = std::make_shared<bool>(false);
			m_steady_timer.expires_after(std::chrono::milliseconds(m_timeout));
			m_steady_timer.async_wait([this, handle, done](const boost::system::error_code&) {
				if (*done)
					return;
				*done = true;
				m_ec = boost::asio::error::timed_out;
				handle.resume();
			});
			boost::asio::async_connect(m_socket, m_results_type,
				[this, handle, done](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type ep) mutable {
					if (*done)
						return;
					*done = true;
					m_ec = ec;
					m_ep = std::move(ep);
					handle.resume();
				});
		}
		else {
			boost::beast::get_lowest_layer(m_socket).async_connect(m_results_type,
				[this, handle](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type ep) mutable {
					m_ec = ec;
					m_ep = std::move(ep);
					handle.resume();
				});
		}
	}
	auto await_resume() noexcept { return std::make_pair(m_ec, m_ep); }

private:
	boost::asio::io_context& io_context_;
	SocketStream& m_socket;
	boost::asio::ip::tcp::resolver::results_type& m_results_type;
	boost::asio::steady_timer m_steady_timer;
	int32_t m_timeout;
	boost::system::error_code m_ec{};
	boost::asio::ip::tcp::resolver::results_type::endpoint_type m_ep;
};

template <typename SocketStream>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, boost::asio::ip::tcp::resolver::results_type::endpoint_type>> async_connect(boost::asio::io_context& io_context, SocketStream& socket,
	boost::asio::ip::tcp::resolver::results_type& results_type, int32_t timeout = 5000) noexcept {
	co_return co_await ConnectAwaiter{io_context, socket, results_type, timeout};
}

template <typename Ws, typename HandshakeType>
class SslHandshakeAwaiter {
public:
	SslHandshakeAwaiter(Ws& ws, HandshakeType handshake_type)
		: m_ws(ws)
		, m_handshake_type(handshake_type) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_ws.async_handshake(m_handshake_type, [this, handle](boost::beast::error_code ec) {
			m_ec = ec;
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	Ws& m_ws;
	HandshakeType m_handshake_type;
	boost::beast::error_code m_ec{};
};

template <typename Ws, typename HandshakeType>
inline async_simple::coro::Lazy<boost::system::error_code> async_ssl_handshake(Ws& ws, HandshakeType handshake_type) noexcept {
	co_return co_await SslHandshakeAwaiter{ws, handshake_type};
}

template <typename Ws>
class HandshakeAwaiter {
public:
	HandshakeAwaiter(Ws& ws, std::string& host, std::string& target)
		: m_ws(ws)
		, m_host(host)
		, m_target(target) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_ws.async_handshake(m_host, m_target, [this, handle](boost::beast::error_code ec) {
			m_ec = ec;
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	Ws& m_ws;
	std::string& m_host;
	std::string& m_target;
	boost::beast::error_code m_ec{};
};

template <typename Ws>
inline async_simple::coro::Lazy<boost::system::error_code> async_ws_handshake(Ws& ws, std::string& host, std::string& target) noexcept {
	co_return co_await HandshakeAwaiter{ws, host, target};
}

class ResolveAwaiter {
public:
	ResolveAwaiter(boost::asio::ip::tcp::resolver& resolver, std::string& server_host, std::string& server_port)
		: m_resolver(resolver)
		, m_server_host(server_host)
		, m_server_port(server_port) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_resolver.async_resolve(m_server_host.c_str(), m_server_port.c_str(),
			[this, handle](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type results) {
				m_results_type = std::move(results);
				m_ec = ec;
				handle.resume();
			});
	}
	auto await_resume() noexcept { return std::make_tuple(m_ec, std::move(m_results_type)); }

private:
	boost::asio::ip::tcp::resolver& m_resolver;
	std::string& m_server_host;
	std::string& m_server_port;
	boost::system::error_code m_ec{};
	boost::asio::ip::tcp::resolver::results_type m_results_type;
};

inline async_simple::coro::Lazy<std::tuple<boost::system::error_code, boost::asio::ip::tcp::resolver::results_type>> async_resolve(
	boost::asio::ip::tcp::resolver& resolver, std::string& server_host, std::string& server_port) {
	co_return co_await ResolveAwaiter{resolver, server_host, server_port};
}

template <typename Socket, typename AsioBuffer>
struct WriteAwaiterWs {
public:
	WriteAwaiterWs(Socket& socket, AsioBuffer&& buffer)
		: m_socket(socket)
		, m_buffer(std::move(buffer)) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, m_size); }
	void await_suspend(std::coroutine_handle<> handle) {
		m_socket.async_write(m_buffer, [this, handle](boost::beast::error_code ec, std::size_t size) {
			m_ec = ec;
			m_size = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer m_buffer;

	boost::system::error_code m_ec{};
	size_t m_size{0};
};

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, size_t>> async_write_ws(Socket& socket, AsioBuffer&& buffer) noexcept {
	static_assert(std::is_rvalue_reference<decltype(buffer)>::value, "async_write_ws AsioBuffer is not rvalue");
	co_return co_await WriteAwaiterWs{socket, std::move(buffer)};
}

template <typename Socket, typename AsioBuffer>
struct ReadAwaiterWs {
public:
	ReadAwaiterWs(Socket& socket, AsioBuffer& buffer)
		: m_socket(socket)
		, m_buffer(buffer) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, m_size); }
	void await_suspend(std::coroutine_handle<> handle) {
		m_socket.async_read(m_buffer, [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			m_size = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer& m_buffer;

	boost::system::error_code m_ec{};
	size_t m_size{0};
};

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::beast::error_code, size_t>> async_read_ws(Socket& socket, AsioBuffer& buffer) noexcept {
	co_return co_await ReadAwaiterWs{socket, buffer};
}

template <typename Stream>
class CloseWsClientAwaiter {
public:
	CloseWsClientAwaiter(Stream& stream)
		: m_stream(stream) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_stream.async_close(boost::beast::websocket::close_code::normal, [this, handle](boost::beast::error_code ec) {
			m_ec = ec;
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	Stream& m_stream;
	boost::system::error_code m_ec{};
};

template <typename Stream>
inline async_simple::coro::Lazy<boost::system::error_code> async_close_ws(Stream& stream) noexcept {
	co_return co_await CloseWsClientAwaiter{stream};
}

template <typename Body, typename Allocator>
class AcceptorAwaiterWs {
public:
	AcceptorAwaiterWs(boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>& stream,
		boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>& req)
		: m_ws_stream(stream)
		, m_req(req) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_ws_stream.async_accept(m_req, [this, handle](boost::beast::error_code ec) {
			m_ec = ec;
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>& m_ws_stream;
	boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>& m_req;
	boost::beast::error_code m_ec{};
};

template <typename Body, typename Allocator>
inline async_simple::coro::Lazy<boost::beast::error_code> async_accept_ws(
	boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>& ws_stream,
	boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>& req) noexcept {
	co_return co_await AcceptorAwaiterWs{ws_stream, req};
}

template <typename Socket, typename AsioBuffer, typename Parser>
struct ReadAwaiterHttp {
public:
	ReadAwaiterHttp(Socket& socket, AsioBuffer& buffer, Parser& parser)
		: m_socket(socket)
		, m_buffer(buffer)
		, m_parser(parser) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, m_size); }
	void await_suspend(std::coroutine_handle<> handle) {
		boost::beast::http::async_read(m_socket, m_buffer, m_parser, [this, handle](boost::beast::error_code ec, std::size_t bytes_transferred) {
			m_ec = ec;
			m_size = bytes_transferred;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer& m_buffer;
	Parser& m_parser;

	boost::system::error_code m_ec{};
	size_t m_size{0};
};

template <typename Socket, typename AsioBuffer, typename Parser>
inline async_simple::coro::Lazy<std::pair<boost::beast::error_code, size_t>> async_read_http(Socket& socket, AsioBuffer& buffer, Parser& parser) noexcept {
	co_return co_await ReadAwaiterHttp{socket, buffer, parser};
}

#endif //