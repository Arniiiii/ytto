

#include <exceptions.hpp>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include <args-parser/all.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/fields_fwd.hpp>
#include <boost/beast/http/message_fwd.hpp>
#include <boost/beast/http/string_body_fwd.hpp>
#include <boost/beast/http/verb.hpp>
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

#include "yto/boost_stacktrace_format.hpp"
#include "yto/cache.hpp"
#include "yto/cache_file.hpp"
#include "yto/ollama_parser.hpp"
#include "yto/omega_exception.hpp"

template <typename T> struct Debug;

quill::Logger *logger;

constexpr auto HTTP_MAX_TIME_TIMEOUT_RFC = std::chrono::seconds (120);
constexpr auto MAX_PROMPT_TIME = std::chrono::minutes (10);
constexpr int HTTP_VERSION_TO_USE = 11;
constexpr size_t MAX_EXPECTED_CHARACTERS = 128000;

struct Config
{
    std::string language;
    std::string prompt_template;
    std::string http_body_template;
    boost::url url;
    boost::beast::http::verb method;
    boost::beast::http::fields headers;
    std::filesystem::path cache_file;
    std::filesystem::path cache_subtitles_file;
    std::filesystem::path log_file;
    quill::LogLevel log_level;
    size_t concurrency_yt_dlp;
    size_t concurrency_ollama;
    bool proceed_with_shorts;
};

struct EntryData
{
    std::string link_str;
    std::string author;
    std::string title;
    std::string description;
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

corral::Task<std::string>
get_subtitles (auto &ioc, std::string const &link, Config const &cfg)
{
    boost::asio::readable_pipe rp{ ioc };
    boost::process::shell cmd_get_subtitles = boost::process::shell (
        R"(yt-dlp -q --no-progress --no-warnings --skip-download --write-subs --write-auto-subs  --sub-lang )"
        + cfg.language
        + R"( --convert-subs vtt --exec before_dl:"cat %(requested_subtitles.:.filepath)#q | sed -e '/^[0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9][0-9] --> [0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9][0-9]/d' -e '/^[[:digit:]]\{1,3\}\$/d' -e 's/<[^>]*>//g' -e '/^[[:space:]]*$/d' -e '1,3d' -e \"s/'/\\\\'/g\" -e 's/\"/\\\"/g' | sed -z 's/\n/ /g' && rm %(requested_subtitles.:.filepath)#q " ')"
        + link + "'");
    auto exe = cmd_get_subtitles.exe ();
    auto proc = boost::process::process (
        ioc, exe, cmd_get_subtitles.args (),
        boost::process::process_stdio{ .in = { /* in to default */ },
                                       .out = rp,
                                       .err = { /* err to default */ } });

    auto read_loop
        = [] (boost::asio::readable_pipe &p) -> corral::Task<std::string>
        {
            std::string res;
            std::array<char, 4096> buf;
            for (;;)
                {
                    auto [error_code, received_size]
                        = co_await p.async_read_some (
                            boost::asio::buffer (buf),
                            corral::asio_nothrow_awaitable);
                    if (received_size)
                        {
                            res.append (buf.data (), received_size);
                        }
                    if (error_code)
                        {
                            co_return res;
                        }
                }
        };

    LOG_INFO (logger, "Called yt-dlp for {} with {} language", link,
              cfg.language);
    auto [ec_proc, subtitles_received] = co_await corral::allOf (
        proc.async_wait (corral::asio_awaitable), read_loop (rp));

    co_return subtitles_received;
}

corral::Task<std::string>
typical_http_request (auto &ioc, std::string const &request_body,
                      Config const &cfg)
{

    auto resolver = boost::asio::ip::tcp::resolver{ ioc };

    LOG_DEBUG (logger,
               "DNS look-up of an LLM's URL... "
               "host: {} port:{}",
               std::string (cfg.url.host ()), std::string (cfg.url.port ()));
    boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> const results
        = co_await resolver.async_resolve (cfg.url.host (), cfg.url.port (),
                                           corral::asio_awaitable);

    auto stream = boost::beast::tcp_stream{ ioc };
    stream.expires_after (std::chrono::seconds (HTTP_MAX_TIME_TIMEOUT_RFC));

    LOG_DEBUG (logger, "Trying to connect to an LLM...");
    co_await stream.async_connect (results, corral::asio_awaitable);
    LOG_DEBUG (logger, "Successfully.");

    LOG_DEBUG (logger, "Doing Jinja stuff to get correct "
                       "prompt and correct HTTP body...");

    LOG_DEBUG (logger, "Done Jinja stuff.");

    LOG_DEBUG (logger,
               "Creating a request object... "
               "method: {} path: {} http version:{}",
               magic_enum::enum_name (cfg.method),
               std::string (cfg.url.path ()), HTTP_VERSION_TO_USE);
    boost::beast::http::request<boost::beast::http::string_body> request{
        cfg.method, cfg.url.path (), HTTP_VERSION_TO_USE, request_body,
        cfg.headers
    };
    request.set (boost::beast::http::field::host, cfg.url.host ());
    request.set (boost::beast::http::field::user_agent,
                 BOOST_BEAST_VERSION_STRING);
    request.prepare_payload ();
    std::stringstream strs;
    strs << request;
    LOG_TRACE_L1 (logger, "Request:\n{}", strs.str ());
    stream.expires_after (MAX_PROMPT_TIME);

    LOG_INFO (logger, "Sending request to an LLM...");
    co_await boost::beast::http::async_write (stream, request,
                                              corral::asio_awaitable);

    boost::beast::flat_buffer buffer (MAX_EXPECTED_CHARACTERS);

    boost::beast::http::response<boost::beast::http::string_body> response;

    LOG_INFO (logger, "Waiting for response...");
    co_await boost::beast::http::async_read (stream, buffer, response,
                                             corral::asio_awaitable);
    LOG_INFO (logger, "Received response.");

    LOG_DEBUG (logger, "Trying to close connection.");

    boost::beast::error_code error_code;
    // NOLINTBEGIN(bugprone-unused-return-value,
    // cert-err33-c)
    stream.socket ().shutdown (boost::asio::ip::tcp::socket::shutdown_both,
                               error_code);
    // NOLINTEND(bugprone-unused-return-value,
    // cert-err33-c)

    // not_connected happens sometimes
    // so don't bother reporting it.
    //
    if (error_code && error_code != boost::beast::errc::not_connected)
        {
            throw boost::system::system_error (error_code, "shutdown");
        }
    LOG_DEBUG (logger, "Supposedly closed connection.");

    co_return response.body ();
}

corral::Task<std::string>
typical_https_request (auto &ioc, std::string const &request_body,
                       Config const &cfg)
{
    boost::asio::ssl::context sslCtx (boost::asio::ssl::context::tlsv13);

    // This holds the root certificate used for verification
    // load_root_certificates(sslCtx);

    // Verify the remote server's certificate
    sslCtx.set_verify_mode (boost::asio::ssl::verify_peer);
    auto resolver = boost::asio::ip::tcp::resolver{ ioc };
    boost::asio::ssl::stream<boost::beast::tcp_stream> stream (ioc, sslCtx);

    LOG_DEBUG (logger,
               "DNS look-up of an LLM's URL... "
               "host: {} port:{}",
               std::string (cfg.url.host ()), std::string (cfg.url.port ()));
    boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> const results
        = co_await resolver.async_resolve (cfg.url.host (), cfg.url.port (),
                                           corral::asio_awaitable);

    boost::beast::get_lowest_layer (stream).expires_after (
        std::chrono::seconds (HTTP_MAX_TIME_TIMEOUT_RFC));

    LOG_DEBUG (logger, "Trying to connect to an LLM...");
    co_await boost::beast::get_lowest_layer (stream).async_connect (
        results, corral::asio_awaitable);
    LOG_DEBUG (logger, "Successfully.");

    LOG_DEBUG (logger, "Doing Jinja stuff to get correct "
                       "prompt and correct HTTP body...");

    LOG_DEBUG (logger, "Done Jinja stuff.");

    LOG_DEBUG (logger,
               "Creating a request object... "
               "method: {} path: {} http version:{}",
               magic_enum::enum_name (cfg.method),
               std::string (cfg.url.path ()), HTTP_VERSION_TO_USE);
    boost::beast::http::request<boost::beast::http::string_body> request{
        cfg.method, cfg.url.path (), HTTP_VERSION_TO_USE, request_body,
        cfg.headers
    };
    request.set (boost::beast::http::field::host, cfg.url.host ());
    request.set (boost::beast::http::field::user_agent,
                 BOOST_BEAST_VERSION_STRING);
    request.prepare_payload ();
    std::stringstream strs;
    strs << request;
    LOG_TRACE_L1 (logger, "Request:\n{}", strs.str ());
    boost::beast::get_lowest_layer (stream).expires_after (MAX_PROMPT_TIME);

    LOG_INFO (logger, "Sending request to an LLM...");
    co_await boost::beast::http::async_write (stream, request,
                                              corral::asio_awaitable);

    boost::beast::flat_buffer buffer (MAX_EXPECTED_CHARACTERS);

    boost::beast::http::response<boost::beast::http::string_body> response;

    LOG_INFO (logger, "Waiting for response...");
    co_await boost::beast::http::async_read (stream, buffer, response,
                                             corral::asio_awaitable);
    LOG_INFO (logger, "Received response.");

    LOG_DEBUG (logger, "Trying to close connection.");

    boost::beast::error_code error_code;
    // NOLINTBEGIN(bugprone-unused-return-value,
    // cert-err33-c)
    auto ec = co_await stream.async_shutdown (corral::asio_nothrow_awaitable);
    // boost::beast::get_lowest_layer (stream).socket ().shutdown (
    //     boost::asio::ip::tcp::socket::shutdown_both, error_code);
    // NOLINTEND(bugprone-unused-return-value,
    // cert-err33-c)

    // not_connected happens sometimes
    // so don't bother reporting it.

    if (ec && ec != boost::asio::ssl::error::stream_truncated)
        {
            throw boost::system::system_error (ec, "shutdown");
        }

    LOG_DEBUG (logger, "Supposedly closed connection.");

    co_return response.body ();
}

corral::Task<std::string>
request_to_LLM (auto &ioc, std::string &request_body, Config const &cfg)
{

    if ("https" == cfg.url.scheme ())
        {
            co_return co_await typical_https_request (ioc, request_body, cfg);
        }
    else
        {
            co_return co_await typical_http_request (ioc, request_body, cfg);
        }
}

corral::Task<std::string>
summarize (corral::Semaphore &semaphore_yt_dlp,
           corral::Semaphore &semaphore_ollama, std::string const &link_str,
           inja::json &data, auto &ioc, ABCCache &cache,
           ABCCache &cache_subtitles, Config const &cfg)
{

    LOG_INFO (logger, "Checking cache...");
    std::optional<std::string> possible_res = cache.get (link_str);
    if (possible_res.has_value ())
        {
            LOG_INFO (logger, "Found result in cache.");
            co_return *possible_res;
        }
    std::string summary;
    LOG_INFO (logger, "Not found in cache.");

    std::optional<std::string> maybe_subtitles = cache_subtitles.get (link_str);
    std::string subtitles;
    if (maybe_subtitles.has_value ())
        {
            subtitles = *maybe_subtitles;
        }
    else
        {
            std::string subtitles_received;

            {
                auto lock = co_await semaphore_yt_dlp.lock ();
                subtitles_received
                    = co_await get_subtitles (ioc, link_str, cfg);
            }

            LOG_INFO (logger, "Received subtitles!");
            LOG_TRACE_L1 (logger, "Received subtitles: {}", subtitles_received);
            subtitles = std::move (subtitles_received);
            LOG_INFO (logger, "Saving received subtitles to "
                              "subtitles's cache...");
            cache_subtitles.set (link_str, subtitles);
            LOG_INFO (logger, "Saved received subtitles to "
                              "subtitles's cache.");
        }

    data["subtitles"] = subtitles;
    std::string prompt = inja::render (cfg.prompt_template, data);

    inja::json data_prompt;
    boost::algorithm::replace_all (prompt, "\n", R"(\n)");
    boost::algorithm::replace_all (prompt, "\"", R"(\")");
    data_prompt["prompt"] = prompt;
    std::string request_body
        = inja::render (cfg.http_body_template, data_prompt);

    std::string LLM_res;

    {
        auto lock = co_await semaphore_ollama.lock ();
        LLM_res = co_await request_to_LLM (ioc, request_body, cfg);
    }

    OllamaParser parser;

    summary = parser.getResponse (LLM_res);

    LOG_TRACE_L1 (logger, "Received response:{}", LLM_res);
    LOG_DEBUG (logger, "Saving response to cache");

    cache.set (link_str, summary);

    co_return summary;
}

corral::Task<void>
async_main (auto &ioc, ABCCache &cache, ABCCache &cache_subtitles,
            Config const &cfg)
{
    namespace pt = boost::property_tree;
    LOG_DEBUG (logger, "Entered coroutine...");

    LOG_DEBUG (logger, "Waiting for YouTube's RSS feed from stdin...");
    std::cin >> std::noskipws;
    std::istreambuf_iterator<char> start (std::cin);
    std::istreambuf_iterator<char> end;
    std::string xml_rss_youtube_feed (start, end);
    LOG_DEBUG (logger, "Received something from stdin...");
    pt::ptree tree;
    std::istringstream istr (xml_rss_youtube_feed);
    LOG_DEBUG (logger, "Trying to parse it as an XML...");
    pt::read_xml (istr, tree);
    LOG_DEBUG (logger, "Successfully parsed an XML...");

    corral::Semaphore semaphore_yt_dlp (cfg.concurrency_yt_dlp);
    corral::Semaphore semaphore_ollama (cfg.concurrency_ollama);

    CORRAL_WITH_NURSERY (nursery)
    {
        for (auto &xml_entry : tree.get_child ("feed"))
            {
                if ("entry" != xml_entry.first)
                    {
                        continue;
                    }
                auto const &link
                    = xml_entry.second.get_child ("link.<xmlattr>.href");
                std::string link_str = link.data ();
                LOG_INFO (logger,
                          "Got link to a YouTube video, maybe... Here's "
                          "the link: {}",
                          link_str);
                if (not cfg.proceed_with_shorts
                    and link_str.contains ("shorts"))
                    {
                        LOG_INFO (logger,
                                  "This is a link to a short. Skipping.");
                        continue;
                    }

                auto const &author = xml_entry.second.get_child ("author.name");
                auto const &title
                    = xml_entry.second.get_child ("media:group.media:title");
                auto &description = xml_entry.second.get_child (
                    "media:group.media:description");

                nursery.start (
                    [&, link_str = link_str, author = std::cref (author),
                     title = std::cref (title),
                     description
                     = std::ref (description)] () mutable -> corral::Task<void>
                        {
                            inja::json data;
                            data["author"] = author.get ().data ();
                            data["title"] = title.get ().data ();
                            data["description"] = description.get ().data ();
                            std::string summary = co_await summarize (
                                semaphore_yt_dlp, semaphore_ollama, link_str,
                                data, ioc, cache, cache_subtitles, cfg);

                            LOG_INFO (logger, "Appending LLM's result to "
                                              "entry's description...");
                            std::string new_description = fmt::format (
                                "{}\n\nLLM's result:\n{}",
                                description.get ().data (), summary);
                            description.get ().put ("", new_description);
                            LOG_INFO (logger,
                                      "Successfully appended, I guess...");
                        });
            }
        co_return corral::join;
    };
    LOG_INFO (logger, "Writing result to stdout...");
    pt::write_xml (std::cout, tree);
    LOG_INFO (logger, "Wrote result to stdout.");
}

int
main (int argc, char *argv[])
{
    Config cfg;
    try
        {
            Args::CmdLine cmd (argc, argv);
            cmd.addArgWithFlagAndName (
                   'c', "cache-folder", true, true, "Filepath to cache folder",
                   "Folder, in which there will be files as cache "
                   "of result of summarization")
                .addArgWithFlagAndName (
                    'S', "cache-folder-subtitles", true, true,
                    "Filepath to cache folder for subtitles",
                    "Folder, in which there will be files as subtitles of a "
                    "specific YouTube link.")
                .addArgWithFlagAndName ('L', "language", true, false,
                                        "yt-dlp subtitles's language",
                                        "yt-dlp language of subtitles", "en")
                .addArgWithFlagAndName ('u', "url", true, false,
                                        "URL of ?Ollama? instance",
                                        "URL of ?Ollama? instance in format "
                                        "http://127.0.0.1:11434/api/chat",
                                        "http://127.0.0.1:11434/api/chat")
                .addArgWithFlagAndName (
                    'X', "method", true, false, "HTTP method ?Ollama?",
                    R"(HTTP method by which to ask an ?Ollama? instance. 
                Possible values are the same as names of enums in boost::beast::http::verb :
                "get", "post","head", "patch","purge" etc. )",
                    "post")
                .addArgWithFlagAndName ('T', "template", true, false,
                                        "HTTP Jinja template",
                                        "Jinja template for HTTP request to an "
                                        "?Ollama? instance.",
                                        R"({
    "model": "gemma3:4b-it-qat",
    "stream": false,
    "messages": [
      {
        "role": "user",
        "content": "{{ prompt }}"
      }
    ]
})")
                .addArgWithFlagAndName (
                    'P', "prompt", true, false, "Prompt's Jinja template",
                    "Prompt's Jinja template for an LLM",
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
)")
                .addMultiArgWithDefaulValues (
                    'H', "header", true, false, "HTTP header",
                    "HTTP headers for request to an ?Ollama? instance.",
                    { "Content-Type: application/json" })
                .addArgWithFlagAndName (
                    'l', "log-file", true, false, "Filepath to internal logs",
                    "Filepath to internal logs", "./logs.log")
                .addArgWithNameOnly ("log-level", true, false,
                                     "Log level: "
                                     "tracel3,tracel2,tracel1,debug,info,"
                                     "notice,warning,error,critical",
                                     "info")
                .addArgWithFlagAndName ('s', "proceed-shorts", false, false,
                                        "Try do with shorts")
                .addArgWithFlagAndName ('j', "jobs-yt-tlp", true, false,
                                        "Amount of concurrent yt-dlp processes "
                                        "created by this application",
                                        "Amount of concurrent yt-dlp processes "
                                        "created by this application.",
                                        "5")
                .addArgWithFlagAndName (
                    'J', "jobs-requests", true, false,
                    "Amount of concurrent request to an ?Ollama? instance "
                    "sent by this application",
                    "Amount of concurrent request to an ?Ollama? instance "
                    "sent by this application",
                    "6")
                .addHelp (
                    true,
                    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                    argv[0],
                    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                    "Post-processor for YouTube's RSS feed, so that you get "
                    "summary of "
                    "video inside the feed via sending an HTTP request to "
                    "something "
                    "like an Ollama instance. For detailed help for specific "
                    "argument, try `exe_name -h --flag` i.e. `exe_name -h -X` "
                    ".",
                    1000);
            cmd.parse ();

            boost::beast::http::fields headers;
            // This loop could be optimized, but good enough for now.
            for (const std::string &header : cmd.values ("-H"))
                {
                    std::vector<std::string> splitted;
                    boost::algorithm::split (splitted, header,
                                             [] (char character)
                                                 { return character == ':'; });
                    for (auto &str : splitted)
                        {
                            boost::algorithm::trim (str);
                        }
                    headers.insert (splitted.at (0), splitted.at (1));
                };
            cfg = Config{
                .language = cmd.value ("-L"),
                .prompt_template = cmd.value ("-P"),
                .http_body_template = cmd.value ("-T"),
                .url = boost::urls::url (cmd.value ("-u")),
                .method = magic_enum::enum_cast<boost::beast::http::verb> (
                              cmd.value ("-X"))
                              .value (),
                .headers = headers,
                .cache_file = cmd.value ("-c"),
                .cache_subtitles_file = cmd.value ("-S"),
                .log_file = cmd.value ("-l"),
                .log_level
                = quill::loglevel_from_string (cmd.value ("--log-level")),
                .concurrency_yt_dlp = std::invoke (
                    [&] ()
                        {
                            auto an_integer = std::stoll (cmd.value ("-j"));
                            if (an_integer < 0)
                                {
                                    throw OmegaException<std::string> (
                                        "Received value for concurrency for "
                                        "yt-dlp is negative, while it has to "
                                        "be a positive integer.",
                                        cmd.value ("-j"));
                                }
                            return static_cast<size_t> (an_integer);
                        }),
                .concurrency_ollama = std::invoke (
                    [&] ()
                        {
                            auto an_integer = std::stoll (cmd.value ("-J"));
                            if (an_integer < 0)
                                {
                                    throw OmegaException<std::string> (
                                        "Received value for concurrency for "
                                        "ollama is negative, while it has to "
                                        "be a positive integer.",
                                        cmd.value ("-J"));
                                }
                            return static_cast<size_t> (an_integer);
                        }),
                .proceed_with_shorts = cmd.isDefined ("-s")
            };
        }
    catch (const Args::HelpHasBeenPrintedException &)
        {
            return 0;
        }
    catch (Args::BaseException &e)
        {

            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();

            fmt::print (
                std::cerr,
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an exception from args-parser library. Here's "
                ".desc():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.desc (), boost::stacktrace::to_string (trace));
            return std::to_underlying (ReturnCodes::FailDuringParsingCmdValues);
        }
    catch (OmegaException<std::string> &e)
        {

            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();
            std::string log = fmt::format (
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an Omega exception. Most definitely an incorrect "
                "value was specified in args. Here's "
                ".what():\n\n{}\nHere's data:\n{}\n Here's trace:\n{}\nHere's "
                "line where something failed:\n{}\n\nHere's an attempt to get "
                "backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace...",
                e.what (), e.data (), e.stack (), e.where (),
                boost::stacktrace::to_string (trace));

            fmt::print (std::cerr, "{}", log);
            return std::to_underlying (
                ReturnCodes::FailSpecifiedValueIsIncorrect);
        }
    catch (std::exception &e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();
            fmt::print (
                std::cerr,
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nYou know that standard exceptions sucks. Here's "
                ".what():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.what (), boost::stacktrace::to_string (trace));
            return std::to_underlying (
                ReturnCodes::FailDuringInitializationConfig);
        }

    try
        {
            // Setup sink and logger
            auto file_sink
                = quill::Frontend::create_or_get_sink<quill::FileSink> (
                    cfg.log_file,
                    [] ()
                        {
                            quill::FileSinkConfig config_quill;
                            config_quill.set_open_mode ('w');
                            // config_quill.set_filename_append_option (
                            //     quill::FilenameAppendOption::StartDateTime);
                            return config_quill;
                        }(),
                    quill::FileEventNotifier{});

            // Create and store the logger
            logger = quill::Frontend::create_or_get_logger (
                "root", std::move (file_sink));
            logger->set_log_level (cfg.log_level);
        }
    catch (std::exception &e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();
            fmt::print (
                std::cerr,
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nYou know that standard exceptions sucks. Here's "
                ".what():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.what (), boost::stacktrace::to_string (trace));
            return std::to_underlying (ReturnCodes::FailInitializationLogger);
        }

    try
        {

            quill::BackendOptions backend_options;
            backend_options.check_printable_char = {};
            quill::Backend::start (backend_options);

            LOG_DEBUG (logger, "Successfully parsed command line arguments.");

            CacheHexHashFile cache (cfg.cache_file);
            LOG_DEBUG (logger, "Successfully created cache object.");
            CacheHexHashFile cache_subtitles (cfg.cache_subtitles_file);
            LOG_DEBUG (logger,
                       "Successfully created cache object for subtitles.");

            LOG_INFO (logger, "Trying to parse supplied headers...");

            boost::asio::io_context ioc;
            LOG_DEBUG (logger, "Entering coroutine...");
            boost::asio::signal_set signals (ioc, SIGINT, SIGTERM);
            corral::run (ioc, corral::anyOf (
                                  async_main (ioc, cache, cache_subtitles, cfg),
                                  signals.async_wait (corral::asio_awaitable)));
        }
    catch (OmegaException<std::filesystem::path> &e)
        {

            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();
            std::string log = fmt::format (
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an Omega exception. Here's "
                ".what():\n\n{}\nHere's data:\n{}\n Here's trace:\n{}\nHere's "
                "line where something failed:\n{}\n\nHere's an attempt to get "
                "backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace...",
                e.what (), e.data (), e.stack (), e.where (),
                boost::stacktrace::to_string (trace));

            fmt::print (std::cerr, "{}", log);
            LOG_ERROR (logger, "{}", log);
            return std::to_underlying (ReturnCodes::FailCacheFolder);
        }
    catch (OmegaException<std::string> &e)
        {

            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();
            std::string log = fmt::format (
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nThis is an Omega exception. Maybe from parsing result from "
                "Ollama. Here's "
                ".what():\n\n{}\nHere's data:\n{}\n Here's trace:\n{}\nHere's "
                "line where something failed:\n{}\n\nHere's an attempt to get "
                "backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace...",
                e.what (), e.data (), e.stack (), e.where (),
                boost::stacktrace::to_string (trace));

            fmt::print (std::cerr, "{}", log);
            LOG_ERROR (logger, "{}", log);
            return std::to_underlying (ReturnCodes::FailParsePromptResult);
        }
    catch (std::exception &e)
        {
            boost::stacktrace::basic_stacktrace<
                std::allocator<boost::stacktrace::frame>>
                trace
                = boost::stacktrace::stacktrace::from_current_exception ();
            std::string log = fmt::format (
                "Oohh, look at you, who got an exception, my cutie lovely guy. "
                "\nYou know that standard exceptions sucks. Here's "
                ".what():\n\n{}\n\nHere's an attempt to get backtrace of it "
                "via "
                "boost::stacktrace and libbacktrace... Hope it works or kill "
                "youself debugging this shit.\n{}\n",
                e.what (), boost::stacktrace::to_string (trace));

            fmt::print (std::cerr, "{}", log);
            LOG_ERROR (logger, "{}", log);
            return std::to_underlying (ReturnCodes::FailStandardException);
        }
    return std::to_underlying (ReturnCodes::Success);
}
