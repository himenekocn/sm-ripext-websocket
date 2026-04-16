#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio.hpp>
#include <memory>
#include "extension.h"
#include <map>
#include <atomic>
#include <mutex>

#if defined WIN32
#include <sdkddkver.h>
#endif

namespace websocket = boost::beast::websocket;
namespace beast = boost::beast;
using tcp = boost::asio::ip::tcp;

// Forward declaration
class websocket_connection_base;
using websocket_connection_ptr = std::shared_ptr<websocket_connection_base>;

class websocket_connection_base : public std::enable_shared_from_this<websocket_connection_base>
{
public:
    websocket_connection_base(std::string address, std::string endpoint, uint16_t port);
    virtual ~websocket_connection_base() = default;
    void set_write_callback(std::function<void(size_t)> callback);
    void set_read_callback(std::function<void(uint8_t *, size_t)> callback);
    void set_connect_callback(std::function<void()> callback);
    void set_disconnect_callback(std::function<void()> callback);
    void set_header(std::string key, std::string value);
    void add_headers(websocket::request_type &req);
    void destroy();
    bool ws_open();
    bool is_destroyed() const { return destroyed.load(); }

    virtual void close() = 0;
    virtual void connect() = 0;
    virtual void write(std::string message) = 0;
    virtual bool socket_open() = 0;
    virtual void cancel() = 0;

protected:
    void begin_async();
    void end_async();
    void notify_disconnect();
    void maybe_delete();
    
    // Helper to create a weak_ptr for callback safety
    std::weak_ptr<websocket_connection_base> weak_from_this() { return shared_from_this(); }

    std::unique_ptr<std::function<void(uint8_t *, size_t)>> read_callback;
    std::unique_ptr<std::function<void(size_t)>> write_callback;
    std::unique_ptr<std::function<void()>> connect_callback;
    std::unique_ptr<std::function<void()>> disconnect_callback;
    std::map<std::string, std::string> headers;
    std::mutex header_mutex;
    beast::flat_buffer buffer;
    std::string address;
    std::string endpoint;
    uint16_t port;
    std::atomic<bool> pending_delete{false};
    std::atomic<bool> ws_connect{false};
    std::atomic<bool> disconnect_notified{false};
    std::atomic<uint32_t> async_ops{0};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> close_in_progress{false};
};
