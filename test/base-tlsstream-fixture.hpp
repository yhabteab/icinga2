/* Icinga 2 | (c) 2025 Icinga GmbH | GPLv2+ */

#ifndef TLSSTREAM_FIXTURE_H
#define TLSSTREAM_FIXTURE_H

#include "test/base-sslcert-fixture.hpp"
#include "base/tlsstream.hpp"
#include "base/io-engine.hpp"
#include <BoostTestTargetConfig.h>

namespace icinga {

/**
 * Creates a pair of TLS Streams on a random unused port.
 */
struct TlsStreamFixture: SslCertificateFixture
{
	TlsStreamFixture()
	{
		using namespace boost::asio::ip;
		using handshake_type = boost::asio::ssl::stream_base::handshake_type;

		EnsureCertFor("remote");
		EnsureCertFor("local");

		auto& localContext = IoEngine::Get().GetIoContext();

		remoteSslCtx = SetupSslContext(certsDir + "remote.crt", certsDir + "remote.key", caDir+"ca.crt", "", DEFAULT_TLS_CIPHERS,
			DEFAULT_TLS_PROTOCOLMIN, DebugInfo());
		client = Shared<AsioTlsStream>::Make(IoEngine::Get().GetIoContext(), *remoteSslCtx);

		localSslCtx = SetupSslContext(certsDir + "local.crt", certsDir + "local.key", caDir+"ca.crt",
			"", DEFAULT_TLS_CIPHERS, DEFAULT_TLS_PROTOCOLMIN, DebugInfo());
		server = Shared<AsioTlsStream>::Make(localContext, *localSslCtx);

		std::mutex handshakeMutex;
		std::condition_variable handshakeCv;
		bool handshakeDone = false;

		tcp::acceptor acceptor{localContext, tcp::endpoint{make_address_v4("127.0.0.1"), 0}};
		acceptor.listen();
		acceptor.async_accept(server->lowest_layer(), [&, this](const boost::system::error_code& ec) {
			if (ec) {
				std::cout << "Local Accept Error: " << ec.message() << std::endl;
				return;
			}
			server->next_layer().async_handshake(handshake_type::server, [&, this](const boost::system::error_code& ec) {
				if (ec) {
					std::cout << "Local Handshake Error: " << ec.message() << std::endl;
					return;
				}

				handshakeMutex.lock();
				handshakeDone = true;
				handshakeMutex.unlock();
				handshakeCv.notify_all();
			});
		});

		boost::system::error_code ec;
		if (client->lowest_layer().connect(acceptor.local_endpoint(), ec)) {
			std::cout << "Client Connect error: " << ec.message() << std::endl;
		}

		if (client->next_layer().handshake(handshake_type::client, ec)) {
			std::cout << "Client Handshake error: " << ec.message() << std::endl;
		}
		if (!client->next_layer().IsVerifyOK()) {
			std::cout << "Verify failed for connection" << std::endl;
			throw;
		}

		std::unique_lock lock{handshakeMutex};
		handshakeCv.wait(lock, [&](){return handshakeDone;});
	}

	~TlsStreamFixture()
	{
		std::cout << "TlsStreamFixture done" << std::endl;
	}

	Shared<boost::asio::ssl::context>::Ptr remoteSslCtx;
	Shared<AsioTlsStream>::Ptr client;

	Shared<boost::asio::ssl::context>::Ptr localSslCtx;
	Shared<AsioTlsStream>::Ptr server;
};

} // namespace icinga

#endif // HTTPSERVERCONNECTION_FIXTURE_H
