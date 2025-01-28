// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2022 The Namecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <names/applications.h>
#include <names/encoding.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(name_applications_tests)

BOOST_AUTO_TEST_CASE( namespace_detection )
{
    const valtype noSlash = DecodeName ("wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(noSlash) == NameNamespace::NonStandard);

    const valtype weirdNamespace = DecodeName ("nft/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(weirdNamespace) == NameNamespace::NonStandard);

    const valtype twoSlash = DecodeName ("d/d/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(twoSlash) == NameNamespace::NonStandard);

    const valtype empty = DecodeName ("", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(empty) == NameNamespace::NonStandard);

    const valtype domainValid = DecodeName ("d/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainValid) == NameNamespace::Domain);

    const valtype domainShortEnough = DecodeName ("d/wikileaks-wikileaks-wikileaks-wikileaks-wikileaks-wikileaks-wik", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainShortEnough) == NameNamespace::Domain);

    const valtype domainTooLong = DecodeName ("d/wikileaks-wikileaks-wikileaks-wikileaks-wikileaks-wikileaks-wiki", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainTooLong) == NameNamespace::NonStandard);

    const valtype domainAllCaps = DecodeName ("d/WIKILEAKS", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainAllCaps) == NameNamespace::NonStandard);

    const valtype domainOneCaps = DecodeName ("d/Wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainOneCaps) == NameNamespace::NonStandard);

    const valtype domainUnderscore = DecodeName ("d/wiki_leaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainUnderscore) == NameNamespace::NonStandard);

    const valtype domainHyphen = DecodeName ("d/wiki-leaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainHyphen) == NameNamespace::Domain);

    const valtype domainDoubleHyphen = DecodeName ("d/wiki--leaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainDoubleHyphen) == NameNamespace::NonStandard);

    const valtype domainStartHyphen = DecodeName ("d/-wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainStartHyphen) == NameNamespace::NonStandard);

    const valtype domainEndHyphen = DecodeName ("d/wikileaks-", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainEndHyphen) == NameNamespace::NonStandard);

    const valtype domainIDN = DecodeName ("d/xn--wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainIDN) == NameNamespace::Domain);

    const valtype domainNumeric = DecodeName ("d/123", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainNumeric) == NameNamespace::NonStandard);

    const valtype domainStartNumeric = DecodeName ("d/123wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainStartNumeric) == NameNamespace::Domain);

    const valtype domainNewline = DecodeName ("d/wiki\nleaks", NameEncoding::UTF8);
    BOOST_CHECK(NamespaceFromName(domainNewline) == NameNamespace::NonStandard);

    const valtype domainData = DecodeName ("dd/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(domainData) == NameNamespace::DomainData);

    const valtype identityValid = DecodeName ("id/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityValid) == NameNamespace::Identity);

    const valtype identityAllCaps = DecodeName ("id/WIKILEAKS", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityAllCaps) == NameNamespace::NonStandard);

    const valtype identityOneCaps = DecodeName ("id/Wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityOneCaps) == NameNamespace::NonStandard);

    const valtype identityUnderscore = DecodeName ("id/wiki_leaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityUnderscore) == NameNamespace::NonStandard);

    const valtype identityHyphen = DecodeName ("id/wiki-leaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityHyphen) == NameNamespace::Identity);

    const valtype identityDoubleHyphen = DecodeName ("id/wiki--leaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityDoubleHyphen) == NameNamespace::NonStandard);

    const valtype identityStartHyphen = DecodeName ("id/-wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityStartHyphen) == NameNamespace::NonStandard);

    const valtype identityEndHyphen = DecodeName ("id/wikileaks-", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityEndHyphen) == NameNamespace::NonStandard);

    const valtype identityIDN = DecodeName ("id/xn--wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityIDN) == NameNamespace::NonStandard);

    const valtype identityNumeric = DecodeName ("id/123", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityNumeric) == NameNamespace::Identity);

    const valtype identityStartNumeric = DecodeName ("id/123wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityStartNumeric) == NameNamespace::Identity);

    const valtype identityNewline = DecodeName ("id/wiki\nleaks", NameEncoding::UTF8);
    BOOST_CHECK(NamespaceFromName(identityNewline) == NameNamespace::NonStandard);

    const valtype identityData = DecodeName ("idd/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(NamespaceFromName(identityData) == NameNamespace::IdentityData);
}

BOOST_AUTO_TEST_CASE( name_description )
{
    const valtype domainValid = DecodeName ("d/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(DescFromName(domainValid, NameNamespace::Domain) == "wikileaks.bit");

    const valtype identityValid = DecodeName ("id/wikileaks", NameEncoding::ASCII);
    BOOST_CHECK(DescFromName(identityValid, NameNamespace::Identity) == "'id/wikileaks'");
}

BOOST_AUTO_TEST_CASE( valid_json )
{
    BOOST_CHECK_EQUAL(IsValidJSONOrEmptyString("{\"bar\": [1,2,3]}"), true);
    
    BOOST_CHECK_EQUAL(IsValidJSONOrEmptyString("{\foo:"), false);
    
    BOOST_CHECK_EQUAL(IsValidJSONOrEmptyString(""), true);
}

BOOST_AUTO_TEST_CASE( minimal_json )
{
    BOOST_CHECK_EQUAL(IsMinimalJSONOrEmptyString("{\"bar\":[1,2,3]}"), true);

    BOOST_CHECK_EQUAL(IsMinimalJSONOrEmptyString("{\"bar\":  [1,2,3]}"), false);

    BOOST_CHECK_EQUAL(IsMinimalJSONOrEmptyString("{\foo:"), false);

    BOOST_CHECK_EQUAL(IsMinimalJSONOrEmptyString(""), true);

    BOOST_CHECK_EQUAL(IsMinimalJSONOrEmptyString("{\"bar\":[1, 2, 3]}"), false);
}

BOOST_AUTO_TEST_CASE( domain_conversion ){
    BOOST_CHECK_EQUAL(ConvertDomainForms("namecoin"), "namecoin");

    BOOST_CHECK_EQUAL(ConvertDomainForms("namecoin.bit"), "namecoin");

    BOOST_CHECK_EQUAL(ConvertDomainForms("6E616D65636F696E0A"), "namecoin");
}

BOOST_AUTO_TEST_SUITE_END()
