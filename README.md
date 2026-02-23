# rss-ytto

RSS YouTubeToOllama is an application that takes as inputs a YouTube's RSS feed from a channel as stdin, call's yt-dlp to get subtitles, sends it to an Ollama instance for summarization, appends it to description of a video, outputs the feed to stdout. It caches subtitles and summary from Ollama in a specified folder.

## Demo

https://github.com/user-attachments/assets/367841a5-d2a2-4a4c-bd58-e266a7c27181

## Concerns

### Is it good to use?

Heavily depends on prompt and a choice of a model.

FYI, Gemini uses a multimodal model for summarization, so it is capable of OCR of some frame in videos. But it uses a lot of tokens.

By default, the application just sends subtitles and description to an Ollama instance, but in a future, you can customize it to use Gemini with a link to it.

### Is it legal?

Technically, maybe not, but for sure this is a violation of YouTube's Terms of Service. But for personal uses I guess you won't be swatted.

### Is it possible to make it legal?

Yes and no.

Yes, in a close future it will be possible to use Gemini. Gemini is an in-house's Google's LLM, and they have legal rights to summarize, since videos are inside YouTube, which is owned by Google. What's more funny, it is a violation of ToS if you try to do that yourself. Greediness in bloom.

No, since I am not sure if Gemini's API allows summarizing, since like a year ago I tested it wasn't possible.

No, if using caching of subtitles. AFAIK legally speaking it is illegal to download subtitles of a video, if it is not your video, since it is intellectual property of a video's author. Also, it is not clear if summarization is transformative enough from legal point of view. So, even if disable caching of subtitles, I have no idea if it's completely legal, but for now it seems to be light-gray-ish. Unless someone tells me to this is illegal, I'll keep this project public. Also, I do not have a goal to monetize this project. I expect it to be used as a personal convenient thing for someone.

### Why it is written in C++ and not, let say, in Python?

I started writing this application in bash. Then I got to the moment that I need to parse XML. I understood that is a difficult task in bash.

Then there were three possibilities from my perspective:

- Write it in Python and heavily vibe code it
- in C++
- in Rust

I decided to do that in C++ over Python, since I wanted to use structured concurrency in C++ via [corral](https://github.com/hudson-trading/corral/) library.

C++ over Rust since I wanted to do the app in a language that I don't use at my current job.

## --help

````
Post-processor for YouTube's RSS feed, so that you get summary of video inside
the feed via sending an HTTP request to something like an Ollama instance.


./build/YoutubeToOllama-0.0.0.1 [OPTIONS]


OPTIONS:
  -h,     --help              Print this help message and exit
  -c,     --cache-folder TEXT REQUIRED
                              Folder, in which there will be files as cache of result of
                              summarization
  -S,     --cache-folder-subtitles TEXT REQUIRED
                              Folder, in which there will be files as subtitles of a specific
                              YouTube link.
  -L,     --language TEXT [en]
                              yt-dlp language of subtitles
  -u,     --url TEXT [http://127.0.0.1:11434/api/chat]
                              URL of ?Ollama? instance in format
                              http://127.0.0.1:11434/api/chat
  -X,     --method TEXT [post]
                              HTTP method by which to ask an ?Ollama? instance. Possible
                              values: get, post, head, patch, purge etc.
  -T,     --template TEXT [{
    "model": "gemma3:4b-it-qat",
    "stream": false,
    "messages": [
      {
        "role": "user",
        "content": "{{ prompt }}"
      }
    ]
}]
                              Jinja template for HTTP request to an ?Ollama? instance.
  -P,     --prompt TEXT [Always be brutally honest (to the point of being a little bit rude), smart, and extremely laconic.
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
]
                              Prompt's Jinja template for an LLM
  -H,     --header TEXT ...   HTTP headers for request to an ?Ollama? instance.
  -l,     --log-file TEXT [./logs.log]
                              Filepath to internal logs
          --log-level TEXT [info]
                              Log level:
                              tracel3,tracel2,tracel1,debug,info,notice,warning,error,critical
  -s,     --proceed-shorts    Try do with shorts
  -j,     --jobs-yt-tlp UINT:POSITIVE [5]
                              Amount of concurrent yt-dlp processes created by this
                              application.
  -J,     --jobs-requests UINT:POSITIVE [6]
                              Amount of concurrent request to an ?Ollama? instance sent by this
                              application

````

## Prerequisites

Installed `yt-dlp`.

## Build from source

Here Conan package manager is used. `CMakeLists.txt` is expected to work for other package managers.

### Dependencies:

- Boost
  - asio
  - Beast
  - URL
  - process
  - Property tree
  - stacktrace
  - range
  - algorithm
- corral
- quill
- inja
- magic_enum
- glaze
- OpenSSL
- CLI11
- fmt

Also, try installing libbacktrace for meaningful stacktraces for arbitrary exceptions. Sadly, but Conan's recipe is not good for this.

### Commands

`cd ./corral/`
`conan create .`
`cd ..`
`conan install . --build=missing --output-folder=./build --update`
`cmake -S . -B ./build`
`cmake --build ./build --verbose`
`./build/YoutubeToOllama`

## To-Do

- [x] Allow `{{ link }}` in prompt.
- [ ] Make it possible to plug in your own parser of requests from an LLM via Boost::DLL or a command.
  - [ ] a command
  - [ ] a plugin interface via Boost::DLL
- [ ] Make limits work for case when call to this application is done multiple times concurrently. Either:
  - [ ] make it possible to make this application an API server that just takes a YouTube URL to an RSS feed.
  - [ ] or find a good solution for monitoring list of processes in async manner on different OSes.
- [x] Support `https` in URL.
  - [ ] Test it.
    - [ ] Test with Gemini,
- [x] Fix `--help`, since it is a little bit ugly.
- [ ] Make it installable.
- [ ] Make CI for releasing.
- [ ] Allow refusing of caching of subtitles.

## License

MIT
