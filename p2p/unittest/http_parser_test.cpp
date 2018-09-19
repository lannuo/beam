// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "p2p/http_msg_reader.h"
#include "utility/helpers.h"
#include "utility/logger.h"

using namespace beam;
using namespace std;

namespace {

struct FragmentedInput {
    FragmentedInput(const char* _buf, size_t _size) :
        buf(_buf), size(_size)
    {}

    bool next_fragment(const void** p, size_t* s, size_t desiredSize) {
        if (size == 0) return false;
        *s = (desiredSize < size) ? desiredSize : size;
        *p = buf;
        buf += *s;
        size -= *s;
        return true;
    }

    const char* buf;
    size_t size;
};

int test_bodyless_request() {
    int errors = 0;
    HttpMsgReader reader(
        HttpMsgReader::server,
        1,
        [&errors](uint64_t streamId, const HttpMsgReader::Message& m) -> bool {
            if (m.what != HttpMsgReader::http_message) {
                ++errors;
                return false;
            }
            if (m.msg->get_method() != "GET") ++errors;
            if (m.msg->get_path() != "/zzz") ++errors;
            if (m.msg->get_header("xxx") != "yyy") ++errors;
            if (m.msg->get_header("Host") != "example.com") ++errors;
            return true;
        },
        100,
        100
    );

    const char* input = "GET /zzz HTTP/1.1\r\nHost: example.com\r\nxxx: yyy\r\n\r\n";

    reader.new_data_from_stream(io::EC_OK, input, strlen(input));

    LOG_DEBUG() << __FUNCTION__ << " " << errors << " errors";
    return errors;
}

int test_request_with_body() {
    int errors = 0;
    HttpMsgReader reader(
        HttpMsgReader::server,
        1,
        [&errors](uint64_t streamId, const HttpMsgReader::Message& m) -> bool {
            if (m.what != HttpMsgReader::http_message) {
                ++errors;
                return false;
            }
            if (streamId != 1) ++errors;
            if (m.msg->get_method() != "GET") ++errors;
            if (m.msg->get_path() != "/zzz") ++errors;
            if (m.msg->get_header("xxx") != "yyy") ++errors;
            if (m.msg->get_header("Host") != "example.com") ++errors;
            size_t bodySize=0;
            const void* body = m.msg->get_body(bodySize);
            if (bodySize != 10 || memcmp(body, "0123456789", 10)) ++errors;
            return true;
        },
        100,
        100
    );

    const char* input = "GET /zzz HTTP/1.1\r\nHost: example.com\r\nxxx: yyy\r\nContent-Length: 10\r\n\r\n0123456789";

    reader.new_data_from_stream(io::EC_OK, input, strlen(input));

    LOG_DEBUG() << __FUNCTION__ << " " << errors << " errors";
    return errors;
}

} //namespace

int main() {
    int logLevel = LOG_LEVEL_DEBUG;
#if LOG_VERBOSE_ENABLED
    logLevel = LOG_LEVEL_VERBOSE;
#endif
    auto logger = Logger::create(logLevel, logLevel);
    int retCode = 0;
    try {
        retCode += test_bodyless_request();
        retCode += test_request_with_body();
    } catch (const exception& e) {
        LOG_ERROR() << e.what();
        retCode = 255;
    } catch (...) {
        LOG_ERROR() << "non-std exception";
        retCode = 255;
    }
    return retCode;
}
