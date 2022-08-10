#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include <memory>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <boost/program_options.hpp>
#include <yaml_cpp_struct.hpp>

#include "utils.h"

struct WsConfig {
	std::string ws_host;
	std::string ws_port;
	std::string ws_sni;
	std::string path;
	std::string password;
};
YCS_ADD_STRUCT(WsConfig, ws_host, ws_port, ws_sni, path, password)

struct Config {
	std::string endpoint;
	std::optional<uint32_t> threads;
	WsConfig ws_cfg;
	std::optional<std::string> log_level;
	std::optional<std::string> log_file;
};
YCS_ADD_STRUCT(Config, endpoint, threads, ws_cfg, log_level, log_file)

using SslWebsocketStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;

async_simple::coro::Lazy<> close(std::shared_ptr<SslWebsocketStream>& stream, std::shared_ptr<boost::asio::ip::tcp::socket>& socket) {
	if (stream->is_open()) {
		// boost::system::error_code ec;
		// boost::beast::get_lowest_layer(*stream).socket().cancel(ec);
		// boost::beast::get_lowest_layer(*stream).socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
		co_await async_close_ws(*stream);
	}
	if (socket->is_open()) {
		boost::system::error_code ec;
		socket->cancel(ec);
		socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
		socket->close(ec);
	}
	co_return;
}

async_simple::coro::Lazy<std::shared_ptr<SslWebsocketStream>> create_websocket_client(const std::shared_ptr<AsioExecutor>& executor_ptr, Config& cfg) {
	boost::asio::ip::tcp::resolver resolver{executor_ptr->m_io_context};
	auto [resolver_ec, resolver_results] = co_await async_resolve(resolver, cfg.ws_cfg.ws_host, cfg.ws_cfg.ws_port);
	if (resolver_ec) {
		SPDLOG_ERROR("resolver_ec: [{}]", resolver_ec.message());
		co_return nullptr;
	}
	SPDLOG_DEBUG("resolver_results size: [{}]", resolver_results.size());
	for (auto& endpoint : resolver_results) {
		std::stringstream ss;
		ss << endpoint.endpoint();
		SPDLOG_DEBUG("resolver_results: [{}]", ss.str());
	}

	boost::asio::ssl::context ctx{boost::asio::ssl::context::tlsv13_client};
	boost::beast::ssl_stream<boost::beast::tcp_stream> stream{executor_ptr->m_io_context, ctx};
	if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg.ws_cfg.ws_sni.c_str())) {
		boost::system::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
		SPDLOG_ERROR("SSL_set_tlsext_host_name error: {}", ec.message());
		co_return nullptr;
	}
	boost::beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
	if (auto [ec, ep] = co_await async_connect(executor_ptr->m_io_context, stream, resolver_results); ec) {
		SPDLOG_DEBUG("async_connect_ssl: [{}]", ec.message());
		co_return nullptr;
	}
	boost::beast::get_lowest_layer(stream).expires_never();
	boost::system::error_code ec;
	boost::beast::get_lowest_layer(stream).socket().set_option(boost::asio::ip::tcp::no_delay(true), ec);
	SPDLOG_DEBUG("set tcp no_delay: [{}]", ec.message());
	boost::beast::get_lowest_layer(stream).socket().set_option(boost::asio::socket_base::keep_alive(true), ec);
	SPDLOG_DEBUG("set tcp keep_alive: [{}]", ec.message());

	if (auto ec = co_await async_ssl_handshake(stream, boost::asio::ssl::stream_base::client); ec) {
		SPDLOG_ERROR("async_ssl_handshake error: {}", ec.message());
		co_return nullptr;
	}
	SslWebsocketStream ws_{std::move(stream)};
	ws_.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
	ws_.set_option(boost::beast::websocket::stream_base::decorator([&](boost::beast::websocket::request_type& req) {
		req.set(boost::beast::http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " wormhole-client");
		req.set("self-client", "true");
		req.set("password", cfg.ws_cfg.password);
	}));
	auto server_host = fmt::format("{}:{}", cfg.ws_cfg.ws_sni, cfg.ws_cfg.ws_port);
	SPDLOG_DEBUG("server_host: {}", server_host);
	if (auto ec = co_await async_ws_handshake(ws_, server_host, cfg.ws_cfg.path); ec) {
		SPDLOG_ERROR("websocket async_ws_handshake error: {}", ec.message());
		co_return nullptr;
	}
	co_return std::make_shared<SslWebsocketStream>(std::move(ws_));
}

async_simple::coro::Lazy<void> forward_cli_to_ws(std::shared_ptr<SslWebsocketStream> ws_ptr,
	std::shared_ptr<boost::asio::ip::tcp::socket> sock_ptr, [[maybe_unused]] std::shared_ptr<AsioExecutor> ex) {
	constexpr int32_t BufferLen{1024 * 20};
	std::unique_ptr<uint8_t[]> buffer_ptr = std::make_unique<uint8_t[]>(BufferLen);
	ws_ptr->binary(true);
	while (true) {
		auto [ec, count] = co_await async_read_some(*sock_ptr, boost::asio::buffer(buffer_ptr.get(), BufferLen));
		if (ec) {
			co_await close(ws_ptr, sock_ptr);
			SPDLOG_DEBUG("[forward_cli_to_ws] async_read_some: {}", ec.message());
			co_return;
		}
		if (auto [ec, _] = co_await async_write_ws(*ws_ptr, boost::asio::buffer(buffer_ptr.get(), count)); ec) {
			co_await close(ws_ptr, sock_ptr);
			SPDLOG_DEBUG("[forward_cli_to_ws] async_write_ws: {}", ec.message());
			co_return;
		}
	}
};

async_simple::coro::Lazy<void> forward_ws_to_cli(std::shared_ptr<SslWebsocketStream> ws_ptr,
	std::shared_ptr<boost::asio::ip::tcp::socket> sock_ptr, [[maybe_unused]] std::shared_ptr<AsioExecutor> ex) {
	boost::beast::flat_buffer buffer_;
	while (true) {
		buffer_.consume(buffer_.size());
		auto [ec, count] = co_await async_read_ws(*ws_ptr, buffer_);
		if (ec) {
			co_await close(ws_ptr, sock_ptr);
			if (ec != boost::asio::error::operation_aborted)
				SPDLOG_DEBUG("[forward_ws_to_cli] async_read_ws: {}", ec.message());
			co_return;
		}
		if (auto [ec, _] = co_await async_write(*sock_ptr, boost::asio::buffer(buffer_.data(), buffer_.size())); ec) {
			co_await close(ws_ptr, sock_ptr);
			SPDLOG_DEBUG("forward_ws_to_cli async_write: {}", ec.message());
			co_return;
		}
	}
};

async_simple::coro::Lazy<void> start_session(std::shared_ptr<boost::asio::ip::tcp::socket> sock_ptr,
	std::shared_ptr<AsioExecutor> executor_ptr, Config& cfg) {
	auto ws_ptr = co_await create_websocket_client(executor_ptr, cfg);
	if (!ws_ptr) {
		SPDLOG_ERROR("create_websocket_client error");
		co_return;
	}
	auto ex_ptr = executor_ptr.get();
	forward_cli_to_ws(ws_ptr, sock_ptr, executor_ptr).via(ex_ptr).detach();
	forward_ws_to_cli(std::move(ws_ptr), std::move(sock_ptr), std::move(executor_ptr)).via(ex_ptr).detach();
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

	auto [cfg, error] = yaml_cpp_struct::from_yaml<Config>(config_file);
	if (!cfg) {
		SPDLOG_ERROR("{}", error);
		return -1;
	}

	spdlog::set_default_logger([&] {
		if (cfg.value().log_file && !cfg.value().log_file.value().empty()) {
			spdlog::flush_every(std::chrono::seconds(1));
			return spdlog::basic_logger_mt("bridge", cfg.value().log_file.value());
		}
		return spdlog::stdout_logger_mt("console");
	}());
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e][thread %t][%s:%#][%l] %v");
	spdlog::set_level(cfg.value().log_level ? spdlog::level::from_str(cfg.value().log_level.value()) : spdlog::level::debug);

	IoContextPool pool(cfg.value().threads ? cfg.value().threads.value() : std::thread::hardware_concurrency());
	pool.start();
	auto& context = pool.getIoContext();
	boost::asio::ip::tcp::acceptor acceptor(context);
	boost::asio::ip::tcp::resolver resolver(context);
	auto [host, port] = split(cfg.value().endpoint);
	boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(host, port).begin();
	acceptor.open(endpoint.protocol());
	acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
	acceptor.bind(endpoint);
	boost::system::error_code ec;
	acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
	if (ec) {
		SPDLOG_ERROR("{}", ec.message());
		return -1;
	}
	std::stringstream ss;
	ss << endpoint;
	SPDLOG_INFO("start accept at {} ...", ss.str());
	boost::asio::signal_set sigset(context, SIGINT, SIGTERM);
	sigset.async_wait([&](const boost::system::error_code&, int) { acceptor.close(); });
	async_simple::coro::syncAwait([&]() -> async_simple::coro::Lazy<void> {
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
			start_session(std::move(socket_ptr), executor, cfg.value()).via(executor.get()).detach();
		}
	}());
	pool.stop();
	return 0;
}
