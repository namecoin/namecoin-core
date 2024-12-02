// Copyright (c) 2021 Jeremy Rand
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <names/applications.h>

#include <names/encoding.h>

#include <regex>

#include <univalue.h>

#include <logging.h>

namespace
{

NameNamespace
NamespaceFromString (const std::string& str)
{
    if (str == "d/")
        return NameNamespace::Domain;
    if (str == "dd/")
        return NameNamespace::DomainData;
    if (str == "id/")
        return NameNamespace::Identity;
    if (str == "idd/")
        return NameNamespace::IdentityData;

    return NameNamespace::NonStandard;
}

std::string
NamespaceToString (NameNamespace ns)
{
    switch (ns)
    {
        case NameNamespace::Domain:
            return "d/";
        case NameNamespace::DomainData:
            return "dd/";
        case NameNamespace::Identity:
            return "id/";
        case NameNamespace::IdentityData:
            return "idd/";
        case NameNamespace::NonStandard:
            return "";
    }

    /* NameNamespace values are only ever created internally in the binary
    and not received externally (except as string).  Thus it should never
    happen (and if it does, is a severe bug) that we see an unexpected
    value.  */
    assert (false);
}

NameNamespace
PurportedNamespaceFromName (const std::string& name)
{
    const auto slashPos = name.find("/");
    if (slashPos == std::string::npos)
    {
        return NameNamespace::NonStandard;
    }

    const std::string namespaceStr = name.substr(0, slashPos+1);
    return NamespaceFromString(namespaceStr);
}

} // anonymous namespace

NameNamespace
NamespaceFromName (const std::string& name)
{
    const NameNamespace purported = PurportedNamespaceFromName(name);
    const std::string purportedStr = NamespaceToString(purported);
    const auto purportedLen = purportedStr.length();

    const std::string label = name.substr(purportedLen);

    if (label.empty())
    {
        return NameNamespace::NonStandard;
    }

    switch (purported)
    {
        case NameNamespace::Domain:
        {
            // Source: https://github.com/namecoin/proposals/blob/master/ifa-0001.md#keys
            if (label.length() > 63)
            {
                return NameNamespace::NonStandard;
            }

            // Source: https://github.com/namecoin/proposals/blob/master/ifa-0001.md#keys
            // The ^ and $ are omitted relative to the spec because
            // std::regex_match implies them.
            std::regex domainPattern("(xn--)?[a-z0-9]+(-[a-z0-9]+)*");
            if (!std::regex_match (label, domainPattern))
            {
                return NameNamespace::NonStandard;
            }

            // Reject digits-only labels
            // Source: https://github.com/namecoin/proposals/blob/master/ifa-0001.md#keys
            std::regex digitsOnly("[0-9]+");
            if (std::regex_match (label, digitsOnly))
            {
                return NameNamespace::NonStandard;
            }

            return NameNamespace::Domain;
        }
        case NameNamespace::Identity:
        {
            // Max id/ identifier length is 255 chars according to wiki spec.
            // But we don't need to check for this, because that's also the max
            // length of an identifier under the Namecoin consensus rules.

            // Same as d/ regex but without IDN prefix.
            // TODO: this doesn't exactly match the https://wiki.namecoin.org spec.
            std::regex identityPattern("[a-z0-9]+(-[a-z0-9]+)*");
            if (!std::regex_match (label, identityPattern))
            {
                return NameNamespace::NonStandard;
            }

            return NameNamespace::Identity;
        }
        case NameNamespace::DomainData:
        case NameNamespace::IdentityData:
        case NameNamespace::NonStandard:
            return purported;
    }

    /* NameNamespace values are only ever created internally in the binary
    and not received externally (except as string).  Thus it should never
    happen (and if it does, is a severe bug) that we see an unexpected
    value.  */
    assert (false);
}

NameNamespace
NamespaceFromName (const valtype& data)
{
    std::string name;

    try
    {
        name = EncodeName (data, NameEncoding::ASCII);
    }
    catch (const InvalidNameString& exc)
    {
        return NameNamespace::NonStandard;
    }

    return NamespaceFromName(name);
}

std::string
DescFromName (const valtype& name, NameNamespace ns)
{
    switch (ns)
    {
        case NameNamespace::Domain:
        {
            const std::string nsStr = NamespaceToString(ns);
            const auto nsLen = nsStr.length();

            const std::string nameStr = EncodeName (name, NameEncoding::ASCII);
            const std::string label = nameStr.substr(nsLen);

            return label + ".bit";
        }
        default:
        {
            return EncodeNameForMessage(name);
        }
    }
}

bool
IsValidJSONOrEmptyString (const std::string& text){
    UniValue v;

    return text.empty() || v.read(text);
}    

bool
IsMinimalJSONOrEmptyString (const std::string& text){
    UniValue v;
    if(text.empty()){
        return true;
    } 

    if(!v.read(text)){ 
        return false;
    } 

    const std::string minimalJSON = GetMinimalJSON(text);
    
    const bool isMinimal = (text == minimalJSON);

    if(!isMinimal){
        LogDebug(BCLog::NAMES, "Minimalised JSON string is: %s \n", minimalJSON);
    }

    return isMinimal;
}

std::string
GetMinimalJSON (const std::string& text){
    UniValue v;

    v.read(text);

    return v.write(0,0);
}

std::string
ConvertDomainForms (const std::string& domain){
    if(domain.ends_with(".bit")){
        return domain.substr(0, domain.size()-4);
    } else if (domain.starts_with("0x")) {
        std::string cutdomain = domain.substr(2, domain.size());
        int hexval = std::stoi(cutdomain, 0, 16);
        return std::to_string(hexval);
    } else {
        return domain;
    }
}
