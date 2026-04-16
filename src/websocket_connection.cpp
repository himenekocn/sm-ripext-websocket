#include "websocket_connection.h"
#include "websocket_eventloop.h"
#include <boost/asio/strand.hpp>

websocket_connection::websocket_connection(std::string address, std::string endpoint, uint16_t port) : websocket_connection_base(address, endpoint, port)
{
    this->ws = std::make_unique<websocket::stream<beast::tcp_stream>>(boost::asio::make_strand(event_loop.get_context()));
    this->work = std::make_unique<boost::asio::io_context::work>(event_loop.get_context());
    this->resolver = std::make_shared<tcp::resolver>(event_loop.get_context());
}

void websocket_connection::connect()
{
    bool resolve_started = false;

    try
    {
        // Check if already destroyed
        if (this->is_destroyed())
        {
            g_RipExt.LogError("WebSocket connect called on destroyed connection");
            return;
        }

        this->disconnect_notified.store(false);
        this->ws_connect.store(false);
        this->close_in_progress.store(false);

        char s_port[8];
        std::snprintf(s_port, sizeof(s_port), "%hu", this->port);
        tcp::resolver::query query(this->address.c_str(), s_port);

        this->begin_async();
        resolve_started = true;
        
        // Use shared_from_this() to ensure object stays alive during async operation
        auto self = shared_from_this();
        this->resolver->async_resolve(query, 
            [self](beast::error_code ec, tcp::resolver::results_type results)
            {
                std::static_pointer_cast<websocket_connection>(self)->on_resolve(ec, results);
            });
        g_RipExt.LogMessage("Init Connect %s:%d", address.c_str(), this->port);
    }
    catch (const std::exception &ex)
    {
        if (resolve_started)
        {
            this->end_async();
        }
        g_RipExt.LogError("WebSocket connect setup error for %s:%d: %s", this->address.c_str(), this->port, ex.what());
        this->notify_disconnect();
    }
}

void websocket_connection::on_resolve(beast::error_code ec, tcp::resolver::results_type results)
{
    const auto guard = std::unique_ptr<void, std::function<void(void *)>>(nullptr, [this](void *)
                                                                          { this->end_async(); });

    // Check if connection is being destroyed
    if (this->pending_delete.load() || this->is_destroyed())
    {
        return;
    }

    if (ec)
    {
        g_RipExt.LogError("Error resolving %s: %d %s", this->address.c_str(), ec.value(), ec.message().c_str());
        this->notify_disconnect();
        this->ws_connect.store(false);
        return;
    }

    beast::get_lowest_layer(*this->ws).expires_after(std::chrono::seconds(30));
    this->begin_async();
    
    auto self = shared_from_this();
    beast::get_lowest_layer(*this->ws).async_connect(results, 
        [self](beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
        {
            std::static_pointer_cast<websocket_connection>(self)->on_connect(ec, ep);
        });
}

void websocket_connection::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
{
    const auto guard = std::unique_ptr<void, std::function<void(void *)>>(nullptr, [this](void *)
                                                                          { this->end_async(); });

    if (this->pending_delete.load() || this->is_destroyed())
    {
        return;
    }

    if (ec)
    {
        g_RipExt.LogError("Error connecting to %s: %d %s", this->address.c_str(), ec.value(), ec.message().c_str());
        this->notify_disconnect();
        this->ws_connect.store(false);
        return;
    }
    beast::get_lowest_layer(*this->ws).expires_never();

    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.keep_alive_pings = true;
    this->ws->set_option(websocket::stream_base::decorator([this](websocket::request_type &req)
                                                           { this->add_headers(req); }));

    this->begin_async();
    
    auto self = shared_from_this();
    this->ws->async_handshake(this->address, this->endpoint.c_str(), 
        [self](beast::error_code ec)
        {
            std::static_pointer_cast<websocket_connection>(self)->on_handshake(ec);
        });
}

void websocket_connection::on_handshake(beast::error_code ec)
{
    const auto guard = std::unique_ptr<void, std::function<void(void *)>>(nullptr, [this](void *)
                                                                          { this->end_async(); });

    if (this->pending_delete.load() || this->is_destroyed())
    {
        return;
    }

    if (ec)
    {
        g_RipExt.LogError("WebSocket Handshake Error: %d %s", ec.value(), ec.message().c_str());
        this->notify_disconnect();
        this->ws_connect.store(false);
        return;
    }

    this->buffer.clear();
    if (this->connect_callback)
    {
        this->connect_callback->operator()();
    }

    this->begin_async();
    
    auto self = shared_from_this();
    this->ws->async_read(this->buffer, 
        [self](beast::error_code ec, size_t bytes_transferred)
        {
            std::static_pointer_cast<websocket_connection>(self)->on_read(ec, bytes_transferred);
        });
    this->ws_connect.store(true);
    g_RipExt.LogMessage("On Handshaked %s:%d", address.c_str(), this->port);
}

void websocket_connection::on_write(beast::error_code ec, size_t bytes_transferred)
{
    const auto guard = std::unique_ptr<void, std::function<void(void *)>>(nullptr, [this](void *)
                                                                          { this->end_async(); });

    if (this->is_destroyed())
    {
        return;
    }

    if (ec)
    {
        g_RipExt.LogError("WebSocket write error: %d %s", ec.value(), ec.message().c_str());
        // FIX: Notify disconnect on write error
        this->notify_disconnect();
        this->ws_connect.store(false);
        return;
    }
}

void websocket_connection::on_read(beast::error_code ec, size_t bytes_transferred)
{
    const auto guard = std::unique_ptr<void, std::function<void(void *)>>(nullptr, [this](void *)
                                                                          { this->end_async(); });

    if (this->is_destroyed())
    {
        return;
    }

    if (ec)
    {
        if (!this->pending_delete.load())
        {
            g_RipExt.LogError("WebSocket read error: %d %s", ec.value(), ec.message().c_str());
            this->notify_disconnect();
            this->ws_connect.store(false);
        }
        return;
    }

    if (this->read_callback)
    {
        // FIX: Check malloc return value
        auto buffer = reinterpret_cast<uint8_t *>(malloc(bytes_transferred));
        if (!buffer)
        {
            g_RipExt.LogError("WebSocket read: failed to allocate buffer of size %zu", bytes_transferred);
            this->notify_disconnect();
            this->ws_connect.store(false);
            return;
        }
        memcpy(buffer, reinterpret_cast<const uint8_t *>(this->buffer.data().data()), bytes_transferred);

        this->read_callback->operator()(buffer, bytes_transferred);
    }
    this->buffer.consume(bytes_transferred);

    this->begin_async();
    
    auto self = shared_from_this();
    this->ws->async_read(this->buffer, 
        [self](beast::error_code ec, size_t bytes_transferred)
        {
            std::static_pointer_cast<websocket_connection>(self)->on_read(ec, bytes_transferred);
        });
}

void websocket_connection::on_close(beast::error_code ec)
{
    const auto guard = std::unique_ptr<void, std::function<void(void *)>>(nullptr, [this](void *)
                                                                          { this->end_async(); });

    if (ec)
    {
        g_RipExt.LogError("WebSocket close error: %d %s", ec.value(), ec.message().c_str());
    }
    this->ws_connect.store(false);
    this->close_in_progress.store(false);
}

void websocket_connection::write(std::string message)
{
    try
    {
        // FIX: Check connection state before writing
        if (this->is_destroyed() || !this->ws_connect.load() || this->pending_delete.load())
        {
            g_RipExt.LogError("WebSocket write: connection not ready or destroyed");
            return;
        }

        auto payload = std::make_shared<std::string>(std::move(message));
        this->begin_async();
        
        auto self = shared_from_this();
        this->ws->async_write(boost::asio::buffer(*payload), 
            [self, payload](beast::error_code ec, size_t bytes_transferred)
            {
                std::static_pointer_cast<websocket_connection>(self)->on_write(ec, bytes_transferred);
            });
    }
    catch (const std::exception &ex)
    {
        this->end_async();
        g_RipExt.LogError("WebSocket write setup error for %s:%d: %s", this->address.c_str(), this->port, ex.what());
    }
}

void websocket_connection::close()
{
    this->cancel();
}

bool websocket_connection::socket_open()
{
    return this->ws->is_open() && !this->is_destroyed();
}

void websocket_connection::cancel()
{
    try
    {
        beast::error_code ec;
        this->resolver->cancel();

        auto &stream = beast::get_lowest_layer(*this->ws);
        
        // FIX: Check if close is already in progress to avoid double-close issues
        if (this->close_in_progress.exchange(true))
        {
            // Close already in progress, just cancel pending operations
            stream.cancel();
            return;
        }
        
        // Cancel all pending async operations first
        stream.cancel();

        if (this->ws->is_open())
        {
            this->begin_async();
            
            auto self = shared_from_this();
            this->ws->async_close(websocket::close_code::normal, 
                [self](beast::error_code ec)
                {
                    std::static_pointer_cast<websocket_connection>(self)->on_close(ec);
                });
            return;
        }

        if (stream.socket().is_open())
        {
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            stream.socket().close(ec);
        }
    }
    catch (const std::exception &ex)
    {
        g_RipExt.LogError("WebSocket cancel error for %s:%d: %s", this->address.c_str(), this->port, ex.what());
    }
}
