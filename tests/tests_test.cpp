#include "cata_catch.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <set>

TEST_CASE( "enforce_normalized_test_cases", "[main]" )
{
    const static std::string allowed_chars =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "_-+/";

    const std::set<char> allowed_set( allowed_chars.begin(), allowed_chars.end() );
    CAPTURE( allowed_chars );
    INFO( "Limit TEST_CASE name to allowed chars this will make invoking tests from cli easier" );

    for( const Catch::TestCase &tc :
         Catch::getAllTestCasesSorted( *Catch::getCurrentContext().getConfig() ) ) {
        const std::string &test_case_name = tc.name;
        CAPTURE( test_case_name );
        for( char char_in_name : test_case_name ) {
            CAPTURE( char_in_name );
            const bool has_invalid_char = !allowed_set.count( char_in_name );
            CHECK( !has_invalid_char );
        }
    }
}
