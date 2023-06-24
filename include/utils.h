#ifndef _ASIO_UTIL_H_
#define _ASIO_UTIL_H_

#include <cstdlib>
#include <iostream>
#include <ranges>
#include <thread>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_simple/Executor.h>
#include <boost/asio.hpp>
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

namespace detail {

template <typename C>
struct to_helper {};

template <typename Container, std::ranges::range R>
	requires std::convertible_to<std::ranges::range_value_t<R>, typename Container::value_type>
Container operator|(R&& r, to_helper<Container>) {
	return Container{r.begin(), r.end()};
}

} // namespace detail

template <std::ranges::range Container>
	requires(!std::ranges::view<Container>)
inline auto to() {
	return detail::to_helper<Container>{};
}

inline std::tuple<std::string, std::string> split(const std::string& endpoint) {
	auto ip_port =
		endpoint |
		std::views::split(':') |
		std::views::transform([](auto&& rng) {
			return std::string(&*rng.begin(), std::ranges::distance(rng.begin(), rng.end()));
		}) |
		to<std::vector<std::string>>();
	return std::make_tuple(std::move(ip_port.at(0)), std::move(ip_port.at(1)));
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

template <typename T>
	requires(!std::is_reference<T>::value)
struct AsioCallbackAwaiter {
public:
	using CallbackFunction =
		std::function<void(std::coroutine_handle<>, std::function<void(T)>)>;

	AsioCallbackAwaiter(CallbackFunction callback_function)
		: callback_function_(std::move(callback_function)) {}

	bool await_ready() noexcept { return false; }

	void await_suspend(std::coroutine_handle<> handle) {
		callback_function_(handle, [this](T t) { result_ = std::move(t); });
	}

	auto coAwait(async_simple::Executor*) noexcept {
		return std::move(*this);
	}

	T await_resume() noexcept { return std::move(result_); }

private:
	CallbackFunction callback_function_;
	T result_;
};

inline async_simple::coro::Lazy<boost::system::error_code> async_accept(
	boost::asio::ip::tcp::acceptor& acceptor, boost::asio::ip::tcp::socket& socket) noexcept {
	co_return co_await AsioCallbackAwaiter<boost::system::error_code>{
		[&](std::coroutine_handle<> handle, auto set_resume_value) {
			acceptor.async_accept(socket, [handle, set_resume_value = std::move(set_resume_value)](auto ec) mutable {
				set_resume_value(std::move(ec));
				handle.resume();
			});
		}};
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, size_t>> async_read_some(Socket& socket, AsioBuffer&& buffer) noexcept
	requires std::is_rvalue_reference<decltype(buffer)>::value
{
	co_return co_await AsioCallbackAwaiter<std::pair<boost::system::error_code, size_t>>{
		[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
			socket.async_read_some(std::move(buffer),
				[handle, set_resume_value = std::move(set_resume_value)](
					auto ec, auto size) mutable {
					set_resume_value(std::make_pair(std::move(ec), size));
					handle.resume();
				});
		}};
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, size_t>> async_write(Socket& socket, AsioBuffer&& buffer) noexcept
	requires std::is_rvalue_reference<decltype(buffer)>::value
{
	co_return co_await AsioCallbackAwaiter<std::pair<boost::system::error_code, size_t>>{
		[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
			auto func = [set_resume_value, handle](auto ec, auto size) mutable {
				set_resume_value(std::make_pair(std::move(ec), size));
				handle.resume();
			};
			if constexpr (std::is_same_v<AsioBuffer, boost::beast::http::response<boost::beast::http::string_body>>) {
				boost::beast::http::async_write(socket, std::move(buffer), func);
			}
			else {
				boost::asio::async_write(socket, std::move(buffer), func);
			}
		}};
}

template <typename SocketStream>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, boost::asio::ip::tcp::resolver::results_type::endpoint_type>> async_connect(boost::asio::io_context& io_context, SocketStream& socket,
	boost::asio::ip::tcp::resolver::results_type& results_type, int32_t timeout = 5000) noexcept {
	boost::asio::steady_timer steady_timer{io_context};
	co_return co_await AsioCallbackAwaiter<std::pair<boost::system::error_code, boost::asio::ip::tcp::resolver::results_type::endpoint_type>>{
		[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
			if constexpr (std::is_same_v<SocketStream, boost::asio::ip::tcp::socket>) {
				auto done = std::make_shared<bool>(false);
				steady_timer.expires_after(std::chrono::milliseconds(timeout));
				steady_timer.async_wait([set_resume_value, handle, done](const boost::system::error_code&) {
					if (*done)
						return;
					*done = true;
					set_resume_value(std::make_pair(boost::asio::error::timed_out, boost::asio::ip::tcp::resolver::results_type::endpoint_type{}));
					handle.resume();
				});
				boost::asio::async_connect(socket, results_type,
					[set_resume_value, handle, done](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type ep) mutable {
						if (*done)
							return;
						*done = true;
						set_resume_value(std::make_pair(std::move(ec), std::move(ep)));

						handle.resume();
					});
			}
			else {
				boost::beast::get_lowest_layer(socket).async_connect(results_type,
					[set_resume_value, handle](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type ep) mutable {
						set_resume_value(std::make_pair(std::move(ec), std::move(ep)));
						handle.resume();
					});
			}
		}};
}

template <typename Ws, typename HandshakeType>
inline async_simple::coro::Lazy<boost::system::error_code> async_ssl_handshake(Ws& ws, HandshakeType handshake_type) noexcept {
	co_return co_await AsioCallbackAwaiter<boost::system::error_code>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		ws.async_handshake(handshake_type, [set_resume_value = std::move(set_resume_value), handle](boost::beast::error_code ec) {
			set_resume_value(std::move(ec));
			handle.resume();
		});
	}};
}

template <typename Ws>
inline async_simple::coro::Lazy<boost::system::error_code> async_ws_handshake(Ws& ws, std::string& host, std::string& target) noexcept {
	co_return co_await AsioCallbackAwaiter<boost::system::error_code>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		ws.async_handshake(host, target, [set_resume_value = std::move(set_resume_value), handle](boost::beast::error_code ec) {
			set_resume_value(std::move(ec));
			handle.resume();
		});
	}};
}

inline async_simple::coro::Lazy<std::tuple<boost::system::error_code, boost::asio::ip::tcp::resolver::results_type>> async_resolve(
	boost::asio::ip::tcp::resolver& resolver, std::string& server_host, std::string& server_port) {
	co_return co_await AsioCallbackAwaiter<std::tuple<boost::system::error_code, boost::asio::ip::tcp::resolver::results_type>>{
		[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
			resolver.async_resolve(server_host.c_str(), server_port.c_str(),
				[handle, set_resume_value = std::move(set_resume_value)](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type results) {
					set_resume_value(std::make_tuple(std::move(ec), std::move(results)));
					handle.resume();
				});
		}};
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::system::error_code, size_t>> async_write_ws(Socket& socket, AsioBuffer&& buffer) noexcept
	requires std::is_rvalue_reference<decltype(buffer)>::value
{
	co_return co_await AsioCallbackAwaiter<std::pair<boost::system::error_code, size_t>>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		socket.async_write(std::move(buffer), [set_resume_value = std::move(set_resume_value), handle](boost::beast::error_code ec, std::size_t size) {
			set_resume_value(std::make_pair(std::move(ec), size));
			handle.resume();
		});
	}};
}

template <typename Socket, typename AsioBuffer>
inline async_simple::coro::Lazy<std::pair<boost::beast::error_code, size_t>> async_read_ws(Socket& socket, AsioBuffer& buffer) noexcept {
	co_return co_await AsioCallbackAwaiter<std::pair<boost::system::error_code, size_t>>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		socket.async_read(buffer, [set_resume_value = std::move(set_resume_value), handle](auto ec, auto size) mutable {
			set_resume_value(std::make_pair(std::move(ec), size));
			handle.resume();
		});
	}};
}

template <typename Stream>
inline async_simple::coro::Lazy<boost::system::error_code> async_close_ws(Stream& stream) noexcept {
	co_return co_await AsioCallbackAwaiter<boost::system::error_code>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		stream.async_close(boost::beast::websocket::close_code::normal, [set_resume_value = std::move(set_resume_value), handle](boost::beast::error_code ec) {
			set_resume_value(std::move(ec));
			handle.resume();
		});
	}};
}

template <typename Body, typename Allocator>
inline async_simple::coro::Lazy<boost::beast::error_code> async_accept_ws(
	boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>& ws_stream,
	boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>& req) noexcept {
	co_return co_await AsioCallbackAwaiter<boost::beast::error_code>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		ws_stream.async_accept(req, [set_resume_value = std::move(set_resume_value), handle](boost::beast::error_code ec) {
			set_resume_value(std::move(ec));
			handle.resume();
		});
	}};
}

template <typename Socket, typename AsioBuffer, typename Parser>
inline async_simple::coro::Lazy<std::pair<boost::beast::error_code, size_t>> async_read_http(Socket& socket, AsioBuffer& buffer, Parser& parser) noexcept {
	co_return co_await AsioCallbackAwaiter<std::pair<boost::beast::error_code, size_t>>{[&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
		boost::beast::http::async_read(socket, buffer, parser, [set_resume_value = std::move(set_resume_value), handle](boost::beast::error_code ec, std::size_t bytes_transferred) {
			set_resume_value(std::make_pair(std::move(ec), bytes_transferred));
			handle.resume();
		});
	}};
}

#endif //