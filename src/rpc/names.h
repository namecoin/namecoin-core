// Copyright (c) 2014-2025 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_NAMES_H
#define BITCOIN_RPC_NAMES_H

#include <bitcoin-build-config.h>

#include <names/encoding.h>
#include <rpc/util.h>
#include <script/script.h>
#include <span.h>

#include <string>
#include <vector>

/** Default value for the -allowexpired argument.  */
static constexpr bool DEFAULT_ALLOWEXPIRED = false;

class ChainstateManager;
class CNameData;
class COutPoint;
class CRPCCommand;
class CScript;
class UniValue;

UniValue getNameInfo (const UniValue& options,
                      const valtype& name, const valtype& value,
                      const COutPoint& outp, const CScript& addr);
UniValue getNameInfo (const ChainstateManager& chainman,
                      const UniValue& options,
                      const valtype& name, const CNameData& data);
void addExpirationInfo (const ChainstateManager& chainman,
                        int height, UniValue& data);

std::span<const CRPCCommand> GetNameRPCCommands ();

#ifdef ENABLE_WALLET
namespace wallet {
class CWallet;
} // namespace wallet
void addOwnershipInfo (const CScript& addr,
                       const wallet::CWallet* pwallet,
                       UniValue& data);
#endif // ENABLE_WALLET

/**
 * Decodes a name given through the RPC interface and throws a
 * JSONRPCError if it is invalid for the requested encoding.  The encoding
 * is extracted from the options object if it is there with the "name_encoding"
 * key, or else the configured default name encoding it used.
 */
valtype DecodeNameFromRPCOrThrow (const UniValue& val, const UniValue& opt);

/**
 * Decodes a value given through the RPC interface and throws an error if it
 * is invalid.  This is the same as DecodeNameFromRPCOrThrow, except that it
 * extracts the "value_encoding" from the options and uses the default encoding
 * for values instead of names.
 */
valtype DecodeValueFromRPCOrThrow (const UniValue& val, const UniValue& opt);

/**
 * RPCResult for the "nameOp" field that is optionally returned from
 * some RPCs that decode scripts.
 */
extern const RPCResult NameOpResult;

/**
 * Builder class for the RPC results for methods that return information about
 * names (like name_show, name_scan, name_pending or name_list).  Since the
 * exact fields contained depend on the case, this class
 * provides a simple and fluent interface to build the right help text for
 * each case.
 */
class NameInfoHelp
{

private:

  /** Result fields that have already been added.  */
  std::vector<RPCResult> fields;

public:

  explicit NameInfoHelp ();

  NameInfoHelp& withExpiration ();

  /**
   * Adds a new field for the result.
   */
  NameInfoHelp&
  withField (const RPCResult& field)
  {
    fields.push_back (field);
    return *this;
  }

  /**
   * Constructs the final RPCResult for all fields added.
   */
  RPCResult
  finish ()
  {
    return RPCResult (RPCResult::Type::OBJ, "", "", fields);
  }

};

/**
 * Builder class for the help of the "options" argument for name RPCs.
 */
class NameOptionsHelp
{

private:

  /** Inner RPCArgs for RPCHelpMan.  */
  std::vector<RPCArg> innerArgs;

public:

  NameOptionsHelp ();

  /**
   * Adds the options for write-type RPCs (e.g. name_update).
   */
  NameOptionsHelp& withWriteOptions ();

  NameOptionsHelp& withNameEncoding ();
  NameOptionsHelp& withValueEncoding ();
  NameOptionsHelp& withByHash ();

  /**
   * Variant of withField that also adds the innerArgs field correctly.
   */
  NameOptionsHelp& withArg (const std::string& name, RPCArg::Type type,
                            const std::string& doc);

  /**
   * Variant of withField that adds inner arguments inside.
   */
  NameOptionsHelp& withArg (const std::string& name, RPCArg::Type type,
                            const std::string& doc,
                            const std::vector<RPCArg> inner);

  /**
   * Adds a new inner argument with a default value.
   */
  NameOptionsHelp& withArg (const std::string& name, RPCArg::Type type,
                            const std::string& defaultValue,
                            const std::string& doc);

  /**
   * Adds a new inner argument with a default value and also inner
   * arguments inside the argument itself.
   */
  NameOptionsHelp& withArg (const std::string& name, RPCArg::Type type,
                            const std::string& defaultValue,
                            const std::string& doc,
                            const std::vector<RPCArg> inner);

  /**
   * Constructs the RPCArg object for the options argument described by this
   * builder instance.
   */
  RPCArg buildRpcArg () const;

};

#endif // BITCOIN_RPC_NAMES_H
