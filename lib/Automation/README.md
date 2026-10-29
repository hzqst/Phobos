# Automation dependencies

`json.hpp` is the unmodified nlohmann/json **v3.12.0** single header.

* Source: https://github.com/nlohmann/json/blob/v3.12.0/single_include/nlohmann/json.hpp
* Download: https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp
* License: MIT, copied to `json.LICENSE`.
* SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`

The project includes this vendored header through `src/Automation/Json.h`, with
`JSON_NOEXCEPTION` to match Phobos's runtime. No package manager, generated code,
external runtime, or network download is needed during a build.

The production HTTP transport uses Windows Winsock directly. The Python MCP SDK
is only a test-client dependency; it does not run a second production server.
