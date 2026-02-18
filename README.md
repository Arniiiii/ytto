# rss-ytto

RSS YouTubeToOllama is an application that takes as inputs an YouTube's RSS feed from a channel as stdin, call's yt-dlp to get subtitles, sends it to an Ollama instance for summarization, appends it to description of a video, outputs the feed to stdout. It caches subtitles and summary from Ollama in a specified folder.

## demo

https://github.com/user-attachments/assets/367841a5-d2a2-4a4c-bd58-e266a7c27181

## --help

```
Post-processor for YouTube's RSS feed, so that you get summary of video inside the feed via sending an HTTP request to something like an Ollama instance. For detailed help for specific argument, try `exe_name -h --flag` i.e. `exe_name -h -X` .

USAGE: ./build/YoutubeToOllama-0.0.0.1 -c, --cache-folder <arg> -S, --cache-folder-subtitles <arg> [ -H, --header <arg> ] [ -h, --help <arg> ] [ -J, --jobs-requests <arg> ] [ -j, --jobs-yt-tlp <arg> ] [ -L, --language <arg> ] [ -l, --log-file <arg> ] [ --log-level <arg> ] [ -X, --method <arg> ] [ -s, --proceed-shorts ] [ -P, --prompt <arg> ] [ -T, --template <arg> ] [ -u, --url <arg> ]

REQUIRED:
 -c, --cache-folder <arg>           Filepath to cache folder

 -S, --cache-folder-subtitles <arg> Filepath to cache folder for subtitles

OPTIONAL:
 -H, --header <arg>                 HTTP header

                                    Default value: Content-Type: application/json

 -h, --help <arg>                   Print this help.

 -J, --jobs-requests <arg>          Amount of concurrent request to an ?Ollama? instance sent by this application

                                    Default value: 6

 -j, --jobs-yt-tlp <arg>            Amount of concurrent yt-dlp processes created by this application

                                    Default value: 5

 -L, --language <arg>               yt-dlp subtitles's language

                                    Default value: en

 -l, --log-file <arg>               Filepath to internal logs

                                    Default value: ./logs.log

     --log-level <arg>              Log level: tracel3,tracel2,tracel1,debug,info,notice,warning,error,critical

 -X, --method <arg>                 HTTP method ?Ollama?

                                    Default value: post

 -s, --proceed-shorts               Try do with shorts

 -P, --prompt <arg>                 Prompt's Jinja template

                                    Default value: Always be brutally honest (to the point of being a little bit rude), smart, and extremely laconic.
                                    Do not rewrite instructions provided by user.
                                    You will be supplied with author's name, title, description and subtitles of a YouTube video.
                                    Please, provide a summary with main points.

                                    Author's name:
                                    \`\`\`
                                    {{ author }}
                                    \`\`\`

                                    Title:
                                    \`\`\`
                                    {{ title }}
                                    \`\`\`

                                    \`\`\`
                                    {{ description }}
                                    \`\`\`

                                    Subtitles:
                                    \`\`\`
                                    {{ subtitles }}
                                    \`\`\`


 -T, --template <arg>               HTTP Jinja template

                                    Default value: {
                                        "model": "gemma3:4b-it-qat",
                                        "stream": false,
                                        "messages": [
                                          {
                                            "role": "user",
                                            "content": "{{ prompt }}"
                                          }
                                        ]
                                    }

 -u, --url <arg>                    URL of ?Ollama? instance

                                    Default value: http://127.0.0.1:11434/api/chat
```


## Build from source

`cd ./corral/`
`conan create .`
`cd ..`
`conan install . --build=missing --output-folder=./build --update`
`cmake -S . -B ./build`
`cmake --build ./build --verbose`
`./build/YoutubeToOllama`

