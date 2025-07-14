/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/eventshandler.hpp"
#include "remote/httputility.hpp"
#include "remote/filterutility.hpp"
#include "config/configcompiler.hpp"
#include "config/expression.hpp"
#include "base/defer.hpp"
#include "base/io-engine.hpp"
#include "base/objectlock.hpp"
#include "base/json.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <map>
#include <set>

using namespace icinga;

REGISTER_URLHANDLER("/v1/events", EventsHandler);

const std::map<String, EventType> l_EventTypes ({
	{"AcknowledgementCleared", EventType::AcknowledgementCleared},
	{"AcknowledgementSet", EventType::AcknowledgementSet},
	{"CheckResult", EventType::CheckResult},
	{"CommentAdded", EventType::CommentAdded},
	{"CommentRemoved", EventType::CommentRemoved},
	{"DowntimeAdded", EventType::DowntimeAdded},
	{"DowntimeRemoved", EventType::DowntimeRemoved},
	{"DowntimeStarted", EventType::DowntimeStarted},
	{"DowntimeTriggered", EventType::DowntimeTriggered},
	{"Flapping", EventType::Flapping},
	{"Notification", EventType::Notification},
	{"StateChange", EventType::StateChange},
	{"ObjectCreated", EventType::ObjectCreated},
	{"ObjectDeleted", EventType::ObjectDeleted},
	{"ObjectModified", EventType::ObjectModified}
});

const String l_ApiQuery ("<API query>");

bool EventsHandler::HandleRequest(
	const WaitGroup::Ptr&,
	HttpRequest& request,
	HttpResponse& response,
	boost::asio::yield_context& yc
)
{
	namespace asio = boost::asio;
	namespace http = boost::beast::http;

	if (request.Url()->GetPath().size() != 2)
		return false;

	if (request.method() != http::verb::post)
		return false;

	if (request.version() == 10) {
		response.SendJsonError(request.Params(), 400, "HTTP/1.0 not supported for event streams.");
		return true;
	}

	Array::Ptr types = request.Params()->Get("types");

	if (!types) {
		response.SendJsonError(request.Params(), 400, "'types' query parameter is required.");
		return true;
	}

	{
		ObjectLock olock(types);
		for (String type : types) {
			FilterUtility::CheckPermission(request.User(), "events/" + type);
		}
	}

	String queueName = request.GetLastParameter("queue");

	if (queueName.IsEmpty()) {
		response.SendJsonError(request.Params(), 400, "'queue' query parameter is required.");
		return true;
	}

	std::set<EventType> eventTypes;

	{
		ObjectLock olock(types);
		for (String type : types) {
			auto typeId (l_EventTypes.find(type));

			if (typeId != l_EventTypes.end()) {
				eventTypes.emplace(typeId->second);
			}
		}
	}

	EventsSubscriber subscriber (std::move(eventTypes), request.GetLastParameter("filter"), l_ApiQuery);

	response.result(http::status::ok);
	response.set(http::field::content_type, "application/json");
	response.StartStreaming();

	/**
	 * We don't want to keep the connection alive once we return from this handler, as the client
	 * is expected to be gone at that point. Otherwise, this will cause a "broken pipe" error in
	 * the HttpServerConnection class when it tries to read from the stream again.
	 */
	request.keep_alive(false);

	IoBoundWorkSlot dontLockTheIoThread (yc);

	auto writer = std::make_shared<BeastHttpMessageAdapter<HttpResponse>>(response);
	JsonEncoder encoder(writer);

	/**
	 * Start a watchdog coroutine that will read from the hijacked stream until the client disconnects.
	 * Though, the read op should block like forever, since we don't expect the client to send any data
	 * at this point, it's just a way to keep track of the client liveness. Once the client disconnects,
	 * the read operation will complete with an error, and we can safely discard the inbox and stop processing events.
	 *
	 * This will ensure that the Shift operation below doesn't unnecessarily wait for events that are never going
	 * to be sent, and allows the HttpServerConnection to gracefully handle the disconnection.
	 *
	 * @note This coroutine is spawned on the very same executor used by the coroutine from which this handler is
	 * called (HttpServerConnection#m_IoStrand = yc.get_executor()), so we can safely perform I/O ops on the stream.
	 */
	IoEngine::SpawnCoroutine(yc.get_executor(), [inbox = subscriber.GetInbox(), &response](asio::yield_context yc) {
		char buf[128];
		asio::mutable_buffer readBuf(buf, 128);
		boost::system::error_code ec;

		const auto& stream(response.HijackStream());
		do {
			stream->async_read_some(readBuf, yc[ec]);
		} while (!ec);

		response.SetClientAlive(false);
		inbox->Discard(yc);
	});

	for (;;) {
		auto event(subscriber.GetInbox()->Shift(yc));

		if (event) {
			encoder.Encode(event);
			response.body() << '\n';
			response.Write(yc);
		} else if (!response.IsWritable()) {
			return true;
		}
	}
}
