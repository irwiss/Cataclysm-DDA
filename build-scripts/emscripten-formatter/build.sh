#!/bin/sh

emcc -O2 -std=c++17 -I./src -isystem ./src/third-party \
    -sSINGLE_FILE --shell-file build-scripts/emscripten-formatter/shell.html -sWASM=0 -sENVIRONMENT=web \
    -s MODULARIZE=1 -s 'EXPORT_NAME=json_formatter' \
    -s LLD_REPORT_UNDEFINED -s MINIFY_HTML=0 -s NO_DISABLE_EXCEPTION_CATCHING \
    -sEXPORTED_FUNCTIONS=_json_format,_free -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString \
    src/json.cpp tools/format/format.cpp build-scripts/emscripten-formatter/main.cpp -o formatter.html

