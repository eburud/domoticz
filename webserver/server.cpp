//
// server.cpp
// ~~~~~~~~~~
//
#include "stdafx.h"
#include "server.hpp"
#include <fstream>
#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/mainworker.h"

namespace http {
namespace server {

	server_base::server_base(const server_settings &settings, request_handler &user_request_handler)
		: io_context_()
		, acceptor_(io_context_)
		, request_handler_(user_request_handler)
		, settings_(settings)
		, timeout_(20)
		, // default read timeout in seconds
		is_running(false)
		, is_stop_complete(false)
		, m_heartbeat_timer(io_context_)
	{
		if (!settings.is_enabled())
		{
			throw std::invalid_argument("cannot initialize a disabled server (listening port cannot be empty or 0)");
		}
	}

	void server_base::init(const init_connectionhandler_func &init_connection_handler, accept_handler_func accept_handler)
	{
		init_connection_handler();

		if (!new_connection_)
		{
			throw std::invalid_argument("cannot initialize a server without a valid connection");
		}

		// Open the acceptor with the option to reuse the address (i.e. SO_REUSEADDR).
		boost::asio::ip::tcp::resolver resolver(io_context_);
		boost::asio::ip::basic_resolver<boost::asio::ip::tcp>::results_type endpoints = resolver.resolve(settings_.listening_address, settings_.listening_port);
		auto endpoint = *endpoints.begin();
		acceptor_.open(endpoint.endpoint().protocol());
		acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
		// bind to both ipv6 and ipv4 sockets for the "::" address only
		if (settings_.listening_address == "::")
		{
			acceptor_.set_option(boost::asio::ip::v6_only(false));
		}
		// bind to our port
		acceptor_.bind(endpoint);
		// listen for incoming requests
		acceptor_.listen();

		// start the accept thread
		acceptor_.async_accept(new_connection_->socket(), accept_handler);
	}

void server_base::run() {
	// The io_context::run() call will block until all asynchronous operations
	// have finished. While the server is running, there is always at least one
	// asynchronous operation outstanding: the asynchronous accept call waiting
	// for new incoming connections.
	try {
		is_running = true;
		heart_beat(boost::system::error_code());
		io_context_.run();
		is_running = false;
	} catch (std::exception& e) {
		_log.Log(LOG_ERROR, "[web:%s] exception occurred : '%s' (need to run again)", settings_.listening_port.c_str(), e.what());
		is_running = false;
		// Note: if acceptor is up everything is OK, we can call run() again
		//       but if the exception has broken the acceptor we cannot stop/start it and the next run() will exit immediatly.
		io_context_.restart(); // this call is needed before calling run() again
		throw;
	} catch (...) {
		_log.Log(LOG_ERROR, "[web:%s] unknown exception occurred (need to run again)", settings_.listening_port.c_str());
		is_running = false;
		// Note: if acceptor is up everything is OK, we can call run() again
		//       but if the exception has broken the acceptor we cannot stop/start it and the next run() will exit immediatly.
		io_context_.restart(); // this call is needed before calling run() again
		throw;
	}
}

/// Ask the server to stop using asynchronous command
void server_base::stop() {
	if (is_running) {
		// Post a call to the stop function so that server_base::stop() is safe to call from any thread.
		// Rene, set is_running to false, because the following is an io_context call, which makes is_running
		// never set to false whilst in the call itself
		is_running = false;
		boost::asio::post(io_context_, [this] { handle_stop(); });
	} else {
		// if io_context is not running then the post call will not be performed
		handle_stop();
	}

	// Wait for acceptor and connections to stop
	int timeout = 15; // force stop after 15 seconds
	time_t start = mytime(nullptr);
	while(true) {
		if (!is_running && is_stop_complete) {
			break;
		}
		if ((mytime(nullptr) - start) > timeout)
		{
			// timeout occurred
			break;
		}
		sleep_milliseconds(500);
	}
	io_context_.stop();

	// Deregister heartbeat
	m_mainworker.HeartbeatRemove(std::string("WebServer:") + settings_.listening_port);
}

void server_base::handle_stop() {
	try {
		boost::system::error_code ignored_ec;
		acceptor_.close(ignored_ec);
	} catch (...) {
		_log.Log(LOG_ERROR, "[web:%s] exception occurred while closing acceptor", settings_.listening_port.c_str());
	}
	connection_manager_.stop_all();
	is_stop_complete = true;
}

void server_base::heart_beat(const boost::system::error_code& error)
{
	if (!error) {
		// Heartbeat
		m_mainworker.HeartbeatUpdate(std::string("WebServer:") + settings_.listening_port);

		// Schedule next heartbeat
		m_heartbeat_timer.expires_after(std::chrono::seconds(4));
		m_heartbeat_timer.async_wait([this](auto &&err) { heart_beat(err); });
	}
}

server::server(const server_settings &settings, request_handler &user_request_handler)
	: server_base(settings, user_request_handler)
{
	init([this] { init_connection(); }, [this](auto &&err) { handle_accept(err); });
}

void server::init_connection() {
	new_connection_.reset(new connection(io_context_, connection_manager_, request_handler_, timeout_));
}

/**
 * accepting incoming requests and start the client connection loop
 */
void server::handle_accept(const boost::system::error_code& e) {
	if (!e) {
		connection_manager_.start(new_connection_);
		new_connection_.reset(new connection(io_context_,
				connection_manager_, request_handler_, timeout_));
		// listen for a subsequent request
		acceptor_.async_accept(new_connection_->socket(), [this](auto &&err) { handle_accept(err); });
	}
}

#ifdef WWW_ENABLE_SSL
ssl_server::ssl_server(const ssl_server_settings &ssl_settings, request_handler &user_request_handler)
	: server_base(ssl_settings, user_request_handler)
	, settings_(ssl_settings)
	, context_(ssl_settings.get_ssl_method())
{
	init([this] { init_connection(); }, [this](auto &&err) { handle_accept(err); });
}

// this constructor will send std::bad_cast exception if the settings argument is not a ssl_server_settings object
ssl_server::ssl_server(const server_settings &settings, request_handler &user_request_handler)
	: server_base(settings, user_request_handler)
	, settings_(dynamic_cast<ssl_server_settings const &>(settings))
	, context_(dynamic_cast<ssl_server_settings const &>(settings).get_ssl_method())
{
	init([this] { init_connection(); }, [this](auto &&err) { handle_accept(err); });
}

void ssl_server::init_connection() {
	// the following line gets the passphrase for protected private server keys
	context_.set_password_callback([this](auto &&...) { return get_passphrase(); });

	if (settings_.ssl_options.empty()) {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL options parameter !", settings_.listening_port.c_str());
	} else {
		context_.set_options(settings_.get_ssl_options());
	}

	const char* cipher_list = &settings_.cipher_list[0];
	SSL_CTX_set_cipher_list(context_.native_handle(), cipher_list);
	_log.Debug(DEBUG_WEBSERVER, "[web:%s] Enabled ciphers (TLSv1.2) %s", settings_.listening_port.c_str(), settings_.cipher_list.c_str());

	SSL_CTX_set_min_proto_version(context_.native_handle(), TLS1_2_VERSION);
	SSL_CTX_set_options(context_.native_handle(), SSL_OP_CIPHER_SERVER_PREFERENCE);
	SSL_CTX_set_options(context_.native_handle(), SSL_OP_NO_RENEGOTIATION);

	struct stat st;
	if (settings_.certificate_chain_file_path.empty()) {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL certificate chain file parameter !", settings_.listening_port.c_str());
	} else if (!stat(settings_.certificate_chain_file_path.c_str(), &st)) {
		cert_chain_tm_ = st.st_mtime;
		context_.use_certificate_chain_file(settings_.certificate_chain_file_path);
	} else {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL certificate chain file %s!", settings_.listening_port.c_str(), settings_.certificate_chain_file_path.c_str());
	}

	if (settings_.cert_file_path.empty()) {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL certificate file parameter !", settings_.listening_port.c_str());
	} else if (!stat(settings_.cert_file_path.c_str(), &st)) {
		cert_tm_ = st.st_mtime;
		context_.use_certificate_file(settings_.cert_file_path, boost::asio::ssl::context::pem);
	} else {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL certificate file %s!", settings_.listening_port.c_str(), settings_.cert_file_path.c_str());
	}

	if (settings_.private_key_file_path.empty()) {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL private key file parameter !", settings_.listening_port.c_str());
	} else if (!stat(settings_.private_key_file_path.c_str(), &st)) {
		// We don't actually bother to track the mtime of the private
		// key file as it can't sanely change without changing the
		// certificate too. And may in fact change *before* the
		// certificate does, while the cert is being issued. We
		// don't want to update until the *cert* file changes.
		context_.use_private_key_file(settings_.private_key_file_path, boost::asio::ssl::context::pem);
	} else {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL private key file %s!", settings_.listening_port.c_str(), settings_.private_key_file_path.c_str());
	}

	// Do not work with mobile devices at this time (2016/02)
	if (settings_.verify_peer || settings_.verify_fail_if_no_peer_cert) {
		if (settings_.verify_file_path.empty()) {
			_log.Log(LOG_ERROR, "[web:%s] missing SSL verify file parameter !", settings_.listening_port.c_str());
		} else {
			context_.load_verify_file(settings_.verify_file_path);
			boost::asio::ssl::context::verify_mode verify_mode = 0;
			if (settings_.verify_peer) {
				verify_mode |= boost::asio::ssl::context::verify_peer;
			}
			if (settings_.verify_fail_if_no_peer_cert) {
				verify_mode |= boost::asio::ssl::context::verify_fail_if_no_peer_cert;
			}
			context_.set_verify_mode(verify_mode);
		}
	}

	// Load DH parameters
	if (settings_.tmp_dh_file_path.empty()) {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL DH file parameter", settings_.listening_port.c_str());
	} else if (!stat(settings_.tmp_dh_file_path.c_str(), &st)) {
		dhparam_tm_ = st.st_mtime;

		std::ifstream ifs(settings_.tmp_dh_file_path.c_str());
		std::string content((std::istreambuf_iterator<char>(ifs)),
				(std::istreambuf_iterator<char>()));
		if (content.find("DH PARAMETERS") != std::string::npos) {
			context_.use_tmp_dh_file(settings_.tmp_dh_file_path);
			_log.Debug(DEBUG_WEBSERVER, "[web:%s] 'DH PARAMETERS' found in file %s", settings_.listening_port.c_str(), settings_.tmp_dh_file_path.c_str());
		} else {
			_log.Log(LOG_ERROR, "[web:%s] missing SSL DH parameters from file %s", settings_.listening_port.c_str(), settings_.tmp_dh_file_path.c_str());
		}
	} else {
		_log.Log(LOG_ERROR, "[web:%s] missing SSL DH parameters file %s!", settings_.listening_port.c_str(), settings_.tmp_dh_file_path.c_str());
	}
	new_connection_.reset(new connection(io_context_, connection_manager_, request_handler_, timeout_, context_));
}

void ssl_server::reinit_connection()
{
	struct stat st;

	if ((!settings_.certificate_chain_file_path.empty() &&
	     !stat(settings_.certificate_chain_file_path.c_str(), &st) &&
	     st.st_mtime != cert_chain_tm_)) {
		cert_chain_tm_ = st.st_mtime;
		_log.Log(LOG_STATUS, "[web:%s] Reloading SSL certificate chain file", settings_.listening_port.c_str());
		context_.use_certificate_chain_file(settings_.certificate_chain_file_path);
	}

	if (!settings_.cert_file_path.empty() &&
	    !stat(settings_.cert_file_path.c_str(), &st) &&
	    st.st_mtime != cert_tm_) {
		cert_tm_ = st.st_mtime;
		_log.Log(LOG_STATUS, "[web:%s] Reloading SSL certificate and private key", settings_.listening_port.c_str());
		context_.use_certificate_file(settings_.cert_file_path, boost::asio::ssl::context::pem);
		context_.use_private_key_file(settings_.private_key_file_path, boost::asio::ssl::context::pem);
	}

	if (!settings_.tmp_dh_file_path.empty() &&
	    !stat(settings_.tmp_dh_file_path.c_str(), &st) &&
	    st.st_mtime != dhparam_tm_) {
		dhparam_tm_ = st.st_mtime;
		std::ifstream ifs(settings_.tmp_dh_file_path.c_str());
		std::string content((std::istreambuf_iterator<char>(ifs)),
				(std::istreambuf_iterator<char>()));
		if (content.find("DH PARAMETERS") != std::string::npos) {
			_log.Log(LOG_STATUS, "[web:%s] Reloading SSL DH parameters", settings_.listening_port.c_str());
			context_.use_tmp_dh_file(settings_.tmp_dh_file_path);
		} else {
			_log.Log(LOG_ERROR, "[web:%s] missing SSL DH parameters from file %s", settings_.listening_port.c_str(), settings_.tmp_dh_file_path.c_str());
		}
	}
	new_connection_.reset(new connection(io_context_, connection_manager_, request_handler_, timeout_, context_));
}

/**
 * accepting incoming requests and start the client connection loop
 */
void ssl_server::handle_accept(const boost::system::error_code& e) {
	if (!e) {
		connection_manager_.start(new_connection_);
		reinit_connection();
		// listen for a subsequent request
		acceptor_.async_accept(new_connection_->socket(), [this](auto &&err) { handle_accept(err); });
	}
}

std::string ssl_server::get_passphrase() const {
	return settings_.private_key_pass_phrase;
}
#endif

std::shared_ptr<server_base> server_factory::create(const server_settings & settings, request_handler & user_request_handler) {
#ifdef WWW_ENABLE_SSL
		if (settings.is_secure()) {
			return create(dynamic_cast<ssl_server_settings const &>(settings), user_request_handler);
		}
#endif
		return std::shared_ptr<server_base>(new server(settings, user_request_handler));
	}

#ifdef WWW_ENABLE_SSL
std::shared_ptr<server_base> server_factory::create(const ssl_server_settings & ssl_settings, request_handler & user_request_handler) {
		return std::shared_ptr<server_base>(new ssl_server(ssl_settings, user_request_handler));
	}
#endif

} // namespace server
} // namespace http
