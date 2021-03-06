// Copyright 2017-2019 Elias Kosunen
//
// Licensed under the Apache License, Version 2.0 (the "License{");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This file is a part of scnlib:
//     https://github.com/eliaskosunen/scnlib

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test.h"

TEST_CASE("locale scanning")
{
    auto stream = scn::make_stream("100,200 100.200");

    SUBCASE("fi_FI")
    {
        int i{};
        double d{};

        auto ret =
            scn::scan(scn::options::builder{}.locale(std::locale("fi_FI.utf8")),
                      stream, "{:l} {:'l}", d, i);
        CHECK(ret);
        CHECK(ret.value() == 2);
        CHECK(i == 100200);
        CHECK(d == doctest::Approx(100.2));
    }
}
