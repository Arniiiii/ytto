

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/fields_fwd.hpp>
#include <boost/beast/http/message_fwd.hpp>
#include <boost/beast/http/string_body_fwd.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/date_time.hpp>
#include <boost/process.hpp>
#include <boost/process/v2/shell.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/range/iterator_range_core.hpp>
#include <boost/stacktrace.hpp>
#include <boost/url.hpp>
#include <corral/Nursery.h>
#include <corral/Semaphore.h>
#include <corral/asio.h>
#include <corral/corral.h>
#include <corral/detail/asio.h>
#include <corral/run.h>
#include <corral/wait.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/os.h>
#include <fmt/ostream.h>
#include <fmt/printf.h>
#include <fmt/std.h>
#include <glaze/glaze.hpp>
#include <inja/environment.hpp>
#include <inja/inja.hpp>
#include <inja/json.hpp>
#include <magic_enum/magic_enum.hpp>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/core/LogLevel.h>
#include <quill/sinks/FileSink.h>
#include <re2/re2.h>

#include "ytto/boost_stacktrace_format.hpp"
#include "ytto/cache.hpp"
#include "ytto/cache_file.hpp"
#include "ytto/ollama_parser.hpp"
#include "ytto/omega_exception.hpp"

template <typename T> struct Debug;

quill::Logger* logger;

constexpr auto HTTP_MAX_TIME_TIMEOUT_RFC = std::chrono::seconds(120);
constexpr auto MAX_PROMPT_TIME = std::chrono::minutes(10);
constexpr int HTTP_VERSION_TO_USE = 11;
constexpr size_t MAX_EXPECTED_CHARACTERS = 128000;
constexpr uint16_t SERVER_DEFAULT_PORT = 8000;
constexpr size_t MAX_CONCURRENT_YTDLP_DEFAULT = 5;
constexpr size_t MAX_CONCURRENT_OLLAMA_DEFAULT = 6;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;

struct Config
{
    std::string language;
    std::string prompt_template;
    std::string http_body_template;
    boost::url url;
    beast::http::verb method;
    beast::http::fields headers;
    std::filesystem::path cache_file;
    std::filesystem::path cache_subtitles_file;
    std::filesystem::path log_file;
    quill::LogLevel log_level;
    size_t concurrency_yt_dlp{};
    size_t concurrency_ollama{};
    uint16_t server_port{};
    bool proceed_with_shorts{};
    bool enable_server{};
};

struct EntryData
{
    std::string link_str;
    std::string author;
    std::string title;
    std::string description;
};

struct RequestServer
{
    std::string url;
};
// NOLINTNEXTLINE(performance-enum-size)
enum class ReturnCodes : int
{
    Success = 0,
    FailDuringParsingCmdValues = 1,
    FailSpecifiedValueIsIncorrect = 2,
    FailDuringInitializationConfig = 3,
    FailCacheFolder = 4,
    FailStandardException = 5,
    FailParsePromptResult = 6,
    FailInitializationLogger = 7,

};

namespace
{

    corral::Task<std::string> get_subtitles(auto& ioc, std::string const& link,
                                            Config const& cfg)
    {
        net::readable_pipe rp{ioc};
        net::readable_pipe rp_err{ioc};
        boost::process::shell cmd_get_subtitles = boost::process::shell(
            R"(yt-dlp -q --no-progress --no-warnings --skip-download --write-subs --write-auto-subs  --sub-lang )"
            + cfg.language
            + R"( --convert-subs vtt --exec before_dl:"cat %(requested_subtitles.:.filepath)#q | sed -e '/^[0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9][0-9] --> [0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9][0-9]/d' -e '/^[[:digit:]]\{1,3\}\$/d' -e 's/<[^>]*>//g' -e '/^[[:space:]]*$/d' -e '1,3d' -e \"s/'/\\\\'/g\" -e 's/\"/\\\"/g' | sed -z 's/\n/ /g' && rm %(requested_subtitles.:.filepath)#q " ')"
            + link + "'");
        auto exe = cmd_get_subtitles.exe();
        auto proc = boost::process::process(
            ioc, exe, cmd_get_subtitles.args(),
            boost::process::process_stdio{
                .in = {/* in to default */}, .out = rp, .err = rp_err});

        auto read_loop = [](net::readable_pipe& p) -> corral::Task<std::string>
            {
                std::string res;
                std::array<char, 4096> buf;
                for (;;)
                    {
                        auto [error_code, received_size]
                            = co_await p.async_read_some(
                                net::buffer(buf),
                                corral::asio_nothrow_awaitable);
                        if (received_size)
                            {
                                res.append(buf.data(), received_size);
                            }
                        if (error_code)
                            {
                                co_return res;
                            }
                    }
            };

        LOG_INFO(logger, "Called yt-dlp for {} with {} language", link,
                 cfg.language);
        auto [ec_proc, subtitles_received, std_err_of_the_process]
            = co_await corral::allOf(proc.async_wait(corral::asio_awaitable),
                                     read_loop(rp), read_loop(rp_err));

        if (not std_err_of_the_process.empty())
            {
                throw std::logic_error(fmt::format(
                    "Failed to do yt-dlp: ec: {}\nstderr: {}\nstdout: {}",
                    ec_proc, std_err_of_the_process, subtitles_received));
            }

        co_return subtitles_received;
    }

    corral::Task<std::string> typical_http_request(
        auto& ioc, std::string const& request_body, const boost::url& url,
        beast::http::verb method, beast::http::fields headers)
    {
        auto resolver = net::ip::tcp::resolver{ioc};

        LOG_DEBUG(logger,
                  "DNS look-up of an URL... "
                  "host: {} port:{}",
                  std::string(url.host()), std::string(url.port()));
        net::ip::basic_resolver_results<boost::asio::ip::tcp> const results
            = co_await resolver.async_resolve(url.host(), url.port(),
                                              corral::asio_awaitable);

        auto stream = beast::tcp_stream{ioc};
        stream.expires_after(std::chrono::seconds(HTTP_MAX_TIME_TIMEOUT_RFC));

        LOG_DEBUG(logger, "Trying to connect to an URL...");
        co_await stream.async_connect(results, corral::asio_awaitable);
        LOG_DEBUG(logger, "Successfully.");

        LOG_DEBUG(logger,
                  "Creating a request object... "
                  "method: {} path: {} http version:{}",
                  magic_enum::enum_name(method),
                  std::string(url.encoded_resource()), HTTP_VERSION_TO_USE);
        beast::http::request<http::string_body> request{
            method, url.encoded_resource(), HTTP_VERSION_TO_USE, request_body,
            headers};
        request.set(beast::http::field::host, url.host());
        request.set(beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        request.prepare_payload();
        std::stringstream strs;
        strs << request;
        LOG_TRACE_L1(logger, "Request:\n{}", strs.str());
        stream.expires_after(MAX_PROMPT_TIME);

        LOG_INFO(logger, "Sending request to an LLM...");
        co_await beast::http::async_write(stream, request,
                                          corral::asio_awaitable);

        beast::flat_buffer buffer(MAX_EXPECTED_CHARACTERS);

        beast::http::response<http::string_body> response;

        LOG_INFO(logger, "Waiting for response...");
        co_await beast::http::async_read(stream, buffer, response,
                                         corral::asio_awaitable);
        LOG_INFO(logger, "Received response.");

        LOG_DEBUG(logger, "Trying to close connection.");

        beast::error_code error_code;
        // NOLINTBEGIN(bugprone-unused-return-value,
        // cert-err33-c)
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both,
                                 error_code);
        // NOLINTEND(bugprone-unused-return-value,
        // cert-err33-c)

        // not_connected happens sometimes
        // so don't bother reporting it.
        //
        if (error_code && error_code != beast::errc::not_connected)
            {
                throw boost::system::system_error(error_code, "shutdown");
            }
        LOG_DEBUG(logger, "Supposedly closed connection.");

        if (response.result() != http::status::ok)
            {
                throw std::logic_error("returned with status not 200");
            }

        co_return response.body();
    }

    corral::Task<std::string> typical_https_request(
        auto& ioc, std::string const& request_body, boost::url const& url,
        beast::http::verb method, const beast::http::fields& headers)
    {
        net::ssl::context sslCtx(boost::asio::ssl::context::tlsv13);

        // This holds the root certificate used for verification
        // load_root_certificates(sslCtx);

        // Verify the remote server's certificate
        sslCtx.set_verify_mode(net::ssl::verify_peer);
        sslCtx.set_default_verify_paths();

        auto resolver = net::ip::tcp::resolver{ioc};
        net::ssl::stream<beast::tcp_stream> stream(ioc, sslCtx);
        //
        // Set the expected hostname in the peer certificate for verification
        stream.set_verify_callback(
            ssl::host_name_verification(url.host_name()));

        LOG_DEBUG(logger,
                  "DNS look-up of an URL... "
                  "host: {} port:{}",
                  std::string(url.host()), std::string(url.port()));
        net::ip::basic_resolver_results<boost::asio::ip::tcp> const results
            = co_await resolver.async_resolve(
                url.host(), url.port() == "" ? "443" : url.port(),
                corral::asio_awaitable);

        if (!SSL_set_tlsext_host_name(stream.native_handle(),
                                      url.host_name().c_str()))
            {
                throw beast::system_error(static_cast<int>(::ERR_get_error()),
                                          net::error::get_ssl_category());
            }

        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(HTTP_MAX_TIME_TIMEOUT_RFC));

        LOG_DEBUG(logger, "Trying to connect to...");
        co_await beast::get_lowest_layer(stream).async_connect(
            results, corral::asio_awaitable);
        LOG_DEBUG(logger, "Successfully.");

        // DO SSL HANDSHAKE
        LOG_DEBUG(logger, "Trying to do SSL handshake...");
        co_await stream.async_handshake(ssl::stream_base::client,
                                        corral::asio_awaitable);
        LOG_DEBUG(logger, "Successfully.");

        LOG_DEBUG(logger,
                  "Creating a request object... "
                  "method: {} path: {} http version:{}",
                  magic_enum::enum_name(method),
                  std::string(url.encoded_resource()), HTTP_VERSION_TO_USE);
        beast::http::request<http::string_body> request{
            method, url.encoded_resource(), HTTP_VERSION_TO_USE, request_body,
            headers};
        request.set(beast::http::field::host, url.host());
        request.set(beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        request.prepare_payload();
        std::stringstream strs;
        strs << request;
        LOG_TRACE_L1(logger, "Request:\n{}", strs.str());
        beast::get_lowest_layer(stream).expires_after(MAX_PROMPT_TIME);

        LOG_INFO(logger, "Sending request to an LLM...");
        co_await beast::http::async_write(stream, request,
                                          corral::asio_awaitable);

        beast::flat_buffer buffer(MAX_EXPECTED_CHARACTERS);

        beast::http::response<http::string_body> response;

        LOG_INFO(logger, "Waiting for response...");
        co_await beast::http::async_read(stream, buffer, response,
                                         corral::asio_awaitable);
        LOG_INFO(logger, "Received response.");

        LOG_DEBUG(logger, "Trying to close connection.");

        beast::error_code error_code;
        // NOLINTBEGIN(bugprone-unused-return-value,
        // cert-err33-c)
        auto ec
            = co_await stream.async_shutdown(corral::asio_nothrow_awaitable);
        // beast::get_lowest_layer (stream).socket ().shutdown (
        //     net::ip::tcp::socket::shutdown_both, error_code);
        // NOLINTEND(bugprone-unused-return-value,
        // cert-err33-c)

        // not_connected happens sometimes
        // so don't bother reporting it.

        if (ec && ec != net::ssl::error::stream_truncated)
            {
                throw boost::system::system_error(ec, "shutdown");
            }

        LOG_DEBUG(logger, "Supposedly closed connection.");

        if (response.result() != http::status::ok)
            {
                throw boost::system::system_error(
                    ec, "returned with status not 200");
            }

        co_return response.body();
    }

    corral::Task<std::string> request_to_LLM(auto& ioc,
                                             std::string& request_body,
                                             Config const& cfg)
    {
        if ("https" == cfg.url.scheme())
            {
                co_return co_await typical_https_request(
                    ioc, request_body, cfg.url, cfg.method, cfg.headers);
            }
        else
            {
                co_return co_await typical_http_request(
                    ioc, request_body, cfg.url, cfg.method, cfg.headers);
            }
    }

    corral::Task<std::string> summarize(corral::Semaphore& semaphore_yt_dlp,
                                        corral::Semaphore& semaphore_ollama,
                                        std::string const& link_str,
                                        inja::json& data, auto& ioc,
                                        ABCCache& cache,
                                        ABCCache& cache_subtitles,
                                        Config const& cfg)
    {
        LOG_INFO(logger, "Checking cache...");
        std::optional<std::string> possible_res = cache.get(link_str);
        if (possible_res.has_value())
            {
                LOG_INFO(logger, "Found result in cache.");
                co_return *possible_res;
            }
        std::string summary;
        LOG_INFO(logger, "Not found in cache.");

        std::optional<std::string> maybe_subtitles
            = cache_subtitles.get(link_str);
        std::string subtitles;
        if (maybe_subtitles.has_value())
            {
                subtitles = *maybe_subtitles;
            }
        else
            {
                std::string subtitles_received;

                {
                    auto lock = co_await semaphore_yt_dlp.lock();
                    subtitles_received
                        = co_await get_subtitles(ioc, link_str, cfg);
                }

                LOG_INFO(logger, "Received subtitles!");
                LOG_TRACE_L1(logger, "Received subtitles: {}",
                             subtitles_received);
                subtitles = std::move(subtitles_received);
                LOG_INFO(logger,
                         "Saving received subtitles to "
                         "subtitles's cache...");
                cache_subtitles.set(link_str, subtitles);
                LOG_INFO(logger,
                         "Saved received subtitles to "
                         "subtitles's cache.");
            }

        data["subtitles"] = subtitles;
        std::string prompt = inja::render(cfg.prompt_template, data);

        inja::json data_prompt;
        boost::algorithm::replace_all(prompt, "\n", R"(\n)");
        boost::algorithm::replace_all(prompt, "\"", R"(\")");
        data_prompt["prompt"] = prompt;
        std::string request_body
            = inja::render(cfg.http_body_template, data_prompt);

        std::string LLM_res;

        {
            auto lock = co_await semaphore_ollama.lock();
            LLM_res = co_await request_to_LLM(ioc, request_body, cfg);
        }

        OllamaParser parser;

        summary = parser.getResponse(LLM_res);

        LOG_TRACE_L1(logger, "Received response:{}", LLM_res);
        LOG_DEBUG(logger, "Saving response to cache");

        cache.set(link_str, summary);

        co_return summary;
    }

    corral::Task<std::string> main_logic(auto& ioc,
                                         boost::property_tree::ptree tree,
                                         ABCCache& cache,
                                         ABCCache& cache_subtitles,
                                         Config const& cfg,
                                         corral::Semaphore& semaphore_yt_dlp,
                                         corral::Semaphore& semaphore_ollama)
    {
        CORRAL_WITH_NURSERY(nursery)
        {
            for (auto& xml_entry : tree.get_child("feed"))
                {
                    if ("entry" != xml_entry.first)
                        {
                            continue;
                        }
                    auto const& link
                        = xml_entry.second.get_child("link.<xmlattr>.href");
                    std::string link_str = link.data();
                    LOG_INFO(logger,
                             "Got link to a YouTube video, maybe... Here's "
                             "the link: {}",
                             link_str);
                    if (not cfg.proceed_with_shorts
                        and link_str.contains("shorts"))
                        {
                            LOG_INFO(logger,
                                     "This is a link to a short. Skipping.");
                            continue;
                        }

                    auto const& author
                        = xml_entry.second.get_child("author.name");
                    auto const& title
                        = xml_entry.second.get_child("media:group.media:title");
                    auto& description = xml_entry.second.get_child(
                        "media:group.media:description");

                    nursery.start(
                        [&, link_str = link_str, author = std::cref(author),
                         title = std::cref(title),
                         description = std::ref(
                             description)]() mutable -> corral::Task<void>
                            {
                                inja::json data;
                                data["author"] = author.get().data();
                                data["title"] = title.get().data();
                                data["description"] = description.get().data();
                                data["link"] = link_str;
                                std::string summary = co_await summarize(
                                    semaphore_yt_dlp, semaphore_ollama,
                                    link_str, data, ioc, cache, cache_subtitles,
                                    cfg);

                                LOG_INFO(logger,
                                         "Appending LLM's result to "
                                         "entry's description...");
                                std::string new_description = fmt::format(
                                    "{}\n\nLLM's result:\n{}",
                                    description.get().data(), summary);
                                description.get().put("", new_description);
                                LOG_INFO(logger,
                                         "Successfully appended, I guess...");
                            });
                }
            co_return corral::join;
        };
        LOG_INFO(logger, "Writing result to stdout...");
        std::stringstream strs;
        boost::property_tree::write_xml(strs, tree);
        LOG_INFO(logger, "Wrote result to stdout.");
        co_return strs.str();
    }

    boost::property_tree::ptree parse_rss_into_tree(std::string const& rss_feed)
    {
        LOG_DEBUG(logger, "Received something from stdin...");
        LOG_TRACE_L1(logger, "rss_feed: {}", rss_feed);
        boost::property_tree::ptree tree;
        std::istringstream istr(rss_feed);
        LOG_DEBUG(logger, "Trying to parse it as an XML...");
        boost::property_tree::read_xml(istr, tree);
        LOG_DEBUG(logger, "Successfully parsed an XML...");
        return tree;
    }

    corral::Task<void> async_main_no_server(auto& ioc, ABCCache& cache,
                                            ABCCache& cache_subtitles,
                                            Config const& cfg)
    {
        LOG_DEBUG(logger, "Waiting for YouTube's RSS feed from stdin...");
        std::cin >> std::noskipws;
        std::istreambuf_iterator<char> start(std::cin);
        std::istreambuf_iterator<char> end;
        std::string xml_rss_youtube_feed(start, end);
        LOG_DEBUG(logger, "Received the YouTube's RSS feed.");

        boost::property_tree::ptree tree
            = parse_rss_into_tree(xml_rss_youtube_feed);

        corral::Semaphore semaphore_yt_dlp(cfg.concurrency_yt_dlp);
        corral::Semaphore semaphore_ollama(cfg.concurrency_ollama);
        std::string res
            = co_await main_logic(ioc, tree, cache, cache_subtitles, cfg,
                                  semaphore_yt_dlp, semaphore_ollama);
        fmt::println("{}", res);
    }

    corral::Task<http::message_generator> handle_request(
        auto& ioc, auto&& req, ABCCache& cache, ABCCache& cache_subtitles,
        Config const& cfg, corral::Semaphore& semaphore_yt_dlp,
        corral::Semaphore& semaphore_ollama)
    {
        // Returns a bad request response
        auto const bad_request = [&req](beast::string_view why)
            {
                http::response<http::string_body> res{http::status::bad_request,
                                                      req.version()};
                res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
                res.set(http::field::content_type, "text/html");
                res.keep_alive(req.keep_alive());
                res.body() = std::string(why);
                res.prepare_payload();
                return res;
            };

        // // Returns a not found response
        // auto const not_found = [&req](beast::string_view target)
        //     {
        //         http::response<http::string_body>
        //         res{http::status::not_found,
        //                                               req.version()};
        //         res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        //         res.set(http::field::content_type, "text/html");
        //         res.keep_alive(req.keep_alive());
        //         res.body() = "The resource '" + std::string(target)
        //                      + "' was not found.";
        //         res.prepare_payload();
        //         return res;
        //     };

        // // Returns a server error response
        // auto const server_error = [&req](beast::string_view what)
        //     {
        //         http::response<http::string_body> res{
        //             http::status::internal_server_error, req.version()};
        //         res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        //         res.set(http::field::content_type, "text/html");
        //         res.keep_alive(req.keep_alive());
        //         res.body() = "An error occurred: '" + std::string(what) +
        //         "'"; res.prepare_payload(); return res;
        //     };

        // Make sure we can handle the method
        if (req.method() != http::verb::get)
            {
                // req.method() != http::verb::head)
                co_return bad_request("Unknown HTTP-method");
            }

        // Request path must be absolute and not contain "..".
        if (req.target().empty() || req.target()[0] != '/'
            || req.target().find("..") != beast::string_view::npos)
            {
                co_return bad_request("Illegal request-target");
            }

        auto res_json = glz::read_json<RequestServer>(req.body());
        if (!res_json)
            {
                co_return bad_request("Request is not correct.");
            }

        const auto& json = res_json.value();

        if (not RE2::FullMatch(
                json.url,
                R"(^https:\/\/www\.youtube\.com\/feeds\/videos\.xml\?channel_id=UC[a-zA-Z0-9_-]{22}$)"))
            {
                co_return bad_request(
                    "Provided URL does not look like an YouTube's URL to an "
                    "RSS "
                    "feed.");
            }

        boost::url url_youtube_rss_feed(json.url);

        std::string youtube_rss_feed = co_await typical_https_request(
            ioc, "", url_youtube_rss_feed, http::verb::get, http::fields{});

        std::string response_body = co_await main_logic(
            ioc, parse_rss_into_tree(youtube_rss_feed), cache, cache_subtitles,
            cfg, semaphore_yt_dlp, semaphore_ollama);

        http::response<http::string_body> res(http::status::ok, req.version());
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/xml");
        res.keep_alive(req.keep_alive());
        res.body() = response_body;
        res.prepare_payload();

        co_return res;
    }

    corral::Task<void> serve(auto& ioc, beast::tcp_stream stream,
                             ABCCache& cache, ABCCache& cache_subtitles,
                             Config const& cfg,
                             corral::Semaphore& semaphore_yt_dlp,
                             corral::Semaphore& semaphore_ollama)
    {
        beast::flat_buffer buffer;

        beast::http::request<http::string_body> req;
        co_await beast::http::async_read(stream, buffer, req,
                                         corral::asio_awaitable);

        auto response_generator = co_await handle_request(
            ioc, std::move(req), cache, cache_subtitles, cfg, semaphore_yt_dlp,
            semaphore_ollama);

        LOG_INFO(logger, "Sending response...");
        // Send the response
        co_await beast::async_write(stream, std::move(response_generator),
                                    corral::asio_awaitable);
    }

    corral::Task<void> server_acceptor(auto& ioc, ABCCache& cache,
                                       ABCCache& cache_subtitles,
                                       Config const& cfg)
    {
        corral::Semaphore semaphore_yt_dlp(cfg.concurrency_yt_dlp);
        corral::Semaphore semaphore_ollama(cfg.concurrency_ollama);

        net::ip::tcp::acceptor acceptor(
            ioc, net::ip::tcp::endpoint(boost::asio::ip::tcp::v4(),
                                        cfg.server_port));
        CORRAL_WITH_NURSERY(nursery)
        {
            while (true)
                {
                    beast::tcp_stream stream{
                        co_await acceptor.async_accept(corral::asio_awaitable)};

                    LOG_INFO(logger, "New connection");

                    nursery.start(
                        [&](beast::tcp_stream stream) mutable
                            {
                                return serve(ioc, std::move(stream), cache,
                                             cache_subtitles, cfg,
                                             semaphore_yt_dlp,
                                             semaphore_ollama);
                            },
                        std::move(stream));
                }
        };
    }

}  // namespace

int main(int argc, char* argv[])
{
    Config cfg;

    CLI::App app{
        "Post-processor for YouTube's RSS feed, so that you get "
        "summary of video inside the feed via sending an HTTP "
        "request to something like an Ollama instance."};

    // Temporary storage for CLI11 to map types it doesn't handle natively
    // without custom validators
    std::string url_str = "http://127.0.0.1:11434/api/chat";
    std::string method_str = "post";
    std::vector<std::string> headers_raw = {"Content-Type: application/json"};
    std::string log_level_str = "info";

    app.add_option("-c,--cache-folder", cfg.cache_file,
                   "Folder, in which there will be files as cache of result "
                   "of summarization")
        ->required();

    app.add_option("-S,--cache-folder-subtitles", cfg.cache_subtitles_file,
                   "Folder, in which there will be files as subtitles of a "
                   "specific YouTube link.")
        ->required();

    app.add_option("-L,--language", cfg.language,
                   "yt-dlp language of subtitles")
        ->capture_default_str()
        ->default_val("en");

    app.add_option(
           "-u,--url", url_str,
           "URL of ?Ollama? instance in format http://127.0.0.1:11434/api/chat")
        ->capture_default_str();

    app.add_option("-X,--method", method_str,
                   "HTTP method by which to ask an ?Ollama? instance. "
                   "Possible values: get, post, head, patch, purge etc.")
        ->capture_default_str();

    app.add_option("-T,--template", cfg.http_body_template,
                   "Jinja template for HTTP request to an ?Ollama? instance.")
        ->default_val(R"({
    "model": "gemma3:4b-it-qat",
    "stream": false,
    "messages": [
      {
        "role": "user",
        "content": "{{ prompt }}"
      }
    ]
})");

    app.add_option("-P,--prompt", cfg.prompt_template,
                   "Prompt's Jinja template for an LLM")
        ->default_val(
            R"(Always be brutally honest (to the point of being a little bit rude), smart, and extremely laconic. 
Do not rewrite instructions provided by user.
You will be supplied with author's name, title, description and subtitles of a YouTube video. 
Please, provide a summary with main points.

Author's name:

```
{{ author }}
```

Title:
```
{{ title }}
```

```
{{ description }}
```

Subtitles:

```
{{ subtitles }}
```
)");

    app.add_option("-H,--header", headers_raw,
                   "HTTP headers for request to an ?Ollama? instance.")
        ->take_all();

    app.add_option("-l,--log-file", cfg.log_file, "Filepath to internal logs")
        ->default_val("./logs.log");

    app.add_option(
           "--log-level", log_level_str,
           "Log level: "
           "tracel3,tracel2,tracel1,debug,info,notice,warning,error,critical")
        ->default_val("info");

    app.add_flag("-s,--proceed-shorts", cfg.proceed_with_shorts,
                 "Try do with shorts");

    app.add_flag("-A,--enable-server", cfg.enable_server, "Enable server")
        ->default_val(false);
    app.add_flag("-p,--port", cfg.server_port, "Server's port")
        ->check(CLI::PositiveNumber)
        ->default_val(SERVER_DEFAULT_PORT);

    app.add_option(
           "-j,--jobs-yt-tlp", cfg.concurrency_yt_dlp,
           "Amount of concurrent yt-dlp processes created by this application.")
        ->check(CLI::PositiveNumber)
        ->default_val(MAX_CONCURRENT_YTDLP_DEFAULT);

    app.add_option("-J,--jobs-requests", cfg.concurrency_ollama,
                   "Amount of concurrent request to an ?Ollama? instance sent "
                   "by this application")
        ->check(CLI::PositiveNumber)
        ->default_val(MAX_CONCURRENT_OLLAMA_DEFAULT);
    try
        {
            app.parse(argc, argv);

            // Post-processing complex types
            cfg.url = boost::urls::url(url_str);

            auto verb_opt
                = magic_enum::enum_cast<beast::http::verb>(method_str);
            if (!verb_opt)
                {
                    throw CLI::ValidationError(
                        "method", "Invalid HTTP method: " + method_str);
                }
            cfg.method = *verb_opt;

            cfg.log_level = quill::loglevel_from_string(log_level_str);

            for (const auto& header_raw_str : headers_raw)
                {
                    std::vector<std::string> parts;
                    boost::split(parts, header_raw_str, boost::is_any_of(":"));
                    if (parts.size() >= 2)
                        {
                            std::string key = boost::trim_copy(parts[0]);
                            std::string val = boost::trim_copy(parts[1]);
                            cfg.headers.insert(key, val);
                        }
                }
        }
    catch (const CLI::ParseError& e)
        {
            return app.exit(e);
        }
    catch (OmegaException<std::string>& e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace = boost::stacktrace::stacktrace::from_current_exception();
            std::string log = fmt::format(
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an Omega exception. Most definitely an incorrect "
                "value was specified in args. Here's "
                ".what():\n\n{}\nHere's data:\n{}\n Here's trace:\n{}\nHere's "
                "line where something failed:\n{}\n\nHere's an attempt to get "
                "backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace...",
                e.what(), e.data(), e.stack(), e.where(),
                boost::stacktrace::to_string(trace));

            fmt::print(std::cerr, "{}", log);
            return std::to_underlying(
                ReturnCodes::FailSpecifiedValueIsIncorrect);
        }
    catch (std::exception& e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace = boost::stacktrace::stacktrace::from_current_exception();
            fmt::print(
                std::cerr,
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nYou know that standard exceptions sucks. Here's "
                ".what():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.what(), boost::stacktrace::to_string(trace));
            return std::to_underlying(
                ReturnCodes::FailDuringInitializationConfig);
        }

    try
        {
            // Setup sink and logger
            auto file_sink
                = quill::Frontend::create_or_get_sink<quill::FileSink>(
                    cfg.log_file,
                    []()
                        {
                            quill::FileSinkConfig config_quill;
                            config_quill.set_open_mode('w');
                            // config_quill.set_filename_append_option (
                            //     quill::FilenameAppendOption::StartDateTime);
                            return config_quill;
                        }(),
                    quill::FileEventNotifier{});

            // Create and store the logger
            logger = quill::Frontend::create_or_get_logger(
                "root", std::move(file_sink));
            logger->set_log_level(cfg.log_level);
        }
    catch (std::exception& e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace = boost::stacktrace::stacktrace::from_current_exception();
            fmt::print(
                std::cerr,
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nYou know that standard exceptions sucks. Here's "
                ".what():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.what(), boost::stacktrace::to_string(trace));
            return std::to_underlying(ReturnCodes::FailInitializationLogger);
        }

    try
        {
            quill::BackendOptions backend_options;
            backend_options.check_printable_char = {};
            quill::Backend::start(backend_options);

            LOG_DEBUG(logger, "Successfully parsed command line arguments.");

            CacheHexHashFile cache(cfg.cache_file);
            LOG_DEBUG(logger, "Successfully created cache object.");
            CacheHexHashFile cache_subtitles(cfg.cache_subtitles_file);
            LOG_DEBUG(logger,
                      "Successfully created cache object for subtitles.");

            LOG_INFO(logger, "Trying to parse supplied headers...");

            net::io_context ioc;
            LOG_DEBUG(logger, "Entering coroutine...");
            net::signal_set signals(ioc, SIGINT, SIGTERM);
            if (not cfg.enable_server)
                {
                    corral::run(
                        ioc, corral::anyOf(
                                 async_main_no_server(ioc, cache,
                                                      cache_subtitles, cfg),
                                 signals.async_wait(corral::asio_awaitable)));
                }
            else
                {
                    LOG_INFO(logger, "Server started. Port: {}",
                             cfg.server_port);
                    corral::run(
                        ioc,
                        corral::anyOf(
                            server_acceptor(ioc, cache, cache_subtitles, cfg),
                            signals.async_wait(corral::asio_awaitable)));
                }
        }
    catch (OmegaException<std::filesystem::path>& e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace = boost::stacktrace::stacktrace::from_current_exception();
            std::string log = fmt::format(
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an Omega exception. Here's "
                ".what():\n\n{}\nHere's data:\n{}\n Here's trace:\n{}\nHere's "
                "line where something failed:\n{}\n\nHere's an attempt to get "
                "backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace...",
                e.what(), e.data(), e.stack(), e.where(),
                boost::stacktrace::to_string(trace));

            fmt::print(std::cerr, "{}", log);
            LOG_ERROR(logger, "{}", log);
            return std::to_underlying(ReturnCodes::FailCacheFolder);
        }
    catch (OmegaException<std::string>& e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace = boost::stacktrace::stacktrace::from_current_exception();
            std::string log = fmt::format(
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an Omega exception. Maybe from parsing result from "
                "Ollama. Here's "
                ".what():\n\n{}\nHere's data:\n{}\n Here's trace:\n{}\nHere's "
                "line where something failed:\n{}\n\nHere's an attempt to get "
                "backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace...",
                e.what(), e.data(), e.stack(), e.where(),
                boost::stacktrace::to_string(trace));

            fmt::print(std::cerr, "{}", log);
            LOG_ERROR(logger, "{}", log);
            return std::to_underlying(ReturnCodes::FailParsePromptResult);
        }
    catch (std::exception& e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace = boost::stacktrace::stacktrace::from_current_exception();
            std::string log = fmt::format(
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nYou know that standard exceptions sucks. Here's "
                ".what():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.what(), boost::stacktrace::to_string(trace));

            fmt::print(std::cerr, "{}", log);
            LOG_ERROR(logger, "{}", log);
            return std::to_underlying(ReturnCodes::FailStandardException);
        }
    return std::to_underlying(ReturnCodes::Success);
}
