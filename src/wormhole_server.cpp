#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include <filesystem>
#include <fstream>
#include <memory>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <boost/program_options.hpp>
#include <yaml_cpp_struct.hpp>

#include "utils.h"

using SslWebsocketStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
using WebsocketStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;

struct Ssl {
	std::string crt;
	std::string key;
};
YCS_ADD_STRUCT(Ssl, crt, key)

struct Auth {
	std::string path;
	std::unordered_set<std::string> passwords;
};
YCS_ADD_STRUCT(Auth, path, passwords)

struct DefaultAction {
	std::optional<std::string> href_url;
	std::optional<std::string> html_path;
};
YCS_ADD_STRUCT(DefaultAction, href_url, html_path)

struct Config {
	std::string endpoint;
	Ssl ssl;
	std::optional<uint32_t> threads;
	std::string proxy_host;
	std::string proxy_port;
	Auth auth;
	std::optional<std::string> log_level;
	std::optional<std::string> log_file;
	std::string forward_host;
	std::string forward_port;
	DefaultAction default_action;
};
YCS_ADD_STRUCT(Config, endpoint, ssl, threads, proxy_host, proxy_port, auth, log_level, log_file, forward_host, forward_port, default_action)

template <typename... Args>
void set_option(Args... args) {
	auto func = [](auto& sock_ptr) {
		boost::system::error_code ec;
		sock_ptr->set_option(boost::asio::ip::tcp::no_delay(true), ec);
		SPDLOG_DEBUG("set no_delay: {}", ec.message());
		sock_ptr->set_option(boost::asio::socket_base::keep_alive(true), ec);
		SPDLOG_DEBUG("set keep_alive: {}", ec.message());
	};
	(func(args), ...);
}

template <typename... Args>
async_simple::coro::Lazy<> close(Args... args) {
	auto func = [](auto& sock_ptr) -> async_simple::coro::Lazy<> {
		if constexpr (std::is_same_v<std::decay_t<decltype(sock_ptr)>, std::shared_ptr<SslWebsocketStream>> ||
					  std::is_same_v<std::decay_t<decltype(sock_ptr)>, std::shared_ptr<WebsocketStream>>) {
			if (sock_ptr->is_open()) {
				// boost::system::error_code ec;
				// boost::beast::get_lowest_layer(*sock_ptr).socket().cancel(ec);
				// boost::beast::get_lowest_layer(*sock_ptr).socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
				if (auto ec = co_await async_close_ws(*sock_ptr); ec)
					SPDLOG_ERROR("close websocket error: [{}]", ec.message());
			}
		}
		else if (std::is_same_v<std::decay_t<decltype(sock_ptr)>, std::shared_ptr<boost::asio::ip::tcp::socket>>) {
			if (sock_ptr->is_open()) {
				boost::system::error_code ec;
				sock_ptr->cancel(ec);
				sock_ptr->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
				sock_ptr->close(ec);
				if (ec)
					SPDLOG_ERROR("close socket error: [{}]", ec.message());
			}
		}
		else {
			SPDLOG_DEBUG("close error type");
			exit(1);
		}
	};
	(co_await func(args), ...);
	co_return;
}

async_simple::coro::Lazy<void> forward_proxy_to_ws(std::shared_ptr<SslWebsocketStream> ws_ptr,
	std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr, std::shared_ptr<AsioExecutor> ex) {
	constexpr int32_t BufferLen{8192};
	std::unique_ptr<uint8_t[]> buffer_ptr = std::make_unique<uint8_t[]>(BufferLen);
	ws_ptr->binary(true);
	while (true) {
		auto [ec, count] = co_await async_read_some(*socket_ptr, boost::asio::buffer(buffer_ptr.get(), BufferLen));
		if (ec) {
			if (ec != boost::asio::error::operation_aborted)
				SPDLOG_DEBUG("[forward_proxy_to_ws] async_read_some: [{}]", ec.message());
			co_await close(ws_ptr, socket_ptr);
			co_return;
		}
		if (auto [ec, _] = co_await async_write_ws(*ws_ptr, boost::asio::buffer(buffer_ptr.get(), count)); ec) {
			if (ec != boost::asio::error::operation_aborted)
				SPDLOG_DEBUG("[forward_proxy_to_ws] async_write_ws: [{}]", ec.message());
			co_await close(ws_ptr, socket_ptr);
			co_return;
		}
	}
}

async_simple::coro::Lazy<void> forward_ws_to_proxy(std::shared_ptr<SslWebsocketStream> ws_ptr,
	std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr, std::shared_ptr<AsioExecutor> ex) {
	while (true) {
		boost::beast::flat_buffer buffer_;
		auto [ec, count] = co_await async_read_ws(*ws_ptr, buffer_);
		if (ec == boost::beast::websocket::error::closed) {
			SPDLOG_DEBUG("boost::beast::websocket::error::closed!!!");
			co_await close(ws_ptr, socket_ptr);
			co_return;
		}
		if (ec) {
			if (ec != boost::asio::error::operation_aborted)
				SPDLOG_DEBUG("[forward_ws_to_proxy] async_read_ws: [{}]", ec.message());
			co_await close(ws_ptr, socket_ptr);
			co_return;
		}
		if (auto [ec, _] = co_await async_write(*socket_ptr, std::move(buffer_)); ec) {
			co_await close(ws_ptr, socket_ptr);
			SPDLOG_DEBUG("[forward_ws_to_proxy] async_write: [{}]", ec.message());
			co_return;
		}
	}
}

async_simple::coro::Lazy<std::shared_ptr<boost::asio::ip::tcp::socket>> create_proxy_socket(
	boost::asio::io_context& context, std::string& proxy_host, std::string& proxy_port) {
	auto socket_ptr = std::make_shared<boost::asio::ip::tcp::socket>(context);
	boost::asio::ip::tcp::resolver resolver{context};
	auto [resolver_ec, resolver_results] = co_await async_resolve(resolver, proxy_host, proxy_port);
	if (resolver_ec) {
		SPDLOG_ERROR("async_resolve: {} host: [{}] port: [{}]", resolver_ec.message(), proxy_host, proxy_port);
		co_return nullptr;
	}
	SPDLOG_DEBUG("async_connect: [{}:{}]", proxy_host, proxy_port);
	if (auto ec = co_await async_connect(context, *socket_ptr, resolver_results, 5000); ec) {
		SPDLOG_ERROR("async_connect: {}, host: [{}] port: [{}]", ec.message(), proxy_host, proxy_port);
		co_return nullptr;
	}
	set_option(socket_ptr);
	co_return socket_ptr;
}

template <typename R, typename W>
async_simple::coro::Lazy<void> start_exchange(R r_ptr, W w_ptr, std::shared_ptr<AsioExecutor> ex) {
	boost::beast::flat_buffer buffer_;
	while (true) {
		buffer_.consume(buffer_.size());
		auto [ec, count] = co_await async_read_ws(*r_ptr, buffer_);
		if (ec) {
			if (ec != boost::asio::error::operation_aborted)
				SPDLOG_DEBUG("[start_exchange] async_read_ws: [{}]", ec.message());
			co_await close(r_ptr, w_ptr);
			co_return;
		}
		if (auto [ec, _] = co_await async_write_ws(*w_ptr, boost::asio::buffer(buffer_.data(), buffer_.size())); ec) {
			co_await close(r_ptr, w_ptr);
			if (ec != boost::asio::error::operation_aborted)
				SPDLOG_DEBUG("[start_exchange] async_write: [{}]", ec.message());
			co_return;
		}
	}
	co_return;
}

async_simple::coro::Lazy<void> forward_request_to_ws_svr(std::shared_ptr<SslWebsocketStream> ws_ptr,
	std::shared_ptr<AsioExecutor> ex, Config& cfg, std::unordered_map<std::string, std::string> http_headers) {
	boost::asio::ip::tcp::resolver resolver{ex->m_io_context};
	SPDLOG_DEBUG("cfg.forward_host: [{}], cfg.forward_port: [{}]", cfg.forward_host, cfg.forward_port);
	auto [resolver_ec, resolver_results] = co_await async_resolve(resolver, cfg.forward_host, cfg.forward_port);
	if (resolver_ec) {
		SPDLOG_DEBUG("resolver_ec: [{}]", resolver_ec.message());
		co_await close(ws_ptr);
		co_return;
	}
	SPDLOG_DEBUG("resolver_results size: [{}]", resolver_results.size());
	WebsocketStream ws_{ex->m_io_context};
	boost::system::error_code ec;
	boost::beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
	auto [con_ec, ep] = co_await async_connect_ws(ws_, resolver_results);
	if (con_ec) {
		SPDLOG_ERROR("async_connect_ws: [{}]", con_ec.message());
		co_await close(ws_ptr);
		co_return;
	}
	boost::beast::get_lowest_layer(ws_).expires_never();
	boost::beast::get_lowest_layer(ws_).socket().set_option(boost::asio::ip::tcp::no_delay(true), ec);
	SPDLOG_DEBUG("set tcp no_delay: [{}]", ec.message());
	boost::beast::get_lowest_layer(ws_).socket().set_option(boost::asio::socket_base::keep_alive(true), ec);
	SPDLOG_DEBUG("set tcp keep_alive: [{}]", ec.message());
	ws_.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
	ws_.set_option(boost::beast::websocket::stream_base::decorator([http_headers = std::move(http_headers)](boost::beast::websocket::request_type& req) mutable {
		for (auto& [name, value] : http_headers) {
			SPDLOG_DEBUG("Field: {}: {}", name, value);
			if (name == "Sec-WebSocket-Key")
				continue;
			req.set(name, value);
		}
		http_headers.clear();
	}));
	auto server_host = fmt::format("{}:{}", cfg.forward_host, std::to_string(ep.port()));
	SPDLOG_DEBUG("server_host: {}, path: {}", server_host, cfg.auth.path);
	if (auto ec = co_await async_handshake(ws_, server_host, cfg.auth.path); ec) {
		SPDLOG_ERROR("websocket async_handshake error: {}", ec.message());
		co_await close(ws_ptr);
		co_return;
	}
	auto forward_ws_ptr = std::make_shared<boost::beast::websocket::stream<boost::beast::tcp_stream>>(std::move(ws_));
	start_exchange(ws_ptr, forward_ws_ptr, ex).via(ex.get()).detach();
	start_exchange(forward_ws_ptr, ws_ptr, ex).via(ex.get()).detach();
	co_return;
}

async_simple::coro::Lazy<void> start_session(std::shared_ptr<AsioExecutor> ex, std::shared_ptr<boost::asio::ip::tcp::socket> sock,
	boost::asio::ssl::context& ctx, Config& cfg) {
	boost::beast::ssl_stream<boost::beast::tcp_stream> stream{std::move(*sock), ctx};
	if (auto ec = co_await async_handshake_server(stream); ec) {
		SPDLOG_ERROR("async_handshake_server error: {}", ec.message());
		co_return;
	}
	boost::beast::http::request<boost::beast::http::string_body> request;
	boost::beast::flat_buffer buffer_;
	auto [http_ec, http_count] = co_await async_read_http(stream, buffer_, request);
	if (http_ec == boost::beast::http::error::end_of_stream) {
		SPDLOG_ERROR("client disconnect!!!");
		co_return;
	}
	std::unordered_map<std::string, std::string> http_headers;
	for (auto& h : request.base())
		http_headers.emplace(h.name_string(), h.value());
	std::string path{request.target()};
	if (!boost::beast::websocket::is_upgrade(request) || path != cfg.auth.path) {
		SPDLOG_INFO("is_upgrade: [{}], path: [{}]", boost::beast::websocket::is_upgrade(request), path.data());
		boost::beast::http::response<boost::beast::http::string_body> res{boost::beast::http::status::moved_permanently, request.version()};
		res.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
		if (cfg.default_action.html_path) {
			auto& path = cfg.default_action.html_path.value();
			if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
				std::ifstream file(path, std::ios::in | std::ios::binary);
				res.result(boost::beast::http::status::ok);
				res.body() = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
			}
			else {
				res.set(boost::beast::http::field::location, cfg.default_action.href_url ? cfg.default_action.href_url.value() : "http://www.baidu.com");
			}
		}
		else {
			res.set(boost::beast::http::field::location, cfg.default_action.href_url ? cfg.default_action.href_url.value() : "http://www.baidu.com");
		}
		res.prepare_payload();
		co_await async_write(stream, std::move(res));
		co_return;
	}
	auto ws_ptr = std::make_shared<SslWebsocketStream>(std::move(stream));
	ws_ptr->set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
	ws_ptr->set_option(boost::beast::websocket::stream_base::decorator([](boost::beast::websocket::response_type& res) {
		res.set(boost::beast::http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " wormhole-server");
	}));
	auto ec = co_await async_accept_ws(*ws_ptr, request);
	if (ec) {
		SPDLOG_ERROR("[async_accept_ws]: {}", ec.message());
		co_return;
	}
	std::string is_self_client;
	if (http_headers.contains("self-client"))
		is_self_client = http_headers["self-client"];
	if (is_self_client != "true") {
		SPDLOG_DEBUG("start call forward_request_to_ws_svr");
		co_await forward_request_to_ws_svr(std::move(ws_ptr), std::move(ex), cfg, std::move(http_headers));
		co_return;
	}

	buffer_.consume(buffer_.size());
	if (auto [ec, count] = co_await async_read_ws(*ws_ptr, buffer_); ec) {
		SPDLOG_ERROR("[start_session] async_read_ws: {}", ec.message());
		co_return;
	}
	std::string request_auth_pwd = boost::beast::buffers_to_string(buffer_.data());
	if (cfg.auth.passwords.find(request_auth_pwd) == cfg.auth.passwords.end()) {
		SPDLOG_ERROR("[start_session] auth fail: {}", request_auth_pwd);
		co_await close(ws_ptr);
		co_return;
	}
	if (auto [ec, count] = co_await async_write_ws(*ws_ptr, boost::asio::buffer("OK", 2)); ec) {
		SPDLOG_ERROR("[start_session] async_read_ws: {}", ec.message());
		co_return;
	}
	SPDLOG_DEBUG("self auth ok: [{}]", cfg.auth.path);
	auto socket_ptr = co_await create_proxy_socket(ex->m_io_context, cfg.proxy_host, cfg.proxy_port);
	if (socket_ptr == nullptr) {
		co_await close(ws_ptr);
		co_return;
	}
	forward_proxy_to_ws(ws_ptr, socket_ptr, ex).via(ex.get()).detach();
	forward_ws_to_proxy(ws_ptr, socket_ptr, ex).via(ex.get()).detach();
	co_return;
}

int main(int argc, char** argv) {
	std::string config_file;
	boost::program_options::options_description desc("options");
	// clang-format off
	desc.add_options()
		("config", boost::program_options::value<std::string>(&config_file)->default_value("config.yaml")->value_name("CONFIG"), "specify config file")
		("help,h", "print help message");
	// clang-format on
	boost::program_options::positional_options_description pd;
	pd.add("config", 1);
	boost::program_options::variables_map vm;
	boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(desc).positional(pd).run(), vm);
	boost::program_options::notify(vm);
	if (vm.count("help") || !vm.count("config")) {
		std::cerr << desc;
		return -1;
	}

	auto [config, error] = yaml_cpp_struct::from_yaml<Config>(config_file);
	if (!config) {
		SPDLOG_ERROR("{}", error);
		return -1;
	}
	auto& cfg = config.value();

	spdlog::set_default_logger([&] {
		if (cfg.log_file && !cfg.log_file.value().empty()) {
			spdlog::flush_every(std::chrono::seconds(1));
			return spdlog::basic_logger_mt("bridge", cfg.log_file.value());
		}
		return spdlog::stdout_logger_mt("console");
	}());
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e][thread %t][%s:%#][%l] %v");
	spdlog::set_level(cfg.log_level ? spdlog::level::from_str(cfg.log_level.value()) : spdlog::level::debug);

	IoContextPool pool(cfg.threads ? cfg.threads.value() : std::thread::hardware_concurrency());
	pool.start();
	auto& context = pool.getIoContext();
	boost::asio::ip::tcp::acceptor acceptor(context);
	boost::asio::ip::tcp::resolver resolver(context);
	auto [host, port] = split(cfg.endpoint);
	boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(host, port).begin();
	std::stringstream ss;
	ss << endpoint;
	SPDLOG_INFO("server start accept at {} ...", ss.str());
	acceptor.open(endpoint.protocol());
	acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
	acceptor.bind(endpoint);
	boost::system::error_code ec;
	acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
	if (ec) {
		SPDLOG_ERROR("{}", ec.message());
		return -1;
	}
	boost::asio::signal_set sigset(context, SIGINT, SIGTERM);
	sigset.async_wait([&](const boost::system::error_code&, int) { acceptor.close(); });

	boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};
	ctx.use_certificate_chain_file(cfg.ssl.crt, ec);
	if (ec) {
		SPDLOG_ERROR("{}", ec.message());
		return -1;
	}
	ctx.use_private_key_file(cfg.ssl.key, boost::asio::ssl::context::pem, ec);
	if (ec) {
		SPDLOG_ERROR("{}", ec.message());
		return -1;
	}
	ctx.set_options(boost::asio::ssl::context::default_workarounds |
					boost::asio::ssl::context::no_tlsv1 |
					boost::asio::ssl::context::no_tlsv1_1);

	async_simple::coro::syncAwait([&]() mutable -> async_simple::coro::Lazy<void> {
		while (true) {
			auto& context = pool.getIoContext();
			auto socket_ptr = std::make_shared<boost::asio::ip::tcp::socket>(context);
			auto ec = co_await async_accept(acceptor, *socket_ptr);
			if (ec) {
				if (ec == boost::asio::error::operation_aborted)
					break;
				SPDLOG_ERROR("Accept failed, error: {}", ec.message());
				continue;
			}
			auto executor = std::make_shared<AsioExecutor>(context);
			set_option(socket_ptr);
			start_session(executor, std::move(socket_ptr), ctx, cfg).via(executor.get()).detach();
		}
	}());
	pool.stop();
	return 0;
}