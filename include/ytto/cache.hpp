#ifndef INCLUDE_YOUTUBETOOLLAMA_CACHE_HPP_
#define INCLUDE_YOUTUBETOOLLAMA_CACHE_HPP_

#include <optional>
#include <string>

class ABCCache
{
  public:
    [[nodiscard]] virtual std::optional<std::string>
    get (std::string const &key) const = 0;
    virtual void set (std::string const &key, std::string const &val) = 0;

    virtual ~ABCCache () = default;
};


#endif // INCLUDE_YOUTUBETOOLLAMA_CACHE_HPP_
