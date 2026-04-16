#include "websocket_eventloop.h"
#include <thread>

websocket_eventloop event_loop;

void ev_run()
{
    event_loop.run();
}

void websocket_eventloop::OnExtLoad()
{
    std::thread(ev_run).detach();
}

void websocket_eventloop::OnExtUnload()
{
    this->context.stop();
    this->context.reset();
}

void websocket_eventloop::run()
{
    while (true)
    {
        try
        {
            this->context.run();
            return;
        }
        catch (const std::exception &ex)
        {
            g_RipExt.LogError("WebSocket event loop crashed: %s", ex.what());
        }
        catch (...)
        {
            g_RipExt.LogError("WebSocket event loop crashed with unknown exception");
        }
    }
}

boost::asio::io_context &websocket_eventloop::get_context()
{
    return this->context;
}

boost::asio::ssl::context &websocket_eventloop::get_ssl_context()
{
    return this->ssl_ctx;
}
