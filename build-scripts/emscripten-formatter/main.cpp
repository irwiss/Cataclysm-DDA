#include <iostream>
#include <type_traits>

enum class error_log_format_t { human_readable };
extern constexpr error_log_format_t error_log_format = error_log_format_t::human_readable;

void replace_substring( std::string &input, const std::string &substring,
                        const std::string &replacement, bool all )
{
    std::size_t find_after = 0;
    std::size_t pos = 0;
    const std::size_t pattern_length = substring.length();
    const std::size_t replacement_length = replacement.length();
    while( ( pos = input.find( substring, find_after ) ) != std::string::npos ) {
        input.replace( pos, pattern_length, replacement );
        find_after = pos + replacement_length;
        if( !all ) {
            break;
        }
    }
}

#include "../../src/json.h"
#include "../../tools/format/format.h"

extern "C" {
    const char *json_format( const char *input )
    {
        std::stringstream ss_out;
        std::stringstream ss_in( input );
        std::string ret_tmp;
        try {
            TextJsonIn jsin( ss_in );
            JsonOut jsout( ss_out, true );
            formatter::format( jsin, jsout );
            ret_tmp = ss_out.str();
        } catch( const std::exception &ex ) {
            ret_tmp = std::string( ex.what() );
        }

        const char *ret_ctmp = ret_tmp.c_str();
        const int len = strlen( ret_ctmp );
        char *ret = static_cast<char *>( malloc( len + 1 ) );
        ret[0] = 0;
        strncat( ret, ret_ctmp, len );
        return ret;
    }
}
