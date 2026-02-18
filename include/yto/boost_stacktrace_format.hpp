#ifndef INCLUDE_YOUTUBETOOLLAMA_BOOST_STACKTRACE_FORMAT_HPP_
#define INCLUDE_YOUTUBETOOLLAMA_BOOST_STACKTRACE_FORMAT_HPP_

#include <boost/stacktrace/stacktrace.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
// #include <fmt/ranges.h>
// #include <quill/DeferredFormatCodec.h>
// #include <quill/bundled/fmt/ranges.h>

// template <> struct fmt::formatter<boost::stacktrace::frame, char>
// {
//     template <class ParseContext> constexpr ParseContext::iterator
//     parse (ParseContext &ctx)
//     {
//         return ctx.begin ();
//     }
//
//     template <class FmtContext> FmtContext::iterator
//     format (boost::stacktrace::frame frame, FmtContext &ctx) const
//     {
//         return fmt::format_to (ctx.out (), "{}\n",
//                                boost::stacktrace::to_string (frame));
//     }
// };
// static_assert (fmt::formattable<boost::stacktrace::frame, char>);

template <> struct fmt::formatter<boost::stacktrace::basic_stacktrace<>, char>
{
    template <class ParseContext> constexpr ParseContext::iterator
    parse (ParseContext &ctx)
    {
        return ctx.begin ();
    }

    template <class FmtContext> FmtContext::iterator
    format (const boost::stacktrace::basic_stacktrace<> &stacktrace,
            FmtContext &ctx) const
    {
        return fmt::format_to (ctx.out (), "{}",
                               boost::stacktrace::to_string (stacktrace));
    }
};

static_assert (fmt::formattable<boost::stacktrace::basic_stacktrace<>, char>);

// template <> struct fmtquill::formatter<boost::stacktrace::frame, char>
// {
//   template <class ParseContext>
//   constexpr ParseContext::iterator parse(ParseContext& ctx)
//   {
//     return ctx.begin();
//   }
//
//   template <class FmtContext>
//   FmtContext::iterator format(boost::stacktrace::frame frame,
//                               FmtContext& ctx) const
//   {
//     return fmt::format_to(ctx.out(), "{}",
//     boost::stacktrace::to_string(frame));
//   }
// };
// static_assert(fmtquill::formattable<boost::stacktrace::frame, char>);
//
// template <>
// struct fmtquill::formatter<boost::stacktrace::basic_stacktrace<>, char>
// {
//   template <class ParseContext>
//   constexpr ParseContext::iterator parse(ParseContext& ctx)
//   {
//     return ctx.begin();
//   }
//
//   template <class FmtContext> FmtContext::iterator format(
//       const boost::stacktrace::basic_stacktrace<>& stacktrace,
//       FmtContext& ctx) const
//   {
//     return fmtquill::format_to(ctx.out(), "{}",
//                                boost::stacktrace::to_string(stacktrace));
//   }
// };
// static_assert(
//     fmtquill::formattable<boost::stacktrace::basic_stacktrace<>, char>);
//
// template <> struct quill::Codec<boost::stacktrace::stacktrace>
//     : quill::DeferredFormatCodec<boost::stacktrace::stacktrace>
// {
// };

#endif // INCLUDE_YOUTUBETOOLLAMA_BOOST_STACKTRACE_FORMAT_HPP_
