#!/bin/bash

emcc -std=c++17 -Oz -flto -I src/ -isystem src/third-party/ \
    -s SINGLE_FILE --shell-file build-scripts/formatter-emscripten/shell.html -s WASM=1 \
    -s ENVIRONMENT=web -s MODULARIZE=1 -s 'EXPORT_NAME=json_formatter' -s NO\_FILESYSTEM=1 \
    -s LLD_REPORT_UNDEFINED -s MINIFY_HTML=0 -s NO_DISABLE_EXCEPTION_CATCHING \
    -s EXPORTED_FUNCTIONS=_json_format -s EXPORTED_RUNTIME_METHODS=cwrap \
    src/json.cpp tools/format/format.cpp build-scripts/formatter-emscripten/formatter-emscripten.cpp \
    -o formatter.html
