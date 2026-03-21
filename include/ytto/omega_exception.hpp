#ifndef INCLUDE_YOUTUBETOOLLAMA_OMEGA_EXCEPTION_HPP_
#define INCLUDE_YOUTUBETOOLLAMA_OMEGA_EXCEPTION_HPP_

#include <boost/stacktrace/stacktrace.hpp>
#include <boost/stacktrace/frame.hpp>
#include <source_location>
#include <utility>

template <typename DATA_T> class OmegaException : public std::exception
{
public:
  OmegaException(std::string str, DATA_T data,
                 const std::source_location& loc
                 = std::source_location::current(),
                 boost::stacktrace::stacktrace trace
                 = boost::stacktrace::stacktrace())
      : err_str(std::move(str)),
        data_(std::move(data)),
        location_{loc},
        backtrace_{std::move(trace)}
  {
  }
  DATA_T& data() { return data_; }
  const DATA_T& data() const noexcept { return data_; }
  std::string& what() { return err_str; }
  [[nodiscard]] const char* what() const noexcept override
  {
    return err_str.data();
  }
  [[nodiscard]] const std::source_location& where() const { return location_; }
  [[nodiscard]] const boost::stacktrace::stacktrace& stack() const
  {
    return backtrace_;
  }

private:
  std::string err_str;
  DATA_T data_;
  const std::source_location location_;
  const boost::stacktrace::stacktrace backtrace_;
};

#endif  // INCLUDE_YOUTUBETOOLLAMA_OMEGA_EXCEPTION_HPP_
