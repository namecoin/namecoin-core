// Copyright (c) 2021 Jeremy Rand
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef H_BITCOIN_NAMES_APPLICATIONS
#define H_BITCOIN_NAMES_APPLICATIONS

#include <script/script.h>

#include <string>

enum class NameNamespace
{
    Domain,
    DomainData,
    Identity,
    IdentityData,
    NonStandard,
};

NameNamespace NamespaceFromName (const std::string& name);
NameNamespace NamespaceFromName (const valtype& data);

std::string DescFromName (const valtype& name, NameNamespace ns);

bool IsValidJSONOrEmptyString (const std::string& text);

bool IsMinimalJSONOrEmptyString (const std::string& text);

std::string GetMinimalJSON (const std::string& text);

std::string ConvertDomainForms (const std::string& domain);

#endif // H_BITCOIN_NAMES_APPLICATIONS
