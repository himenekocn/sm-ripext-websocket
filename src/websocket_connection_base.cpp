#include "websocket_connection_base.h"

websocket_connection_base::websocket_connection_base(std::string address, std::string endpoint, uint16_t port)
    : address(std::move(address)), endpoint(std::move(endpoint)), port(port)
{
}

void websocket_connection_base::set_write_callback(std::function<void(size_t)> callback)
{
    this->write_callback = std::make_unique<std::function<void(size_t)>>(callback);
}

void websocket_connection_base::set_read_callback(std::function<void(uint8_t *, size_t)> callback)
{
    this->read_callback = std::make_unique<std::function<void(uint8_t *, size_t)>>(callback);
}

void websocket_connection_base::set_connect_callback(std::function<void()> callback)
{
    this->connect_callback = std::make_unique<std::function<void()>>(callback);
}

void websocket_connection_base::set_disconnect_callback(std::function<void()> callback)
{
    this->disconnect_callback = std::make_unique<std::function<void()>>(callback);
}

void websocket_connection_base::set_header(std::string header, std::string value)
{
    std::lock_guard<std::mutex> guard(this->header_mutex);
    this->headers.insert_or_assign(header, value);
}

void websocket_connection_base::add_headers(websocket::request_type &req)
{
    req.set(beast::http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " SourceMod-WebSockets v" + SMEXT_CONF_VERSION);
    std::lock_guard<std::mutex> guard(this->header_mutex);
    for (std::pair<std::string, std::string> elem : this->headers)
    {
        req.set(elem.first, elem.second);
    }
}

void websocket_connection_base::destroy()
{
    // Set pending_delete first, then cancel operations
    // The atomic operations ensure proper sequencing
    this->pending_delete.store(true);
    this->cancel();
    // maybe_delete will be called by end_async when async_ops reaches 0
    // or we can call it here as a fallback
    this->maybe_delete();
}

bool websocket_connection_base::ws_open()
{
    return this->ws_connect.load() && !this->destroyed.load();
}

void websocket_connection_base::begin_async()
{
    this->async_ops.fetch_add(1, std::memory_order_acq_rel);
}

void websocket_connection_base::end_async()
{
    // Use memory_order_acq_rel to ensure proper synchronization
    if (this->async_ops.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        this->maybe_delete();
    }
}

void websocket_connection_base::notify_disconnect()
{
    if (!this->disconnect_callback || this->disconnect_notified.exchange(true))
    {
        return;
    }

    this->disconnect_callback->operator()();
}

void websocket_connection_base::maybe_delete()
{
    // Double-check pattern to avoid race conditions
    // Both conditions must be true for deletion
    if (this->pending_delete.load(std::memory_order_acquire) && 
        this->async_ops.load(std::memory_order_acquire) == 0)
    {
        // Mark as destroyed before actual deletion to prevent any new operations
        this->destroyed.store(true, std::memory_order_release);
        // Note: This is called from shared_ptr context, so we don't delete directly
        // The shared_ptr will handle cleanup when all references are gone
    }
}
