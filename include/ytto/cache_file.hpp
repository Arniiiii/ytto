#ifndef INCLUDE_YOUTUBETOOLLAMA_CACHE_FILE_HPP_
#define INCLUDE_YOUTUBETOOLLAMA_CACHE_FILE_HPP_

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <boost/hash2/hash_append.hpp>
#include <boost/hash2/xxhash.hpp>
#include <fmt/format.h>
#include <fmt/os.h>

#include "cache.hpp"
#include "omega_exception.hpp"

/**
 * @class CacheHexHashFile
 * @brief a dumb cache that works on filesystem.
 * @description  Takes a path to a folder, implements something like hash map,
 * but stores it in files.
 * Actual logic:
 * 1. Take a path to a folder.
 * 2. Uses files in the folder with filenames as a key.
 * 3. Actual key value is a lowercase hex dump of hash function of a key.
 *
 * On collision throws an OmegaException. On any filesystem failure throws
 * exceptions from std::filesystem.
 *
 */
class CacheHexHashFile final : public ABCCache
{
    std::filesystem::path filepath_to_folder_;

  public:
    explicit CacheHexHashFile (const std::filesystem::path &filepath_to_folder)
    {

        std::filesystem::path abs
            = std::filesystem::absolute (filepath_to_folder);
        // all possible erros are going to be throwed. If directory exists, no
        // error
        /* bool did_directory_existed = */ std::filesystem::create_directories (
            abs);
        filepath_to_folder_ = abs;
    }

    [[nodiscard]] std::optional<std::string>
    get (std::string const &key) const final
    {
        auto hexed_key = get_actual_key (key);
        auto result_path = filepath_to_folder_ / hexed_key;
        if (std::filesystem::exists (result_path))
            {
                std::ifstream ifs (result_path);
                std::string result;
                std::getline (ifs, result, '\0');
                return result;
            }
        return std::nullopt;
    }

    void
    set (std::string const &key, std::string const &val) final
    {
        auto hexed_key = get_actual_key (key);
        auto result_path = filepath_to_folder_ / hexed_key;

        if (std::filesystem::exists (result_path))
            {
                throw OmegaException<decltype (result_path)> (
                    "Hash collision or intentional rewrite", result_path);
            }
        fmt::output_file (result_path.string ()).print ("{}\n", val);
    };

  private:
    [[nodiscard]] static std::string
    get_actual_key (std::string const &key)
    {
        boost::hash2::xxhash_64 hash_object;
        boost::hash2::hash_append (hash_object, {}, key);
        std::uint64_t hash = hash_object.result ();
        std::string hexed_key = fmt::format ("{:x}", hash);
        return hexed_key;
    }
};

#endif // INCLUDE_YOUTUBETOOLLAMA_CACHE_FILE_HPP_
