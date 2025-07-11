/* Icinga 2 | (c) 2025 Icinga GmbH | GPLv2+ */

#include "test/base-tlsstream-fixture.hpp"
#include "remote/httpmessage.hpp"
#include "base/base64.hpp"
#include "base/json.hpp"
#include <BoostTestTargetConfig.h>

using namespace icinga;
using namespace boost::beast;

struct HttpMessageFixture: TlsStreamFixture
{};

BOOST_FIXTURE_TEST_SUITE(remote_httpmessage, HttpMessageFixture)

BOOST_AUTO_TEST_CASE(request_parse)
{
	http::request<boost::beast::http::string_body> requestOut;
	requestOut.method(http::verb::get);
	requestOut.target("https://localhost:5665/v1/test");
	requestOut.set(http::field::authorization, "Basic " + Base64::Encode("invalid:invalid"));
	requestOut.set(http::field::accept, "application/json");
	requestOut.set(http::field::connection, "close");
	requestOut.content_length(0);

	auto & io = IoEngine::Get();
	io.SpawnCoroutine(io.GetIoContext(), [&, this](boost::asio::yield_context yc) {
		boost::beast::flat_buffer buf;
		HttpRequest request(server);
		server->async_fill(yc);
		BOOST_REQUIRE_NO_THROW(request.ParseHeader(buf, yc));

		for (auto & field : requestOut.base()) {
			BOOST_REQUIRE(request.count(field.name()));
		}

		request.ParseBody(buf, yc);

		BOOST_REQUIRE(Shutdown(server));
	});

	http::write(*client, requestOut);
	client->flush();

	BOOST_REQUIRE(Shutdown(client));
}

BOOST_AUTO_TEST_CASE(request_params)
{
	HttpRequest request(client);
	request.body() = JsonEncode(new Dictionary{{"test1", false}, {"test2", true}});
	request.target("https://localhost:1234/v1/test?test1=1&test3=0&test3=1");
	request.DecodeParams();

	BOOST_REQUIRE(!request.Params()->Get("test2").IsObjectType<Array>());
	BOOST_REQUIRE(request.Params()->Get("test2").IsBoolean());
	BOOST_REQUIRE(request.Params()->Get("test2").ToBool());
	BOOST_REQUIRE(request.Params()->Get("test1").IsObjectType<Array>());
	BOOST_REQUIRE(request.GetLastParameter("test1"));
	BOOST_REQUIRE(request.Params()->Get("test3").IsObjectType<Array>());
	BOOST_REQUIRE(request.GetLastParameter("test3"));
}

BOOST_AUTO_TEST_CASE(response_body_reader)
{

}

BOOST_AUTO_TEST_CASE(response_body_writer)
{

}

BOOST_AUTO_TEST_CASE(response_write_empty)
{
	auto & io = IoEngine::Get();
	io.SpawnCoroutine(io.GetIoContext(), [&, this](boost::asio::yield_context yc) {
		boost::beast::flat_buffer buf;
		HttpResponse response(server);
		response.result(http::status::ok);

		BOOST_REQUIRE_NO_THROW(response.Flush(yc));
		server->async_flush(yc);

		BOOST_REQUIRE(Shutdown(server));
	});

	http::response_parser<http::string_body> parser;
	flat_buffer buf;
	http::read(*client, buf, parser);

	BOOST_REQUIRE_EQUAL(parser.get().result(), http::status::ok);
	BOOST_REQUIRE_EQUAL(parser.get().chunked(), false);
	BOOST_REQUIRE_EQUAL(parser.get().body(), "");

	BOOST_REQUIRE(Shutdown(client));
}

BOOST_AUTO_TEST_CASE(response_write_fixed)
{
	auto & io = IoEngine::Get();
	io.SpawnCoroutine(io.GetIoContext(), [&, this](boost::asio::yield_context yc) {
		boost::beast::flat_buffer buf;
		HttpResponse response(server);
		response.result(http::status::ok);
		response.body() << "test";

		BOOST_REQUIRE_NO_THROW(response.Flush(yc));
		server->async_flush(yc);

		BOOST_REQUIRE(Shutdown(server));
	});

	http::response_parser<http::string_body> parser;
	flat_buffer buf;
	http::read(*client, buf, parser);

	BOOST_REQUIRE_EQUAL(parser.get().result(), http::status::ok);
	BOOST_REQUIRE_EQUAL(parser.get().chunked(), false);
	BOOST_REQUIRE_EQUAL(parser.get().body(), "test");

	BOOST_REQUIRE(Shutdown(client));
}

BOOST_AUTO_TEST_CASE(response_write_chunked)
{
	auto & io = IoEngine::Get();
	io.SpawnCoroutine(io.GetIoContext(), [&, this](boost::asio::yield_context yc) {
		boost::beast::flat_buffer buf;
		HttpResponse response(server);
		response.result(http::status::ok);

		response.StartStreaming();
		response.body() << "test" << 1;
		BOOST_REQUIRE_NO_THROW(response.Flush(yc));

		response.body() << "test" << 2;
		BOOST_REQUIRE_NO_THROW(response.Flush(yc));

		response.body() << "test" << 3;
		response.body().Finish();
		BOOST_REQUIRE_NO_THROW(response.Flush(yc));

		BOOST_REQUIRE(Shutdown(server));
	});

	http::response_parser<http::string_body> parser;
	flat_buffer buf;
	http::read(*client, buf, parser);

	BOOST_REQUIRE_EQUAL(parser.get().result(), http::status::ok);
	BOOST_REQUIRE_EQUAL(parser.get().chunked(), true);
	BOOST_REQUIRE_EQUAL(parser.get().body(), "test1test2test3");

	BOOST_REQUIRE(Shutdown(client));
}

BOOST_AUTO_TEST_CASE(response_sendjsonbody)
{

}

BOOST_AUTO_TEST_SUITE_END()
