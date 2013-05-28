/** bid_request_synth_test.cc                                 -*- C++ -*-
    Rémi Attab, 25 May 2013
    Copyright (c) 2013 Datacratic.  All rights reserved.

    Tests for the bid request synthetizer.

*/

#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include "bid_request_synth.h"
#include "soa/jsoncpp/value.h"
#include "soa/jsoncpp/reader.h"

#include <boost/test/unit_test.hpp>
#include <iostream>
#include <set>
#include <algorithm>

using namespace std;
using namespace RTBKIT;

void checkErr(
        const string& msg, const Json::Value& src, const Json::Value& dst)
{
    cerr << "=== ERROR: " << msg << endl
        << "src: " << src.toString()
        << "dst: " << dst.toString()
        << endl;

    BOOST_CHECK(false);
}


void check(const Json::Value& src, const Json::Value& dst)
{
    if (src.type() != dst.type()) {
        checkErr("type mismatch", src, dst);
        return;
    }

    if (src.type() == Json::objectValue) {
        const auto& srcMembers = src.getMemberNames();
        for (const string& member : srcMembers) {

            if (!dst.isMember(member))
                checkErr("missing field in dst: " + member, src, dst);

            else check(src[member], dst[member]);
        }

        const auto& dstMembers = dst.getMemberNames();
        for (const string& member : dstMembers)
            if (!src.isMember(member))
                checkErr("missing field in src: " + member, src, dst);
    }

    else if (src.type() == Json::arrayValue) {
        if (src.size() != dst.size())
            checkErr("size mismatch", src, dst);

        set<Json::Value> srcSet, dstSet;

        for (size_t i = 0; i < src.size(); ++i)
            srcSet.insert(src[i]);
        for (size_t i = 0; i < dst.size(); ++i)
            dstSet.insert(dst[i]);

        auto srcIt = srcSet.begin(), srcEnd = srcSet.end();
        auto dstIt = dstSet.begin(), dstEnd = dstSet.end();

        set<Json::Value> diff;
        set_difference(
                srcIt, srcEnd, dstIt, dstEnd, inserter(diff, diff.end()));
        for(const auto& json: diff)
            checkErr("Extra field in src", json, Json::Value());

        diff.clear();
        set_difference(
                dstIt, dstEnd, srcIt, srcEnd, inserter(diff, diff.end()));
        for(const auto& json: diff)
            checkErr("Extra field in dst", Json::Value(), json);
    }

    else if (src != dst)
        checkErr("value mismatch", src, dst);
}

void check(BidRequestSynth& synth)
{
    Json::Value first;
    {
        stringstream ss;
        synth.dump(ss);
        synth.load(ss);
        first = Json::parse(ss.str());
    }

    Json::Value second;
    {
        stringstream ss;
        synth.dump(ss);
        second = Json::parse(ss.str());
    }
    check(first, second);
}

BOOST_AUTO_TEST_CASE( recordLeafs )
{
    BidRequestSynth synth;

    Json::Value source = Json::parse(
            "{"
            "  'bool':true"
            ", 'int':123, 'ull':-4123576534534, 'float':123.5"
            ", 'str':\"This is a string and it's awesome\""
            "}"
        );

    synth.record(source);
    check(source, synth.generate());
    check(synth);
}

BOOST_AUTO_TEST_CASE( recordArray )
{
    BidRequestSynth synth;

    Json::Value source = Json::parse(
            "{"
            "  'ints':[123, 12345, 23, 1512]"
            ", 'floats':[12.32, 425.123, 1523.21]"
            ", 'strings':[\"bob\", \"oob\", \"bleh\"]"
            ", 'objs':[{'a':1}, {'b':2}, {'c':3}, {'d':4}]"
            ", 'matrix':[[123, 231], [412, 231], [2451, 25123]]"
            "}"
        );

    synth.record(source);

    cerr << synth.generate().toString() << endl;
    check(synth);
}

BOOST_AUTO_TEST_CASE( recordObject )
{
    BidRequestSynth synth;
    Json::Value source = Json::parse(
            "{"
            "  'obj':{"
            "      'nested':{'a':1, 'b':2, 'c':3}"
            "    , 'wee':[1]"
            "    , 'bleh':\"For the gloop!\""
            "  }"
            "}"
        );

    synth.record(source);
    check(source, synth.generate());
    check(synth);
}
