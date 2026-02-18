#ifndef INCLUDE_YOUTUBETOOLLAMA_OLLAMA_PARSER_HPP_
#define INCLUDE_YOUTUBETOOLLAMA_OLLAMA_PARSER_HPP_

#include <string>

#include <glaze/core/context.hpp>
#include <glaze/json.hpp>
#include <glaze/util/expected.hpp>

#include "yto/omega_exception.hpp"

class OllamaParser
{

    struct Message
    {
        std::string role;
        std::string content;
    };

    struct Response
    {
        std::string model;
        std::string created_at;
        Message message;
        std::string done_reason;
        bool done;
        int64_t total_duration;
        int64_t load_duration;
        int64_t prompt_eval_count;
        int64_t prompt_eval_duration;
        int64_t eval_count;
        int64_t eval_duration;
    };

  public:
    std::string
    getResponse (std::string raw_json)
    {
        glz::expected<Response, glz::error_ctx> maybe_parsed
            = glz::read_json<Response> (raw_json);

        if (not maybe_parsed.has_value ())
            {
                throw OmegaException<std::string> (
                    glz::format_error (maybe_parsed.error ()), raw_json);
            }

        return maybe_parsed.value ().message.content;
    }
};

#endif // INCLUDE_YOUTUBETOOLLAMA_OLLAMA_PARSER_HPP_
