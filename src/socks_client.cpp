#include <chrono>

#include "assertion.hpp"
#include "logging.hpp"
#include "network_parameters.hpp"
#include "socks_client.hpp"

namespace algo = boost::algorithm;
namespace asio = boost::asio;
namespace beast = boost::beast;

namespace ga {
namespace sdk {

    namespace {
        // TODO: URI parsing
        std::pair<std::string, uint16_t> split_url(const std::string& domain_name)
        {
            auto endpoint = domain_name;
            const bool use_tls = algo::starts_with(endpoint, "wss://") || algo::starts_with(endpoint, "https://");
            if (use_tls) {
                algo::erase_all(endpoint, "wss://");
                algo::erase_all(endpoint, "https://");
            } else {
                algo::erase_all(endpoint, "ws://");
            }
            std::vector<std::string> endpoint_parts;
            algo::split(endpoint_parts, endpoint, algo::is_any_of("/"));
            GDK_RUNTIME_ASSERT(endpoint_parts.size() > 0);
            std::vector<std::string> host_parts;
            algo::split(host_parts, endpoint_parts[0], algo::is_any_of(":"));
            GDK_RUNTIME_ASSERT(host_parts.size() > 0);
            const uint16_t port = host_parts.size() > 1 ? std::stoul(host_parts[1], nullptr, 10) : use_tls ? 443 : 80;
            return { host_parts[0], htons(port) };
        }
    } // namespace

    socks_client::socks_client(asio::io_context& io, boost::beast::tcp_stream& stream)
        : m_resolver(asio::make_strand(io))
        , m_stream(stream)
    {
    }

    std::future<void> socks_client::run(const std::string& endpoint, const std::string& proxy_uri)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:run");

        m_endpoint = endpoint;

        std::string proxy = algo::trim_copy(proxy_uri);
        GDK_RUNTIME_ASSERT(algo::starts_with(proxy, "socks5://"));
        algo::erase_all(proxy, "socks5://");

        std::vector<std::string> proxy_parts;
        algo::split(proxy_parts, proxy, algo::is_any_of(":"));
        GDK_RUNTIME_ASSERT(proxy_parts.size() == 2);

        const auto host = proxy_parts[0];
        const auto port = proxy_parts[1];

        m_resolver.async_resolve(host, port, beast::bind_front_handler(&socks_client::on_resolve, shared_from_this()));

        return m_promise.get_future();
    }

    void socks_client::shutdown()
    {
        GDK_LOG_NAMED_SCOPE("socks_client:run");

        beast::error_code ec;
        m_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        if (ec && ec != beast::errc::not_connected) {
            GDK_RUNTIME_ASSERT(false);
        }
    }

    void socks_client::on_resolve(beast::error_code ec, asio::ip::tcp::resolver::results_type results)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:on_resolve");

        NET_ERROR_CODE_CHECK("socks_client", ec);
        m_stream.async_connect(results, beast::bind_front_handler(&socks_client::on_connect, shared_from_this()));
    }

    void socks_client::on_connect(beast::error_code ec, asio::ip::tcp::resolver::results_type::endpoint_type)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:on_connect");

        NET_ERROR_CODE_CHECK("socks_client", ec);

        m_negotiation_phase = negotiation_phase::method_selection;

        asio::async_write(m_stream, method_selection_request(),
            beast::bind_front_handler(&socks_client::on_write, shared_from_this()));
    }

    void socks_client::on_write(boost::beast::error_code ec, size_t __attribute__((unused)) bytes_transferred)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:on_write");

        NET_ERROR_CODE_CHECK("socks_client", ec);

        m_response.resize(m_negotiation_phase == negotiation_phase::method_selection ? 2 : 4);
        asio::async_read(
            m_stream, asio::buffer(m_response), beast::bind_front_handler(&socks_client::on_read, shared_from_this()));
    }

    void socks_client::on_read(boost::beast::error_code ec, size_t __attribute__((unused)) bytes_transferred)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:on_read");

        NET_ERROR_CODE_CHECK("socks_client", ec);

        if (m_negotiation_phase == negotiation_phase::method_selection) {
            m_negotiation_phase = negotiation_phase::connect;

            asio::async_write(m_stream, connect_request(m_endpoint),
                beast::bind_front_handler(&socks_client::on_write, shared_from_this()));
        } else {
            GDK_RUNTIME_ASSERT(m_negotiation_phase == negotiation_phase::connect && m_response[1] == 0);

            const size_t response_siz
                = m_response[3] == 0x1 ? 4 + sizeof(uint16_t) : m_response[3] == 0x4 ? 16 + sizeof(uint16_t) : 1;
            m_response.resize(response_siz);

            asio::async_read(m_stream, asio::buffer(m_response),
                beast::bind_front_handler(&socks_client::on_connect_read, shared_from_this()));
        }
    }

    void socks_client::on_connect_read(boost::beast::error_code ec, size_t __attribute__((unused)) bytes_transferred)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:on_connect_read");

        NET_ERROR_CODE_CHECK("socks_client", ec);
        GDK_RUNTIME_ASSERT(m_negotiation_phase == negotiation_phase::connect);

        if (m_response.size() == 1) {
            m_response.resize(m_response[0] + sizeof(uint16_t));
            asio::async_read(m_stream, asio::buffer(m_response),
                beast::bind_front_handler(&socks_client::on_domain_name_read, shared_from_this()));
        } else {
            m_promise.set_value();
        }
    }

    void socks_client::on_domain_name_read(
        boost::beast::error_code ec, size_t __attribute__((unused)) bytes_transferred)
    {
        GDK_LOG_NAMED_SCOPE("socks_client:on_domain_name_read");

        NET_ERROR_CODE_CHECK("socks_client", ec);

        m_promise.set_value();
    }

    asio::const_buffer socks_client::method_selection_request()
    {
        // version: 5
        // methods: 1
        // authentication: no authentication
        m_request.assign({ 0x5, 0x1, 0x0 });
        return asio::const_buffer(m_request.data(), m_request.size());
    }

    asio::const_buffer socks_client::connect_request(const std::string& domain_name)
    {
        GDK_RUNTIME_ASSERT(!domain_name.empty());

        const auto host_port = split_url(domain_name);

        // version: 5
        // command: connect
        // reserved
        // address type domain name: 3
        // address size
        m_request.assign({ 0x5, 0x1, 0x0, 0x3, static_cast<unsigned char>(host_port.first.size()) });
        std::copy(std::cbegin(host_port.first), std::cend(host_port.first), std::back_inserter(m_request));

        const auto p = reinterpret_cast<const unsigned char*>(&host_port.second);
        std::copy(p, p + sizeof(uint16_t), std::back_inserter(m_request));

        return asio::const_buffer(m_request.data(), m_request.size());
    }

    void socks_client::set_exception(const std::string& what)
    {
        m_promise.set_exception(std::make_exception_ptr(std::runtime_error(what)));
    }

} // namespace sdk
} // namespace ga
