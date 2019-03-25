// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "base58.h"
#include "chain.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "../vote.h"
#include "net.h"
#include "policy/policy.h"
#include "policy/rbf.h"
#include "rpc/server.h"
#include "script/sign.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "wallet.h"
#include "walletdb.h"
#include "validation.h"
#include "coincontrol.h"

#include "script/script.h"
#include "rpc/rawtransaction.h"

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

#include "rpcwallet.h"
#include "consensus/merkle.h"
#include "miner.h"
#include "../lbtc.pb.h"
#include "../dpos_db.h"
#include "../module.h"
#include "../token_evaluator.h"
#include "../token_db.h"
#include "../lbtc.pb.h"
#include "../dpos_db.h"
#include "../module.h"

using namespace std;

int64_t nWalletUnlockTime;
static CCriticalSection cs_nWalletUnlockTime;

std::string HelpRequiringPassphrase()
{
    return pwalletMain && pwalletMain->IsCrypted()
        ? "\nRequires wallet passphrase to be set with walletpassphrase call."
        : "";
}

bool EnsureWalletIsAvailable(bool avoidException)
{
    if (!pwalletMain)
    {
        if (!avoidException)
            throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (disabled)");
        else
            return false;
    }
    return true;
}

void EnsureWalletIsUnlocked()
{
    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
}

void WalletTxToJSON(const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain();
    entry.push_back(Pair("confirmations", confirms));
    if (wtx.IsCoinBase())
        entry.push_back(Pair("generated", true));
    if (confirms > 0)
    {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
        entry.push_back(Pair("blocktime", mapBlockIndex[wtx.hashBlock]->GetBlockTime()));
    } else {
        entry.push_back(Pair("trusted", wtx.IsTrusted()));
    }
    uint256 hash = wtx.GetHash();
    entry.push_back(Pair("txid", hash.GetHex()));
    UniValue conflicts(UniValue::VARR);
    BOOST_FOREACH(const uint256& conflict, wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.push_back(Pair("walletconflicts", conflicts));
    entry.push_back(Pair("time", wtx.GetTxTime()));
    entry.push_back(Pair("timereceived", (int64_t)wtx.nTimeReceived));

    // Add opt-in RBF status
    std::string rbfStatus = "no";
    if (confirms <= 0) {
        LOCK(mempool.cs);
        RBFTransactionState rbfState = IsRBFOptIn(wtx, mempool);
        if (rbfState == RBF_TRANSACTIONSTATE_UNKNOWN)
            rbfStatus = "unknown";
        else if (rbfState == RBF_TRANSACTIONSTATE_REPLACEABLE_BIP125)
            rbfStatus = "yes";
    }
    entry.push_back(Pair("bip125-replaceable", rbfStatus));

    BOOST_FOREACH(const PAIRTYPE(string,string)& item, wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

string AccountFromValue(const UniValue& value)
{
    string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

UniValue getnewaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
            "getnewaddress ( \"account\" )\n"
            "\nReturns a new Bitcoin address for receiving payments.\n"
            "If 'account' is specified (DEPRECATED), it is added to the address book \n"
            "so payments received with the address will be credited to 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, optional) DEPRECATED. The account name for the address to be linked to. If not provided, the default account \"\" is used. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created if there is no account by the given name.\n"
            "\nResult:\n"
            "\"address\"    (string) The new bitcoin address\n"
            "\nExamples:\n"
            + HelpExampleCli("getnewaddress", "")
            + HelpExampleRpc("getnewaddress", "")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    string strAccount;
    if (request.params.size() > 0)
        strAccount = AccountFromValue(request.params[0]);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    CKeyID keyID = newKey.GetID();

    pwalletMain->SetAddressBook(keyID, strAccount, "receive");

    return CBitcoinAddress(keyID).ToString();
}


CBitcoinAddress GetAccountAddress(string strAccount, bool bForceNew=false)
{
    CPubKey pubKey;
    if (!pwalletMain->GetAccountPubkey(pubKey, strAccount, bForceNew)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }

    return CBitcoinAddress(pubKey.GetID());
}

UniValue getaccountaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getaccountaddress \"account\"\n"
            "\nDEPRECATED. Returns the current Bitcoin address for receiving payments to this account.\n"
            "\nArguments:\n"
            "1. \"account\"       (string, required) The account name for the address. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created and a new address created  if there is no account by the given name.\n"
            "\nResult:\n"
            "\"address\"          (string) The account bitcoin address\n"
            "\nExamples:\n"
            + HelpExampleCli("getaccountaddress", "")
            + HelpExampleCli("getaccountaddress", "\"\"")
            + HelpExampleCli("getaccountaddress", "\"myaccount\"")
            + HelpExampleRpc("getaccountaddress", "\"myaccount\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    string strAccount = AccountFromValue(request.params[0]);

    UniValue ret(UniValue::VSTR);

    ret = GetAccountAddress(strAccount).ToString();
    return ret;
}


UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
            "getrawchangeaddress\n"
            "\nReturns a new Bitcoin address, for receiving change.\n"
            "This is for use with raw transactions, NOT normal use.\n"
            "\nResult:\n"
            "\"address\"    (string) The address\n"
            "\nExamples:\n"
            + HelpExampleCli("getrawchangeaddress", "")
            + HelpExampleRpc("getrawchangeaddress", "")
       );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    CReserveKey reservekey(pwalletMain);
    CPubKey vchPubKey;
    if (!reservekey.GetReservedKey(vchPubKey))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    reservekey.KeepKey();

    CKeyID keyID = vchPubKey.GetID();

    return CBitcoinAddress(keyID).ToString();
}


UniValue setaccount(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "setaccount \"address\" \"account\"\n"
            "\nDEPRECATED. Sets the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The bitcoin address to be associated with an account.\n"
            "2. \"account\"         (string, required) The account to assign the address to.\n"
            "\nExamples:\n"
            + HelpExampleCli("setaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"tabby\"")
            + HelpExampleRpc("setaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"tabby\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    string strAccount;
    if (request.params.size() > 1)
        strAccount = AccountFromValue(request.params[1]);

    // Only add the account if the address is yours.
    if (IsMine(*pwalletMain, address.Get()))
    {
        // Detect when changing the account of an address that is the 'unused current key' of another account:
        if (pwalletMain->mapAddressBook.count(address.Get()))
        {
            string strOldAccount = pwalletMain->mapAddressBook[address.Get()].name;
            if (address == GetAccountAddress(strOldAccount))
                GetAccountAddress(strOldAccount, true);
        }
        pwalletMain->SetAddressBook(address.Get(), strAccount, "receive");
    }
    else
        throw JSONRPCError(RPC_MISC_ERROR, "setaccount can only be used with own address");

    return NullUniValue;
}


UniValue getaccount(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getaccount \"address\"\n"
            "\nDEPRECATED. Returns the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The bitcoin address for account lookup.\n"
            "\nResult:\n"
            "\"accountname\"        (string) the account address\n"
            "\nExamples:\n"
            + HelpExampleCli("getaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"")
            + HelpExampleRpc("getaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    string strAccount;
    map<CTxDestination, CAddressBookData>::iterator mi = pwalletMain->mapAddressBook.find(address.Get());
    if (mi != pwalletMain->mapAddressBook.end() && !(*mi).second.name.empty())
        strAccount = (*mi).second.name;
    return strAccount;
}


UniValue getaddressesbyaccount(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getaddressesbyaccount \"account\"\n"
            "\nDEPRECATED. Returns the list of addresses for the given account.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, required) The account name.\n"
            "\nResult:\n"
            "[                     (json array of string)\n"
            "  \"address\"         (string) a bitcoin address associated with the given account\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getaddressesbyaccount", "\"tabby\"")
            + HelpExampleRpc("getaddressesbyaccount", "\"tabby\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount = AccountFromValue(request.params[0]);

    // Find all addresses that have the given account
    UniValue ret(UniValue::VARR);
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, CAddressBookData)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strName = item.second.name;
        if (strName == strAccount)
            ret.push_back(address.ToString());
    }
    return ret;
}

static void SendMoneyNew(const CTxDestination &address, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, CTxDestination* pfromAddress = NULL)
{
    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    CTxDestination fromAddress;
    CAmount balance = 0;
    if(pfromAddress) {
        fromAddress = *pfromAddress;
        balance = pwalletMain->GetAddressBalance(fromAddress);
        if (nValue > balance)
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
    } else {
        if(pwalletMain->SelectAddress(fromAddress, balance, nValue) == false)
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
    }

    if (pwalletMain->GetBroadcastTransactions() && !g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    // Parse Bitcoin address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwalletMain);
    CAmount nFeeRequired;
    std::string strError;
    vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);
    if (!pwalletMain->CreateTransaction(vecSend, wtxNew, reservekey, nFeeRequired, nChangePosRet, strError, NULL, true, &fromAddress, NULL)) {
    //if (!pwalletMain->CreateNoFeeTransaction(vecSend, wtxNew, reservekey, nFeeRequired, nChangePosRet, strError, NULL, true, &fromAddress, NULL)) {
        if (!fSubtractFeeFromAmount && nValue + nFeeRequired > balance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    CValidationState state;
    if (!pwalletMain->CommitTransaction(wtxNew, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
}

UniValue sendtoaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw runtime_error(
            "sendtoaddress \"address\" amount ( \"comment\" \"comment_to\" subtractfeefromamount )\n"
            "\nSend an amount to a given address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"            (string, required) The bitcoin address to send to.\n"
            "2. \"amount\"             (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "3. \"comment\"            (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment_to\"         (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "5. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less bitcoins than you enter in the amount field.\n"
            "\nResult:\n"
            "\"txid\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"donation\" \"seans outpost\"")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"\" \"\" true")
            + HelpExampleRpc("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", 0.1, \"donation\", \"seans outpost\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    // Wallet comments
    CWalletTx wtx;
    if (request.params.size() > 2 && !request.params[2].isNull() && !request.params[2].get_str().empty())
        wtx.mapValue["comment"] = request.params[2].get_str();
    if (request.params.size() > 3 && !request.params[3].isNull() && !request.params[3].get_str().empty())
        wtx.mapValue["to"]      = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (request.params.size() > 4)
        fSubtractFeeFromAmount = request.params[4].get_bool();

    EnsureWalletIsUnlocked();

    SendMoneyNew(address.Get(), nAmount, fSubtractFeeFromAmount, wtx);

    return wtx.GetHash().GetHex();
}

UniValue sendfromaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 6)
        throw runtime_error(
            "sendfromaddress \"from_address\" \"to_address\" amount ( \"comment\" \"comment_to\" subtractfeefromamount )\n"
            "\nSend an amount from a given address to a given address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"from_address\"       (string, required) The bitcoin address to from to.\n"
            "2. \"to_address\"         (string, required) The bitcoin address to send to.\n"
            "3. \"amount\"             (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "4. \"comment\"            (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "5. \"comment_to\"         (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "6. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less bitcoins than you enter in the amount field.\n"
            "\nResult:\n"
            "\"txid\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("sendfromaddress", "\"1CKraLMPjXpJwutrsy7MsYxXRigoRBk481\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1")
            + HelpExampleCli("sendfromaddress", "\"1CKraLMPjXpJwutrsy7MsYxXRigoRBk481\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"donation\" \"seans outpost\"")
            + HelpExampleCli("sendfromaddress", "\"1CKraLMPjXpJwutrsy7MsYxXRigoRBk481\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"\" \"\" true")
            + HelpExampleRpc("sendfromaddress", "\"1CKraLMPjXpJwutrsy7MsYxXRigoRBk481\", \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"0.1\", \"donation\", \"seans outpost\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CBitcoinAddress fromAddress(request.params[0].get_str());
    if (!fromAddress.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    CBitcoinAddress address(request.params[1].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    // Amount
    CAmount nAmount = AmountFromValue(request.params[2]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    // Wallet comments
    CWalletTx wtx;
    if (request.params.size() > 3 && !request.params[3].isNull() && !request.params[2].get_str().empty())
        wtx.mapValue["comment"] = request.params[3].get_str();
    if (request.params.size() > 4 && !request.params[4].isNull() && !request.params[3].get_str().empty())
        wtx.mapValue["to"]      = request.params[4].get_str();

    bool fSubtractFeeFromAmount = false;
    if (request.params.size() > 5)
        fSubtractFeeFromAmount = request.params[5].get_bool();

    EnsureWalletIsUnlocked();

    auto addr = fromAddress.Get();
    SendMoneyNew(address.Get(), nAmount, fSubtractFeeFromAmount, wtx, &addr);

    return wtx.GetHash().GetHex();
}

UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp)
        throw runtime_error(
            "listaddressgroupings\n"
            "\nLists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions\n"
            "\nResult:\n"
            "[\n"
            "  [\n"
            "    [\n"
            "      \"address\",            (string) The bitcoin address\n"
            "      amount,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"account\"             (string, optional) DEPRECATED. The account\n"
            "    ]\n"
            "    ,...\n"
            "  ]\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listaddressgroupings", "")
            + HelpExampleRpc("listaddressgroupings", "")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    map<CTxDestination, CAmount> balances = pwalletMain->GetAddressBalances();
    BOOST_FOREACH(set<CTxDestination> grouping, pwalletMain->GetAddressGroupings())
    {
        UniValue jsonGrouping(UniValue::VARR);
        BOOST_FOREACH(CTxDestination address, grouping)
        {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(CBitcoinAddress(address).ToString());
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwalletMain->mapAddressBook.find(CBitcoinAddress(address).Get()) != pwalletMain->mapAddressBook.end())
                    addressInfo.push_back(pwalletMain->mapAddressBook.find(CBitcoinAddress(address).Get())->second.name);
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

UniValue signmessage(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw runtime_error(
            "signmessage \"address\" \"message\"\n"
            "\nSign a message with the private key of an address"
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The bitcoin address to use for the private key.\n"
            "2. \"message\"         (string, required) The message to create a signature of.\n"
            "\nResult:\n"
            "\"signature\"          (string) The signature of the message encoded in base 64\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n"
            + HelpExampleCli("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"signature\" \"my message\"") +
            "\nAs json rpc\n"
            + HelpExampleRpc("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"my message\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    string strAddress = request.params[0].get_str();
    string strMessage = request.params[1].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if (!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(&vchSig[0], vchSig.size());
}

UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "getreceivedbyaddress \"address\" ( minconf )\n"
            "\nReturns the total amount received by the given address in transactions with at least minconf confirmations.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The bitcoin address for transactions.\n"
            "2. minconf             (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "\nResult:\n"
            "amount   (numeric) The total amount in " + CURRENCY_UNIT + " received at this address.\n"
            "\nExamples:\n"
            "\nThe amount from transactions with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 0") +
            "\nThe amount with at least 6 confirmation, very safe\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 6") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", 6")
       );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Bitcoin address
    CBitcoinAddress address = CBitcoinAddress(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");
    CScript scriptPubKey = GetScriptForDestination(address.Get());
    if (!IsMine(*pwalletMain, scriptPubKey))
        return ValueFromAmount(0);

    // Minimum confirmations
    int nMinDepth = 1;
    if (request.params.size() > 1)
        nMinDepth = request.params[1].get_int();

    // Tally
    CAmount nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}


UniValue getreceivedbyaccount(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "getreceivedbyaccount \"account\" ( minconf )\n"
            "\nDEPRECATED. Returns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.\n"
            "\nArguments:\n"
            "1. \"account\"      (string, required) The selected account, may be the default account using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this account.\n"
            "\nExamples:\n"
            "\nAmount received by the default account with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbyaccount", "\"\"") +
            "\nAmount received at the tabby account including unconfirmed amounts with zero confirmations\n"
            + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmation, very safe\n"
            + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 6") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("getreceivedbyaccount", "\"tabby\", 6")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Minimum confirmations
    int nMinDepth = 1;
    if (request.params.size() > 1)
        nMinDepth = request.params[1].get_int();

    // Get the set of pub keys assigned to account
    string strAccount = AccountFromValue(request.params[0]);
    set<CTxDestination> setAddress = pwalletMain->GetAccountAddresses(strAccount);

    // Tally
    CAmount nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.tx->vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwalletMain, address) && setAddress.count(address))
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
        }
    }

    return ValueFromAmount(nAmount);
}


UniValue getbalance(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 3)
        throw runtime_error(
            "getbalance ( \"account\" minconf include_watchonly )\n"
            "\nIf account is not specified, returns the server's total available balance.\n"
            "If account is specified (DEPRECATED), returns the balance in the account.\n"
            "Note that the account \"\" is not the same as leaving the parameter out.\n"
            "The server total may be different to the balance in the default \"\" account.\n"
            "\nArguments:\n"
            "1. \"account\"         (string, optional) DEPRECATED. The account string may be given as a\n"
            "                     specific account name to find the balance associated with wallet keys in\n"
            "                     a named account, or as the empty string (\"\") to find the balance\n"
            "                     associated with wallet keys not in any named account, or as \"*\" to find\n"
            "                     the balance associated with all wallet keys regardless of account.\n"
            "                     When this option is specified, it calculates the balance in a different\n"
            "                     way than when it is not specified, and which can count spends twice when\n"
            "                     there are conflicting pending transactions (such as those created by\n"
            "                     the bumpfee command), temporarily resulting in low or even negative\n"
            "                     balances. In general, account balance calculation is not considered\n"
            "                     reliable and has resulted in confusing outcomes, so it is recommended to\n"
            "                     avoid passing this argument.\n"
            "2. minconf           (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. include_watchonly (bool, optional, default=false) Also include balance in watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this account.\n"
            "\nExamples:\n"
            "\nThe total amount in the wallet\n"
            + HelpExampleCli("getbalance", "") +
            "\nThe total amount in the wallet at least 5 blocks confirmed\n"
            + HelpExampleCli("getbalance", "\"*\" 6") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("getbalance", "\"*\", 6")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (request.params.size() == 0)
        return  ValueFromAmount(pwalletMain->GetBalance());

    int nMinDepth = 1;
    if (request.params.size() > 1)
        nMinDepth = request.params[1].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if(request.params.size() > 2)
        if(request.params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (request.params[0].get_str() == "*") {
        // Calculate total balance in a very different way from GetBalance().
        // The biggest difference is that GetBalance() sums up all unspent
        // TxOuts paying to the wallet, while this sums up both spent and
        // unspent TxOuts paying to the wallet, and then subtracts the values of
        // TxIns spending from the wallet. This also has fewer restrictions on
        // which unconfirmed transactions are considered trusted.
        CAmount nBalance = 0;
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
        {
            const CWalletTx& wtx = (*it).second;
            if (!CheckFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 || wtx.GetDepthInMainChain() < 0)
                continue;

            CAmount allFee;
            string strSentAccount;
            list<COutputEntry> listReceived;
            list<COutputEntry> listSent;
            wtx.GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);
            if (wtx.GetDepthInMainChain() >= nMinDepth)
            {
                BOOST_FOREACH(const COutputEntry& r, listReceived)
                    nBalance += r.amount;
            }
            BOOST_FOREACH(const COutputEntry& s, listSent)
                nBalance -= s.amount;
            nBalance -= allFee;
        }
        return  ValueFromAmount(nBalance);
    }

    string strAccount = AccountFromValue(request.params[0]);

    CAmount nBalance = pwalletMain->GetAccountBalance(strAccount, nMinDepth, filter);

    return ValueFromAmount(nBalance);
}

UniValue getunconfirmedbalance(const JSONRPCRequest &request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "getunconfirmedbalance\n"
                "Returns the server's total unconfirmed balance\n");

    LOCK2(cs_main, pwalletMain->cs_wallet);

    return ValueFromAmount(pwalletMain->GetUnconfirmedBalance());
}


UniValue movecmd(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 5)
        throw runtime_error(
            "move \"fromaccount\" \"toaccount\" amount ( minconf \"comment\" )\n"
            "\nDEPRECATED. Move a specified amount from one account in your wallet to another.\n"
            "\nArguments:\n"
            "1. \"fromaccount\"   (string, required) The name of the account to move funds from. May be the default account using \"\".\n"
            "2. \"toaccount\"     (string, required) The name of the account to move funds to. May be the default account using \"\".\n"
            "3. amount            (numeric) Quantity of " + CURRENCY_UNIT + " to move between accounts.\n"
            "4. (dummy)           (numeric, optional) Ignored. Remains for backward compatibility.\n"
            "5. \"comment\"       (string, optional) An optional comment, stored in the wallet only.\n"
            "\nResult:\n"
            "true|false           (boolean) true if successful.\n"
            "\nExamples:\n"
            "\nMove 0.01 " + CURRENCY_UNIT + " from the default account to the account named tabby\n"
            + HelpExampleCli("move", "\"\" \"tabby\" 0.01") +
            "\nMove 0.01 " + CURRENCY_UNIT + " timotei to akiko with a comment and funds have 6 confirmations\n"
            + HelpExampleCli("move", "\"timotei\" \"akiko\" 0.01 6 \"happy birthday!\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("move", "\"timotei\", \"akiko\", 0.01, 6, \"happy birthday!\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strFrom = AccountFromValue(request.params[0]);
    string strTo = AccountFromValue(request.params[1]);
    CAmount nAmount = AmountFromValue(request.params[2]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
    if (request.params.size() > 3)
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)request.params[3].get_int();
    string strComment;
    if (request.params.size() > 4)
        strComment = request.params[4].get_str();

    if (!pwalletMain->AccountMove(strFrom, strTo, nAmount, strComment))
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    return true;
}

UniValue sendmany(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 4 || request.params.size() > 7)
        throw runtime_error(
            "sendmany \"fromaccount\" {\"address\":amount,...} ( minconf \"comment\" [\"address\",...] )\n"
            "\nSend multiple times. Amounts are double-precision floating point numbers."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
            "1. \"fromaccount\"         (string, required) DEPRECATED. The account to send the funds from. Should be \"\" for the default account\n"
            "2. \"amounts\"             (string, required) A json object with addresses and amounts\n"
            "    {\n"
            "      \"address\":amount   (numeric or string) The bitcoin address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value\n"
            "      ,...\n"
            "    }\n"
            "3. \"fromaddress\"         (string, required) The address to send the funds from. If the address is empty, auto select address\n"
            "4. \"changeaddress\"       (string, required) The change address. If the address is empty, fromaddress is changeaddress\n"
            "5. minconf                 (numeric, optional, default=1) Only use the balance confirmed at least this many times.\n"
            "6. \"comment\"             (string, optional) A comment\n"
            "7. subtractfeefrom         (array, optional) A json array with addresses.\n"
            "                           The fee will be equally deducted from the amount of each selected address.\n"
            "                           Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
            "                           If no addresses are specified here, the sender pays the fee.\n"
            "    [\n"
            "      \"address\"          (string) Subtract fee from this address\n"
            "      ,...\n"
            "    ]\n"
            "\nResult:\n"
            "\"txid\"                   (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
            "                                    the number of addresses.\n"
            "\nExamples:\n"
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" \"\" \"\"") +
            "\nSend two amounts to two different addresses with fromaddress and changeaddress:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" \"13wU6wmLoBshNqmBi9Ur8e92eF1eH3kxPP\" \"13wU6wmLoBshNqmBi9Ur8e92eF1eH3kxPP\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" \"\"  \"\" 6 \"testing\"") +
            "\nSend two amounts to two different addresses, subtract fee from amount:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" \"\" \"\" 1 \"\" \"[\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\",\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\"]\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendmany", "\"\", {\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\":0.01,\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\":0.02}, \"\", \"\", 6, \"testing\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (pwalletMain->GetBroadcastTransactions() && !g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    string strAccount = AccountFromValue(request.params[0]);
    UniValue sendTo = request.params[1].get_obj();
    string strFromAddress = request.params[2].get_str();
    if(strFromAddress.empty() == false) {
        if(CBitcoinAddress(strFromAddress).IsValid() == false) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Bitcoin address: ")+strFromAddress);
        }
    }
    string strChangeAddress = request.params[3].get_str();
    if(strChangeAddress.empty() == false) {
        if(CBitcoinAddress(strChangeAddress).IsValid() == false) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Bitcoin address: ")+strChangeAddress);
        }
    }

    int nMinDepth = 1;
    if (request.params.size() > 4)
        nMinDepth = request.params[4].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (request.params.size() > 5 && !request.params[5].isNull() && !request.params[5].get_str().empty())
        wtx.mapValue["comment"] = request.params[5].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (request.params.size() > 6)
        subtractFeeFromAmount = request.params[6].get_array();

    set<CBitcoinAddress> setAddress;
    vector<CRecipient> vecSend;

    CAmount totalAmount = 0;
    vector<string> keys = sendTo.getKeys();
    BOOST_FOREACH(const string& name_, keys)
    {
        CBitcoinAddress address(name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Bitcoin address: ")+name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address.Get());
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
        totalAmount += nAmount;

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == name_)
                fSubtractFeeFromAmount = true;
        }

        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount};
        vecSend.push_back(recipient);
    }

    EnsureWalletIsUnlocked();

    CTxDestination fromAddress;
    if(strFromAddress.empty() == false) {
        fromAddress = CBitcoinAddress(strFromAddress).Get();
        if(pwalletMain->GetAddressBalance(fromAddress) < totalAmount) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds from a simple address");
        }
    } else {
        CAmount balance = 0;
        if(pwalletMain->SelectAddress(fromAddress, balance, totalAmount, nMinDepth) == false) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds from a simple address");
        }
    }

    // Send
    CReserveKey keyChange(pwalletMain);
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    string strFailReason;
    CCoinControl cControl;
    cControl.destChange = CBitcoinAddress(strChangeAddress).Get();
    bool fCreated = pwalletMain->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, nChangePosRet, strFailReason, &cControl, true, &fromAddress, NULL);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    CValidationState state;
    if (!pwalletMain->CommitTransaction(wtx, keyChange, g_connman.get(), state)) {
        strFailReason = strprintf("Transaction commit failed:: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return wtx.GetHash().GetHex();
}

// Defined in rpc/misc.cpp
extern CScript _createmultisig_redeemScript(const UniValue& params);

UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
    {
        string msg = "addmultisigaddress nrequired [\"key\",...] ( \"account\" )\n"
            "\nAdd a nrequired-to-sign multisignature address to the wallet.\n"
            "Each key is a Bitcoin address or hex-encoded public key.\n"
            "If 'account' is specified (DEPRECATED), assign address to that account.\n"

            "\nArguments:\n"
            "1. nrequired        (numeric, required) The number of required signatures out of the n keys or addresses.\n"
            "2. \"keys\"         (string, required) A json array of bitcoin addresses or hex-encoded public keys\n"
            "     [\n"
            "       \"address\"  (string) bitcoin address or hex-encoded public key\n"
            "       ...,\n"
            "     ]\n"
            "3. \"account\"      (string, optional) DEPRECATED. An account to assign the addresses to.\n"

            "\nResult:\n"
            "\"address\"         (string) A bitcoin address associated with the keys.\n"

            "\nExamples:\n"
            "\nAdd a multisig address from 2 addresses\n"
            + HelpExampleCli("addmultisigaddress", "2 \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"")
        ;
        throw runtime_error(msg);
    }

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount;
    if (request.params.size() > 2)
        strAccount = AccountFromValue(request.params[2]);

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(request.params);
    CScriptID innerID(inner);
    pwalletMain->AddCScript(inner);

    pwalletMain->SetAddressBook(innerID, strAccount, "send");
    return CBitcoinAddress(innerID).ToString();
}

class Witnessifier : public boost::static_visitor<bool>
{
public:
    CScriptID result;

    bool operator()(const CNoDestination &dest) const { return false; }

    bool operator()(const CKeyID &keyID) {
        CPubKey pubkey;
        if (pwalletMain) {
            CScript basescript = GetScriptForDestination(keyID);
            isminetype typ;
            typ = IsMine(*pwalletMain, basescript, SIGVERSION_WITNESS_V0);
            if (typ != ISMINE_SPENDABLE && typ != ISMINE_WATCH_SOLVABLE)
                return false;
            CScript witscript = GetScriptForWitness(basescript);
            pwalletMain->AddCScript(witscript);
            result = CScriptID(witscript);
            return true;
        }
        return false;
    }

    bool operator()(const CScriptID &scriptID) {
        CScript subscript;
        if (pwalletMain && pwalletMain->GetCScript(scriptID, subscript)) {
            int witnessversion;
            std::vector<unsigned char> witprog;
            if (subscript.IsWitnessProgram(witnessversion, witprog)) {
                result = scriptID;
                return true;
            }
            isminetype typ;
            typ = IsMine(*pwalletMain, subscript, SIGVERSION_WITNESS_V0);
            if (typ != ISMINE_SPENDABLE && typ != ISMINE_WATCH_SOLVABLE)
                return false;
            CScript witscript = GetScriptForWitness(subscript);
            pwalletMain->AddCScript(witscript);
            result = CScriptID(witscript);
            return true;
        }
        return false;
    }
};

UniValue addwitnessaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1)
    {
        string msg = "addwitnessaddress \"address\"\n"
            "\nAdd a witness address for a script (with pubkey or redeemscript known).\n"
            "It returns the witness script.\n"

            "\nArguments:\n"
            "1. \"address\"       (string, required) An address known to the wallet\n"

            "\nResult:\n"
            "\"witnessaddress\",  (string) The value of the new address (P2SH of witness script).\n"
            "}\n"
        ;
        throw runtime_error(msg);
    }

    {
        LOCK(cs_main);
        if (!IsWitnessEnabled(chainActive.Tip(), Params().GetConsensus()) && !GetBoolArg("-walletprematurewitness", false)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Segregated witness not enabled on network");
        }
    }

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    Witnessifier w;
    CTxDestination dest = address.Get();
    bool ret = boost::apply_visitor(w, dest);
    if (!ret) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Public key or redeemscript not known to wallet, or the key is uncompressed");
    }

    pwalletMain->SetAddressBook(w.result, "", "receive");

    return CBitcoinAddress(w.result).ToString();
}

struct tallyitem
{
    CAmount nAmount;
    int nConf;
    vector<uint256> txids;
    bool fIsWatchonly;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
        fIsWatchonly = false;
    }
};

UniValue ListReceived(const UniValue& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (params.size() > 1)
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;
    if(params.size() > 2)
        if(params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    // Tally
    map<CBitcoinAddress, tallyitem> mapTally;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;

        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth)
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.tx->vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            isminefilter mine = IsMine(*pwalletMain, address);
            if(!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    map<string, tallyitem> mapAccountTally;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, CAddressBookData)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strAccount = item.second.name;
        map<CBitcoinAddress, tallyitem>::iterator it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (fByAccounts)
        {
            tallyitem& _item = mapAccountTally[strAccount];
            _item.nAmount += nAmount;
            _item.nConf = min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        }
        else
        {
            UniValue obj(UniValue::VOBJ);
            if(fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("address",       address.ToString()));
            obj.push_back(Pair("account",       strAccount));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            if (!fByAccounts)
                obj.push_back(Pair("label", strAccount));
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end())
            {
                BOOST_FOREACH(const uint256& _item, (*it).second.txids)
                {
                    transactions.push_back(_item.GetHex());
                }
            }
            obj.push_back(Pair("txids", transactions));
            ret.push_back(obj);
        }
    }

    if (fByAccounts)
    {
        for (map<string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it)
        {
            CAmount nAmount = (*it).second.nAmount;
            int nConf = (*it).second.nConf;
            UniValue obj(UniValue::VOBJ);
            if((*it).second.fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("account",       (*it).first));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 3)
        throw runtime_error(
            "listreceivedbyaddress ( minconf include_empty include_watchonly)\n"
            "\nList balances by receiving address.\n"
            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. include_empty     (bool, optional, default=false) Whether to include addresses that haven't received any payments.\n"
            "3. include_watchonly (bool, optional, default=false) Whether to include watch-only addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,        (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"account\" : \"accountname\",       (string) DEPRECATED. The account of the receiving address. The default account is \"\".\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in " + CURRENCY_UNIT + " received by the address\n"
            "    \"confirmations\" : n,               (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\",               (string) A comment for the address/transaction, if any\n"
            "    \"txids\": [\n"
            "       n,                                (numeric) The ids of transactions received with the address \n"
            "       ...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("listreceivedbyaddress", "")
            + HelpExampleCli("listreceivedbyaddress", "6 true")
            + HelpExampleRpc("listreceivedbyaddress", "6, true, true")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    return ListReceived(request.params, false);
}

UniValue listreceivedbyaccount(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 3)
        throw runtime_error(
            "listreceivedbyaccount ( minconf include_empty include_watchonly)\n"
            "\nDEPRECATED. List balances by account.\n"
            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. include_empty     (bool, optional, default=false) Whether to include accounts that haven't received any payments.\n"
            "3. include_watchonly (bool, optional, default=false) Whether to include watch-only addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,   (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"account\" : \"accountname\",  (string) The account name of the receiving account\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this account\n"
            "    \"confirmations\" : n,          (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\"           (string) A comment for the address/transaction, if any\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("listreceivedbyaccount", "")
            + HelpExampleCli("listreceivedbyaccount", "6 true")
            + HelpExampleRpc("listreceivedbyaccount", "6, true, true")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    return ListReceived(request.params, true);
}

static void MaybePushAddress(UniValue & entry, const CTxDestination &dest)
{
    CBitcoinAddress addr;
    if (addr.Set(dest))
        entry.push_back(Pair("address", addr.ToString()));
}

void ListTransactions(const CWalletTx& wtx, const string& strAccount, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter)
{
    CAmount nFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, filter);

    bool fAllAccounts = (strAccount == string("*"));
    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount))
    {
        BOOST_FOREACH(const COutputEntry& s, listSent)
        {
            UniValue entry(UniValue::VOBJ);
            if(involvesWatchonly || (::IsMine(*pwalletMain, s.destination) & ISMINE_WATCH_ONLY))
                entry.push_back(Pair("involvesWatchonly", true));
            entry.push_back(Pair("account", strSentAccount));
            MaybePushAddress(entry, s.destination);
            entry.push_back(Pair("category", "send"));
            entry.push_back(Pair("amount", ValueFromAmount(-s.amount)));
            if (pwalletMain->mapAddressBook.count(s.destination))
                entry.push_back(Pair("label", pwalletMain->mapAddressBook[s.destination].name));
            entry.push_back(Pair("vout", s.vout));
            entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            entry.push_back(Pair("abandoned", wtx.isAbandoned()));
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth)
    {
        BOOST_FOREACH(const COutputEntry& r, listReceived)
        {
            string account;
            if (pwalletMain->mapAddressBook.count(r.destination))
                account = pwalletMain->mapAddressBook[r.destination].name;
            if (fAllAccounts || (account == strAccount))
            {
                UniValue entry(UniValue::VOBJ);
                if(involvesWatchonly || (::IsMine(*pwalletMain, r.destination) & ISMINE_WATCH_ONLY))
                    entry.push_back(Pair("involvesWatchonly", true));
                entry.push_back(Pair("account", account));
                MaybePushAddress(entry, r.destination);
                if (wtx.IsCoinBase())
                {
                    if (wtx.GetDepthInMainChain() < 1)
                        entry.push_back(Pair("category", "orphan"));
                    else if (wtx.GetBlocksToMaturity() > 0)
                        entry.push_back(Pair("category", "immature"));
                    else
                        entry.push_back(Pair("category", "generate"));
                }
                else
                {
                    entry.push_back(Pair("category", "receive"));
                }
                entry.push_back(Pair("amount", ValueFromAmount(r.amount)));
                if (pwalletMain->mapAddressBook.count(r.destination))
                    entry.push_back(Pair("label", account));
                entry.push_back(Pair("vout", r.vout));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const string& strAccount, UniValue& ret)
{
    bool fAllAccounts = (strAccount == string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount)
    {
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 4)
        throw runtime_error(
            "listtransactions ( \"account\" count skip include_watchonly)\n"
            "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions for account 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"    (string, optional) DEPRECATED. The account name. Should be \"*\".\n"
            "2. count          (numeric, optional, default=10) The number of transactions to return\n"
            "3. skip           (numeric, optional, default=0) The number of transactions to skip\n"
            "4. include_watchonly (bool, optional, default=false) Include transactions to watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"account\":\"accountname\",       (string) DEPRECATED. The account name associated with the transaction. \n"
            "                                                It will be \"\" for the default account.\n"
            "    \"address\":\"address\",    (string) The bitcoin address of the transaction. Not present for \n"
            "                                                move transactions (category = move).\n"
            "    \"category\":\"send|receive|move\", (string) The transaction category. 'move' is a local (off blockchain)\n"
            "                                                transaction between accounts, and not associated with an address,\n"
            "                                                transaction id or block. 'send' and 'receive' transactions are \n"
            "                                                associated with an address, transaction id and block details\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and for the\n"
            "                                         'move' category for moves outbound. It is positive for the 'receive' category,\n"
            "                                         and for the 'move' category for inbound funds.\n"
            "    \"label\": \"label\",       (string) A comment for the address/transaction, if any\n"
            "    \"vout\": n,                (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and \n"
            "                                         'receive' category of transactions. Negative confirmations indicate the\n"
            "                                         transaction conflicts with the block chain\n"
            "    \"trusted\": xxx,           (bool) Whether we consider the outputs of this unconfirmed transaction safe to spend.\n"
            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\", (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT). Available \n"
            "                                          for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"otheraccount\": \"accountname\",  (string) DEPRECATED. For the 'move' category of transactions, the account the funds came \n"
            "                                          from (for receiving funds, positive amounts), or went to (for sending funds,\n"
            "                                          negative amounts).\n"
            "    \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                     may be unknown for unconfirmed transactions not in the mempool\n"
            "    \"abandoned\": xxx          (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                         'send' category of transactions.\n"
            "  }\n"
            "]\n"

            "\nExamples:\n"
            "\nList the most recent 10 transactions in the systems\n"
            + HelpExampleCli("listtransactions", "") +
            "\nList transactions 100 to 120\n"
            + HelpExampleCli("listtransactions", "\"*\" 20 100") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("listtransactions", "\"*\", 20, 100")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount = "*";
    if (request.params.size() > 0)
        strAccount = request.params[0].get_str();
    int nCount = 10;
    if (request.params.size() > 1)
        nCount = request.params[1].get_int();
    int nFrom = 0;
    if (request.params.size() > 2)
        nFrom = request.params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if(request.params.size() > 3)
        if(request.params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    const CWallet::TxItems & txOrdered = pwalletMain->wtxOrdered;

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        if (pwtx != 0)
            ListTransactions(*pwtx, strAccount, 0, true, ret, filter);
        CAccountingEntry *const pacentry = (*it).second.second;
        if (pacentry != 0)
            AcentryToJSON(*pacentry, strAccount, ret);

        if ((int)ret.size() >= (nCount+nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    vector<UniValue> arrTmp = ret.getValues();

    vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom+nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

UniValue listaccounts(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 2)
        throw runtime_error(
            "listaccounts ( minconf include_watchonly)\n"
            "\nDEPRECATED. Returns Object that has account names as keys, account balances as values.\n"
            "\nArguments:\n"
            "1. minconf             (numeric, optional, default=1) Only include transactions with at least this many confirmations\n"
            "2. include_watchonly   (bool, optional, default=false) Include balances in watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "{                      (json object where keys are account names, and values are numeric balances\n"
            "  \"account\": x.xxx,  (numeric) The property name is the account name, and the value is the total balance for the account.\n"
            "  ...\n"
            "}\n"
            "\nExamples:\n"
            "\nList account balances where there at least 1 confirmation\n"
            + HelpExampleCli("listaccounts", "") +
            "\nList account balances including zero confirmation transactions\n"
            + HelpExampleCli("listaccounts", "0") +
            "\nList account balances for 6 or more confirmations\n"
            + HelpExampleCli("listaccounts", "6") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("listaccounts", "6")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    int nMinDepth = 1;
    if (request.params.size() > 0)
        nMinDepth = request.params[0].get_int();
    isminefilter includeWatchonly = ISMINE_SPENDABLE;
    if(request.params.size() > 1)
        if(request.params[1].get_bool())
            includeWatchonly = includeWatchonly | ISMINE_WATCH_ONLY;

    map<string, CAmount> mapAccountBalances;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first) & includeWatchonly) // This address belongs to me
            mapAccountBalances[entry.second.name] = 0;
    }

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        CAmount nFee;
        string strSentAccount;
        list<COutputEntry> listReceived;
        list<COutputEntry> listSent;
        int nDepth = wtx.GetDepthInMainChain();
        if (wtx.GetBlocksToMaturity() > 0 || nDepth < 0)
            continue;
        wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, includeWatchonly);
        mapAccountBalances[strSentAccount] -= nFee;
        BOOST_FOREACH(const COutputEntry& s, listSent)
            mapAccountBalances[strSentAccount] -= s.amount;
        if (nDepth >= nMinDepth)
        {
            BOOST_FOREACH(const COutputEntry& r, listReceived)
                if (pwalletMain->mapAddressBook.count(r.destination))
                    mapAccountBalances[pwalletMain->mapAddressBook[r.destination].name] += r.amount;
                else
                    mapAccountBalances[""] += r.amount;
        }
    }

    const list<CAccountingEntry> & acentries = pwalletMain->laccentries;
    BOOST_FOREACH(const CAccountingEntry& entry, acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    UniValue ret(UniValue::VOBJ);
    BOOST_FOREACH(const PAIRTYPE(string, CAmount)& accountBalance, mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

UniValue listsinceblock(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp)
        throw runtime_error(
            "listsinceblock ( \"blockhash\" target_confirmations include_watchonly)\n"
            "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted\n"
            "\nArguments:\n"
            "1. \"blockhash\"            (string, optional) The block hash to list transactions since\n"
            "2. target_confirmations:    (numeric, optional) The confirmations required, must be 1 or more\n"
            "3. include_watchonly:       (bool, optional, default=false) Include transactions to watch-only addresses (see 'importaddress')"
            "\nResult:\n"
            "{\n"
            "  \"transactions\": [\n"
            "    \"account\":\"accountname\",       (string) DEPRECATED. The account name associated with the transaction. Will be \"\" for the default account.\n"
            "    \"address\":\"address\",    (string) The bitcoin address of the transaction. Not present for move transactions (category = move).\n"
            "    \"category\":\"send|receive\",     (string) The transaction category. 'send' has negative amounts, 'receive' has positive amounts.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and for the 'move' category for moves \n"
            "                                          outbound. It is positive for the 'receive' category, and for the 'move' category for inbound funds.\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the 'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "                                          When it's < 0, it means the transaction conflicted that many blocks ago.\n"
            "    \"blockhash\": \"hashvalue\",     (string) The block hash containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT). Available for 'send' and 'receive' category of transactions.\n"
            "    \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                   may be unknown for unconfirmed transactions not in the mempool\n"
            "    \"abandoned\": xxx,         (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the 'send' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"label\" : \"label\"       (string) A comment for the address/transaction, if any\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
             "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the last block\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("listsinceblock", "")
            + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6")
            + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    const CBlockIndex *pindex = NULL;
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    if (request.params.size() > 0)
    {
        uint256 blockId;

        blockId.SetHex(request.params[0].get_str());
        BlockMap::iterator it = mapBlockIndex.find(blockId);
        if (it != mapBlockIndex.end())
        {
            pindex = it->second;
            if (chainActive[pindex->nHeight] != pindex)
            {
                // the block being asked for is a part of a deactivated chain;
                // we don't want to depend on its perceived height in the block
                // chain, we want to instead use the last common ancestor
                pindex = chainActive.FindFork(pindex);
            }
        }
    }

    if (request.params.size() > 1)
    {
        target_confirms = request.params[1].get_int();

        if (target_confirms < 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
    }

    if (request.params.size() > 2 && request.params[2].get_bool())
    {
        filter = filter | ISMINE_WATCH_ONLY;
    }

    int depth = pindex ? (1 + chainActive.Height() - pindex->nHeight) : -1;

    UniValue transactions(UniValue::VARR);

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); it++)
    {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth)
            ListTransactions(tx, "*", 0, true, transactions, filter);
    }

    CBlockIndex *pblockLast = chainActive[chainActive.Height() + 1 - target_confirms];
    uint256 lastblock = pblockLast ? pblockLast->GetBlockHash() : uint256();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("transactions", transactions));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

UniValue gettransaction(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "gettransaction \"txid\" ( include_watchonly )\n"
            "\nGet detailed information about in-wallet transaction <txid>\n"
            "\nArguments:\n"
            "1. \"txid\"                  (string, required) The transaction id\n"
            "2. \"include_watchonly\"     (bool, optional, default=false) Whether to include watch-only addresses in balance calculation and details[]\n"
            "\nResult:\n"
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in " + CURRENCY_UNIT + "\n"
            "  \"fee\": x.xxx,            (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                              'send' category of transactions.\n"
            "  \"confirmations\" : n,     (numeric) The number of confirmations\n"
            "  \"blockhash\" : \"hash\",  (string) The block hash\n"
            "  \"blockindex\" : xx,       (numeric) The index of the transaction in the block that includes it\n"
            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                   may be unknown for unconfirmed transactions not in the mempool\n"
            "  \"details\" : [\n"
            "    {\n"
            "      \"account\" : \"accountname\",      (string) DEPRECATED. The account name involved in the transaction, can be \"\" for the default account.\n"
            "      \"address\" : \"address\",          (string) The bitcoin address involved in the transaction\n"
            "      \"category\" : \"send|receive\",    (string) The category, either 'send' or 'receive'\n"
            "      \"amount\" : x.xxx,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"label\" : \"label\",              (string) A comment for the address/transaction, if any\n"
            "      \"vout\" : n,                       (numeric) the vout value\n"
            "      \"fee\": x.xxx,                     (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                           'send' category of transactions.\n"
            "      \"abandoned\": xxx                  (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                           'send' category of transactions.\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true")
            + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    isminefilter filter = ISMINE_SPENDABLE;
    if(request.params.size() > 1)
        if(request.params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    UniValue entry(UniValue::VOBJ);
    if (!pwalletMain->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    const CWalletTx& wtx = pwalletMain->mapWallet[hash];

    CAmount nCredit = wtx.GetCredit(filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.tx->GetValueOut() - nDebit : 0);

    entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
    if (wtx.IsFromMe(filter))
        entry.push_back(Pair("fee", ValueFromAmount(nFee)));

    WalletTxToJSON(wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(wtx, "*", 0, false, details, filter);
    entry.push_back(Pair("details", details));

    string strHex = EncodeHexTx(static_cast<CTransaction>(wtx), RPCSerializationFlags());
    entry.push_back(Pair("hex", strHex));

    return entry;
}

UniValue abandontransaction(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "abandontransaction \"txid\"\n"
            "\nMark in-wallet transaction <txid> as abandoned\n"
            "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
            "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
            "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
            "It has no effect on transactions which are already conflicted or abandoned.\n"
            "\nArguments:\n"
            "1. \"txid\"    (string, required) The transaction id\n"
            "\nResult:\n"
            "\nExamples:\n"
            + HelpExampleCli("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleRpc("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    if (!pwalletMain->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    if (!pwalletMain->AbandonTransaction(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");

    return NullUniValue;
}


UniValue backupwallet(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "backupwallet \"destination\"\n"
            "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n"
            "\nArguments:\n"
            "1. \"destination\"   (string) The destination directory or file\n"
            "\nExamples:\n"
            + HelpExampleCli("backupwallet", "\"backup.dat\"")
            + HelpExampleRpc("backupwallet", "\"backup.dat\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strDest = request.params[0].get_str();
    if (!pwalletMain->BackupWallet(strDest))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");

    return NullUniValue;
}


UniValue keypoolrefill(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
            "keypoolrefill ( newsize )\n"
            "\nFills the keypool."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments\n"
            "1. newsize     (numeric, optional, default=100) The new keypool size\n"
            "\nExamples:\n"
            + HelpExampleCli("keypoolrefill", "")
            + HelpExampleRpc("keypoolrefill", "")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (request.params.size() > 0) {
        if (request.params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].get_int();
    }

    EnsureWalletIsUnlocked();
    pwalletMain->TopUpKeyPool(kpSize);

    if (pwalletMain->GetKeyPoolSize() < kpSize)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");

    return NullUniValue;
}


static void LockWallet(CWallet* pWallet)
{
    LOCK(cs_nWalletUnlockTime);
    nWalletUnlockTime = 0;
    pWallet->Lock();
}

UniValue walletpassphrase(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (pwalletMain->IsCrypted() && (request.fHelp || request.params.size() != 2))
        throw runtime_error(
            "walletpassphrase \"passphrase\" timeout\n"
            "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
            "This is needed prior to performing transactions related to private keys such as sending bitcoins\n"
            "\nArguments:\n"
            "1. \"passphrase\"     (string, required) The wallet passphrase\n"
            "2. timeout            (numeric, required) The time to keep the decryption key in seconds.\n"
            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one.\n"
            "\nExamples:\n"
            "\nunlock the wallet for 60 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nLock the wallet again (before 60 seconds)\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");

    // Note that the walletpassphrase is stored in request.params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.length() > 0)
    {
        if (!pwalletMain->Unlock(strWalletPass))
            throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }
    else
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout>\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.");

    pwalletMain->TopUpKeyPool();

    int64_t nSleepTime = request.params[1].get_int64();
    LOCK(cs_nWalletUnlockTime);
    nWalletUnlockTime = GetTime() + nSleepTime;
    RPCRunLater("lockwallet", boost::bind(LockWallet, pwalletMain), nSleepTime);

    return NullUniValue;
}


UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (pwalletMain->IsCrypted() && (request.fHelp || request.params.size() != 2))
        throw runtime_error(
            "walletpassphrasechange \"oldpassphrase\" \"newpassphrase\"\n"
            "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n"
            "\nArguments:\n"
            "1. \"oldpassphrase\"      (string) The current passphrase\n"
            "2. \"newpassphrase\"      (string) The new passphrase\n"
            "\nExamples:\n"
            + HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"")
            + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwalletMain->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    return NullUniValue;
}


UniValue walletlock(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (pwalletMain->IsCrypted() && (request.fHelp || request.params.size() != 0))
        throw runtime_error(
            "walletlock\n"
            "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.\n"
            "\nExamples:\n"
            "\nSet the passphrase for 2 minutes to perform a transaction\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n"
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("walletlock", "")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");

    {
        LOCK(cs_nWalletUnlockTime);
        pwalletMain->Lock();
        nWalletUnlockTime = 0;
    }

    return NullUniValue;
}


UniValue encryptwallet(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (!pwalletMain->IsCrypted() && (request.fHelp || request.params.size() != 1))
        throw runtime_error(
            "encryptwallet \"passphrase\"\n"
            "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
            "After this, any calls that interact with private keys such as sending or signing \n"
            "will require the passphrase to be set prior the making these calls.\n"
            "Use the walletpassphrase call for this, and then walletlock call.\n"
            "If the wallet is already encrypted, use the walletpassphrasechange call.\n"
            "Note that this will shutdown the server.\n"
            "\nArguments:\n"
            "1. \"passphrase\"    (string) The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long.\n"
            "\nExamples:\n"
            "\nEncrypt you wallet\n"
            + HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending bitcoin\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can so something like sign\n"
            + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("encryptwallet", "\"my pass phrase\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (request.fHelp)
        return true;
    if (pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwalletMain->EncryptWallet(strWalletPass))
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "wallet encrypted; Bitcoin server stopping, restart to run with encrypted wallet. The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

UniValue lockunspent(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "lockunspent unlock ([{\"txid\":\"txid\",\"vout\":n},...])\n"
            "\nUpdates list of temporarily unspendable outputs.\n"
            "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
            "If no transaction outputs are specified when unlocking then all current locked transaction outputs are unlocked.\n"
            "A locked transaction output will not be chosen by automatic coin selection, when spending bitcoins.\n"
            "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
            "is always cleared (by virtue of process exit) when a node stops or fails.\n"
            "Also see the listunspent call\n"
            "\nArguments:\n"
            "1. unlock            (boolean, required) Whether to unlock (true) or lock (false) the specified transactions\n"
            "2. \"transactions\"  (string, optional) A json array of objects. Each object the txid (string) vout (numeric)\n"
            "     [           (json array of json objects)\n"
            "       {\n"
            "         \"txid\":\"id\",    (string) The transaction id\n"
            "         \"vout\": n         (numeric) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "true|false    (boolean) Whether the command was successful or not\n"

            "\nExamples:\n"
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (request.params.size() == 1)
        RPCTypeCheck(request.params, boost::assign::list_of(UniValue::VBOOL));
    else
        RPCTypeCheck(request.params, boost::assign::list_of(UniValue::VBOOL)(UniValue::VARR));

    bool fUnlock = request.params[0].get_bool();

    if (request.params.size() == 1) {
        if (fUnlock)
            pwalletMain->UnlockAllCoins();
        return true;
    }

    UniValue outputs = request.params[1].get_array();
    for (unsigned int idx = 0; idx < outputs.size(); idx++) {
        const UniValue& output = outputs[idx];
        if (!output.isObject())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");
        const UniValue& o = output.get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        string txid = find_value(o, "txid").get_str();
        if (!IsHex(txid))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected hex txid");

        int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        COutPoint outpt(uint256S(txid), nOutput);

        if (fUnlock)
            pwalletMain->UnlockCoin(outpt);
        else
            pwalletMain->LockCoin(outpt);
    }

    return true;
}

UniValue listlockunspent(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
            "listlockunspent\n"
            "\nReturns list of temporarily unspendable outputs.\n"
            "See the lockunspent call to lock and unlock transactions for spending.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("listlockunspent", "")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    vector<COutPoint> vOutpts;
    pwalletMain->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    BOOST_FOREACH(COutPoint &outpt, vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.push_back(Pair("txid", outpt.hash.GetHex()));
        o.push_back(Pair("vout", (int)outpt.n));
        ret.push_back(o);
    }

    return ret;
}

UniValue settxfee(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1)
        throw runtime_error(
            "settxfee amount\n"
            "\nSet the transaction fee per kB. Overwrites the paytxfee parameter.\n"
            "\nArguments:\n"
            "1. amount         (numeric or string, required) The transaction fee in " + CURRENCY_UNIT + "/kB\n"
            "\nResult\n"
            "true|false        (boolean) Returns true if successful\n"
            "\nExamples:\n"
            + HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Amount
    CAmount nAmount = AmountFromValue(request.params[0]);

    payTxFee = CFeeRate(nAmount, 1000);
    return true;
}

UniValue getwalletinfo(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getwalletinfo\n"
            "Returns an object containing various wallet state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"walletversion\": xxxxx,       (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,           (numeric) the total confirmed balance of the wallet in " + CURRENCY_UNIT + "\n"
            "  \"unconfirmed_balance\": xxx,   (numeric) the total unconfirmed balance of the wallet in " + CURRENCY_UNIT + "\n"
            "  \"immature_balance\": xxxxxx,   (numeric) the total immature balance of the wallet in " + CURRENCY_UNIT + "\n"
            "  \"txcount\": xxxxxxx,           (numeric) the total number of transactions in the wallet\n"
            "  \"keypoololdest\": xxxxxx,      (numeric) the timestamp (seconds since Unix epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,          (numeric) how many new keys are pre-generated\n"
            "  \"unlocked_until\": ttt,        (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,           (numeric) the transaction fee configuration, set in " + CURRENCY_UNIT + "/kB\n"
            "  \"hdmasterkeyid\": \"<hash160>\" (string) the Hash160 of the HD master pubkey\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getwalletinfo", "")
            + HelpExampleRpc("getwalletinfo", "")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));
    obj.push_back(Pair("balance",       ValueFromAmount(pwalletMain->GetBalance())));
    obj.push_back(Pair("unconfirmed_balance", ValueFromAmount(pwalletMain->GetUnconfirmedBalance())));
    obj.push_back(Pair("immature_balance",    ValueFromAmount(pwalletMain->GetImmatureBalance())));
    obj.push_back(Pair("txcount",       (int)pwalletMain->mapWallet.size()));
    obj.push_back(Pair("keypoololdest", pwalletMain->GetOldestKeyPoolTime()));
    obj.push_back(Pair("keypoolsize",   (int)pwalletMain->GetKeyPoolSize()));
    if (pwalletMain->IsCrypted())
        obj.push_back(Pair("unlocked_until", nWalletUnlockTime));
    obj.push_back(Pair("paytxfee",      ValueFromAmount(payTxFee.GetFeePerK())));
    CKeyID masterKeyID = pwalletMain->GetHDChain().masterKeyID;
    if (!masterKeyID.IsNull())
         obj.push_back(Pair("hdmasterkeyid", masterKeyID.GetHex()));
    return obj;
}

UniValue resendwallettransactions(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "resendwallettransactions\n"
            "Immediately re-broadcast unconfirmed wallet transactions to all peers.\n"
            "Intended only for testing; the wallet code periodically re-broadcasts\n"
            "automatically.\n"
            "Returns array of transaction ids that were re-broadcast.\n"
            );

    if (!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    LOCK2(cs_main, pwalletMain->cs_wallet);

    std::vector<uint256> txids = pwalletMain->ResendWalletTransactionsBefore(GetTime(), g_connman.get());
    UniValue result(UniValue::VARR);
    BOOST_FOREACH(const uint256& txid, txids)
    {
        result.push_back(txid.ToString());
    }
    return result;
}

UniValue listunspent(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 4)
        throw runtime_error(
            "listunspent ( minconf maxconf  [\"addresses\",...] [include_unsafe] )\n"
            "\nReturns array of unspent transaction outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filter to only include txouts paid to specified addresses.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) The minimum confirmations to filter\n"
            "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
            "3. \"addresses\"    (string) A json array of bitcoin addresses to filter\n"
            "    [\n"
            "      \"address\"   (string) bitcoin address\n"
            "      ,...\n"
            "    ]\n"
            "4. include_unsafe (bool, optional, default=true) Include outputs that are not safe to spend\n"
            "                  because they come from unconfirmed untrusted transactions or unconfirmed\n"
            "                  replacement transactions (cases where we are less sure that a conflicting\n"
            "                  transaction won't be mined).\n"
            "\nResult\n"
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",          (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",    (string) the bitcoin address\n"
            "    \"account\" : \"account\",    (string) DEPRECATED. The associated account, or \"\" for the default account\n"
            "    \"scriptPubKey\" : \"key\",   (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " + CURRENCY_UNIT + "\n"
            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
            "    \"redeemScript\" : n        (string) The redeemScript if scriptPubKey is P2SH\n"
            "    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
            "    \"solvable\" : xxx          (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n"
            + HelpExampleCli("listunspent", "")
            + HelpExampleCli("listunspent", "6 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
        );

    int nMinDepth = 1;
    if (request.params.size() > 0 && !request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    set<CBitcoinAddress> setAddress;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CBitcoinAddress address(input.get_str());
            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Bitcoin address: ")+input.get_str());
            if (setAddress.count(address))
                throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+input.get_str());
           setAddress.insert(address);
        }
    }

    bool include_unsafe = true;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }

    UniValue results(UniValue::VARR);
    vector<COutput> vecOutputs;
    assert(pwalletMain != NULL);
    LOCK2(cs_main, pwalletMain->cs_wallet);
    pwalletMain->AvailableCoins(vecOutputs, NULL, !include_unsafe, NULL, true);
    BOOST_FOREACH(const COutput& out, vecOutputs) {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;

        CTxDestination address;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, address);

        if (setAddress.size() && (!fValidAddress || !setAddress.count(address)))
            continue;

        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("txid", out.tx->GetHash().GetHex()));
        entry.push_back(Pair("vout", out.i));

        if (fValidAddress) {
            entry.push_back(Pair("address", CBitcoinAddress(address).ToString()));

            if (pwalletMain->mapAddressBook.count(address))
                entry.push_back(Pair("account", pwalletMain->mapAddressBook[address].name));

            if (scriptPubKey.IsPayToScriptHash()) {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwalletMain->GetCScript(hash, redeemScript))
                    entry.push_back(Pair("redeemScript", HexStr(redeemScript.begin(), redeemScript.end())));
            }
        }

        entry.push_back(Pair("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end())));
        entry.push_back(Pair("amount", ValueFromAmount(out.tx->tx->vout[out.i].nValue)));
        entry.push_back(Pair("confirmations", out.nDepth));
        entry.push_back(Pair("spendable", out.fSpendable));
        entry.push_back(Pair("solvable", out.fSolvable));
        results.push_back(entry);
    }

    return results;
}

UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
                            "fundrawtransaction \"hexstring\" ( options )\n"
                            "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
                            "This will not modify existing inputs, and will add at most one change output to the outputs.\n"
                            "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
                            "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                            "The inputs added will not be signed, use signrawtransaction for that.\n"
                            "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
                            "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                            "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
                            "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
                            "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n"
                            "\nArguments:\n"
                            "1. \"hexstring\"           (string, required) The hex string of the raw transaction\n"
                            "2. options                 (object, optional)\n"
                            "   {\n"
                            "     \"changeAddress\"          (string, optional, default pool address) The bitcoin address to receive the change\n"
                            "     \"changePosition\"         (numeric, optional, default random) The index of the change output\n"
                            "     \"includeWatching\"        (boolean, optional, default false) Also select inputs which are watch only\n"
                            "     \"lockUnspents\"           (boolean, optional, default false) Lock selected unspent outputs\n"
                            "     \"reserveChangeKey\"       (boolean, optional, default true) Reserves the change output key from the keypool\n"
                            "     \"feeRate\"                (numeric, optional, default not set: makes wallet determine the fee) Set a specific feerate (" + CURRENCY_UNIT + " per KB)\n"
                            "     \"subtractFeeFromOutputs\" (array, optional) A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              The outputs are specified by their zero-based index, before any change output is added.\n"
                            "                              Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.\n"
                            "                                  [vout_index,...]\n"
                            "   }\n"
                            "                         for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}\n"
                            "\nResult:\n"
                            "{\n"
                            "  \"hex\":       \"value\", (string)  The resulting raw transaction (hex-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                            "\nExamples:\n"
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransaction", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                            );

    RPCTypeCheck(request.params, boost::assign::list_of(UniValue::VSTR));

    CTxDestination changeAddress = CNoDestination();
    int changePosition = -1;
    bool includeWatching = false;
    bool lockUnspents = false;
    bool reserveChangeKey = true;
    CFeeRate feeRate = CFeeRate(0);
    bool overrideEstimatedFeerate = false;
    UniValue subtractFeeFromOutputs;
    set<int> setSubtractFeeFromOutputs;

    if (request.params.size() > 1) {
      if (request.params[1].type() == UniValue::VBOOL) {
        // backward compatibility bool only fallback
        includeWatching = request.params[1].get_bool();
      }
      else {
        RPCTypeCheck(request.params, boost::assign::list_of(UniValue::VSTR)(UniValue::VOBJ));

        UniValue options = request.params[1];

        RPCTypeCheckObj(options,
            {
                {"changeAddress", UniValueType(UniValue::VSTR)},
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"reserveChangeKey", UniValueType(UniValue::VBOOL)},
                {"feeRate", UniValueType()}, // will be checked below
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
            },
            true, true);

        if (options.exists("changeAddress")) {
            CBitcoinAddress address(options["changeAddress"].get_str());

            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "changeAddress must be a valid bitcoin address");

            changeAddress = address.Get();
        }

        if (options.exists("changePosition"))
            changePosition = options["changePosition"].get_int();

        if (options.exists("includeWatching"))
            includeWatching = options["includeWatching"].get_bool();

        if (options.exists("lockUnspents"))
            lockUnspents = options["lockUnspents"].get_bool();

        if (options.exists("reserveChangeKey"))
            reserveChangeKey = options["reserveChangeKey"].get_bool();

        if (options.exists("feeRate"))
        {
            feeRate = CFeeRate(AmountFromValue(options["feeRate"]));
            overrideEstimatedFeerate = true;
        }

        if (options.exists("subtractFeeFromOutputs"))
            subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();
      }
    }

    // parse hex string from parameter
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str(), true))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (changePosition != -1 && (changePosition < 0 || (unsigned int)changePosition > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    CAmount nFeeOut;
    string strFailReason;

    LOCK2(cs_main, pwalletMain->cs_wallet);
    if(!pwalletMain->FundTransaction(tx, nFeeOut, overrideEstimatedFeerate, feeRate, changePosition, strFailReason, includeWatching, lockUnspents, setSubtractFeeFromOutputs, reserveChangeKey, changeAddress))
        throw JSONRPCError(RPC_INTERNAL_ERROR, strFailReason);

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexTx(tx)));
    result.push_back(Pair("changepos", changePosition));
    result.push_back(Pair("fee", ValueFromAmount(nFeeOut)));

    return result;
}

// Calculate the size of the transaction assuming all signatures are max size
// Use DummySignatureCreator, which inserts 72 byte signatures everywhere.
// TODO: re-use this in CWallet::CreateTransaction (right now
// CreateTransaction uses the constructed dummy-signed tx to do a priority
// calculation, but we should be able to refactor after priority is removed).
// NOTE: this requires that all inputs must be in mapWallet (eg the tx should
// be IsAllFromMe).
int64_t CalculateMaximumSignedTxSize(const CTransaction &tx)
{
    CMutableTransaction txNew(tx);
    std::vector<pair<CWalletTx *, unsigned int>> vCoins;
    // Look up the inputs.  We should have already checked that this transaction
    // IsAllFromMe(ISMINE_SPENDABLE), so every input should already be in our
    // wallet, with a valid index into the vout array.
    for (auto& input : tx.vin) {
        const auto mi = pwalletMain->mapWallet.find(input.prevout.hash);
        assert(mi != pwalletMain->mapWallet.end() && input.prevout.n < mi->second.tx->vout.size());
        vCoins.emplace_back(make_pair(&(mi->second), input.prevout.n));
    }
    if (!pwalletMain->DummySignTx(txNew, vCoins)) {
        // This should never happen, because IsAllFromMe(ISMINE_SPENDABLE)
        // implies that we can sign for every input.
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction contains inputs that cannot be signed");
    }
    return GetVirtualTransactionSize(txNew);
}

UniValue bumpfee(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw runtime_error(
            "bumpfee \"txid\" ( options ) \n"
            "\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n"
            "An opt-in RBF transaction with the given txid must be in the wallet.\n"
            "The command will pay the additional fee by decreasing (or perhaps removing) its change output.\n"
            "If the change output is not big enough to cover the increased fee, the command will currently fail\n"
            "instead of adding new inputs to compensate. (A future implementation could improve this.)\n"
            "The command will fail if the wallet or mempool contains a transaction that spends one of T's outputs.\n"
            "By default, the new fee will be calculated automatically using estimatefee.\n"
            "The user can specify a confirmation target for estimatefee.\n"
            "Alternatively, the user can specify totalFee, or use RPC setpaytxfee to set a higher fee rate.\n"
            "At a minimum, the new fee rate must be high enough to pay an additional new relay fee (incrementalfee\n"
            "returned by getnetworkinfo) to enter the node's mempool.\n"
            "\nArguments:\n"
            "1. txid                  (string, required) The txid to be bumped\n"
            "2. options               (object, optional)\n"
            "   {\n"
            "     \"confTarget\"        (numeric, optional) Confirmation target (in blocks)\n"
            "     \"totalFee\"          (numeric, optional) Total fee (NOT feerate) to pay, in satoshis.\n"
            "                         In rare cases, the actual fee paid might be slightly higher than the specified\n"
            "                         totalFee if the tx change output has to be removed because it is too close to\n"
            "                         the dust threshold.\n"
            "     \"replaceable\"       (boolean, optional, default true) Whether the new transaction should still be\n"
            "                         marked bip-125 replaceable. If true, the sequence numbers in the transaction will\n"
            "                         be left unchanged from the original. If false, any input sequence numbers in the\n"
            "                         original transaction that were less than 0xfffffffe will be increased to 0xfffffffe\n"
            "                         so the new transaction will not be explicitly bip-125 replaceable (though it may\n"
            "                         still be replacable in practice, for example if it has unconfirmed ancestors which\n"
            "                         are replaceable).\n"
            "   }\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\":    \"value\",   (string)  The id of the new transaction\n"
            "  \"origfee\":  n,         (numeric) Fee of the replaced transaction\n"
            "  \"fee\":      n,         (numeric) Fee of the new transaction\n"
            "  \"errors\":  [ str... ] (json array of strings) Errors encountered during processing (may be empty)\n"
            "}\n"
            "\nExamples:\n"
            "\nBump the fee, get the new transaction\'s txid\n" +
            HelpExampleCli("bumpfee", "<txid>"));
    }

    RPCTypeCheck(request.params, boost::assign::list_of(UniValue::VSTR)(UniValue::VOBJ));
    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    // retrieve the original tx from the wallet
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    if (!pwalletMain->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    CWalletTx& wtx = pwalletMain->mapWallet[hash];

    if (pwalletMain->HasWalletSpend(hash)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Transaction has descendants in the wallet");
    }

    {
        LOCK(mempool.cs);
        auto it = mempool.mapTx.find(hash);
        if (it != mempool.mapTx.end() && it->GetCountWithDescendants() > 1) {
            throw JSONRPCError(RPC_MISC_ERROR, "Transaction has descendants in the mempool");
        }
    }

    if (wtx.GetDepthInMainChain() != 0) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction has been mined, or is conflicted with a mined transaction");
    }

    if (!SignalsOptInRBF(wtx)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction is not BIP 125 replaceable");
    }

    if (wtx.mapValue.count("replaced_by_txid")) {
        throw JSONRPCError(RPC_INVALID_REQUEST, strprintf("Cannot bump transaction %s which was already bumped by transaction %s", hash.ToString(), wtx.mapValue.at("replaced_by_txid")));
    }

    // check that original tx consists entirely of our inputs
    // if not, we can't bump the fee, because the wallet has no way of knowing the value of the other inputs (thus the fee)
    if (!pwalletMain->IsAllFromMe(wtx, ISMINE_SPENDABLE)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction contains inputs that don't belong to this wallet");
    }

    // figure out which output was change
    // if there was no change output or multiple change outputs, fail
    int nOutput = -1;
    for (size_t i = 0; i < wtx.tx->vout.size(); ++i) {
        if (pwalletMain->IsChange(wtx.tx->vout[i])) {
            if (nOutput != -1) {
                throw JSONRPCError(RPC_MISC_ERROR, "Transaction has multiple change outputs");
            }
            nOutput = i;
        }
    }
    if (nOutput == -1) {
        throw JSONRPCError(RPC_MISC_ERROR, "Transaction does not have a change output");
    }

    // Calculate the expected size of the new transaction.
    int64_t txSize = GetVirtualTransactionSize(*(wtx.tx));
    const int64_t maxNewTxSize = CalculateMaximumSignedTxSize(*wtx.tx);

    // optional parameters
    bool specifiedConfirmTarget = false;
    int newConfirmTarget = nTxConfirmTarget;
    CAmount totalFee = 0;
    bool replaceable = true;
    if (request.params.size() > 1) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"totalFee", UniValueType(UniValue::VNUM)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
            },
            true, true);

        if (options.exists("confTarget") && options.exists("totalFee")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget and totalFee options should not both be set. Please provide either a confirmation target for fee estimation or an explicit total fee for the transaction.");
        } else if (options.exists("confTarget")) {
            specifiedConfirmTarget = true;
            newConfirmTarget = options["confTarget"].get_int();
            if (newConfirmTarget <= 0) { // upper-bound will be checked by estimatefee/smartfee
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid confTarget (cannot be <= 0)");
            }
        } else if (options.exists("totalFee")) {
            totalFee = options["totalFee"].get_int64();
            CAmount requiredFee = CWallet::GetRequiredFee(maxNewTxSize);
            if (totalFee < requiredFee ) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   strprintf("Insufficient totalFee (cannot be less than required fee %s)",
                                             FormatMoney(requiredFee)));
            }
        }

        if (options.exists("replaceable")) {
            replaceable = options["replaceable"].get_bool();
        }
    }

    // calculate the old fee and fee-rate
    CAmount nOldFee = wtx.GetDebit(ISMINE_SPENDABLE) - wtx.tx->GetValueOut();
    CFeeRate nOldFeeRate(nOldFee, txSize);
    CAmount nNewFee;
    CFeeRate nNewFeeRate;
    // The wallet uses a conservative WALLET_INCREMENTAL_RELAY_FEE value to
    // future proof against changes to network wide policy for incremental relay
    // fee that our node may not be aware of.
    CFeeRate walletIncrementalRelayFee = CFeeRate(WALLET_INCREMENTAL_RELAY_FEE);
    if (::incrementalRelayFee > walletIncrementalRelayFee) {
        walletIncrementalRelayFee = ::incrementalRelayFee;
    }

    if (totalFee > 0) {
        CAmount minTotalFee = nOldFeeRate.GetFee(maxNewTxSize) + ::incrementalRelayFee.GetFee(maxNewTxSize);
        if (totalFee < minTotalFee) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Insufficient totalFee, must be at least %s (oldFee %s + incrementalFee %s)",
                                                                FormatMoney(minTotalFee), FormatMoney(nOldFeeRate.GetFee(maxNewTxSize)), FormatMoney(::incrementalRelayFee.GetFee(maxNewTxSize))));
        }
        nNewFee = totalFee;
        nNewFeeRate = CFeeRate(totalFee, maxNewTxSize);
    } else {
        // if user specified a confirm target then don't consider any global payTxFee
        if (specifiedConfirmTarget) {
            nNewFee = CWallet::GetMinimumFee(maxNewTxSize, newConfirmTarget, mempool, CAmount(0));
        }
        // otherwise use the regular wallet logic to select payTxFee or default confirm target
        else {
            nNewFee = CWallet::GetMinimumFee(maxNewTxSize, newConfirmTarget, mempool);
        }

        nNewFeeRate = CFeeRate(nNewFee, maxNewTxSize);

        // New fee rate must be at least old rate + minimum incremental relay rate
        // walletIncrementalRelayFee.GetFeePerK() should be exact, because it's initialized
        // in that unit (fee per kb).
        // However, nOldFeeRate is a calculated value from the tx fee/size, so
        // add 1 satoshi to the result, because it may have been rounded down.
        if (nNewFeeRate.GetFeePerK() < nOldFeeRate.GetFeePerK() + 1 + walletIncrementalRelayFee.GetFeePerK()) {
            nNewFeeRate = CFeeRate(nOldFeeRate.GetFeePerK() + 1 + walletIncrementalRelayFee.GetFeePerK());
            nNewFee = nNewFeeRate.GetFee(maxNewTxSize);
        }
    }

    // Check that in all cases the new fee doesn't violate maxTxFee
     if (nNewFee > maxTxFee) {
         throw JSONRPCError(RPC_MISC_ERROR,
                            strprintf("Specified or calculated fee %s is too high (cannot be higher than maxTxFee %s)",
                                      FormatMoney(nNewFee), FormatMoney(maxTxFee)));
     }

    // check that fee rate is higher than mempool's minimum fee
    // (no point in bumping fee if we know that the new tx won't be accepted to the mempool)
    // This may occur if the user set TotalFee or paytxfee too low, if fallbackfee is too low, or, perhaps,
    // in a rare situation where the mempool minimum fee increased significantly since the fee estimation just a
    // moment earlier. In this case, we report an error to the user, who may use totalFee to make an adjustment.
    CFeeRate minMempoolFeeRate = mempool.GetMinFee(GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000);
    if (nNewFeeRate.GetFeePerK() < minMempoolFeeRate.GetFeePerK()) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("New fee rate (%s) is less than the minimum fee rate (%s) to get into the mempool. totalFee value should to be at least %s or settxfee value should be at least %s to add transaction.", FormatMoney(nNewFeeRate.GetFeePerK()), FormatMoney(minMempoolFeeRate.GetFeePerK()), FormatMoney(minMempoolFeeRate.GetFee(maxNewTxSize)), FormatMoney(minMempoolFeeRate.GetFeePerK())));
    }

    // Now modify the output to increase the fee.
    // If the output is not large enough to pay the fee, fail.
    CAmount nDelta = nNewFee - nOldFee;
    assert(nDelta > 0);
    CMutableTransaction tx(*(wtx.tx));
    CTxOut* poutput = &(tx.vout[nOutput]);
    if (poutput->nValue < nDelta) {
        throw JSONRPCError(RPC_MISC_ERROR, "Change output is too small to bump the fee");
    }

    // If the output would become dust, discard it (converting the dust to fee)
    poutput->nValue -= nDelta;
    if (poutput->nValue <= poutput->GetDustThreshold(::dustRelayFee)) {
        LogPrint("rpc", "Bumping fee and discarding dust output\n");
        nNewFee += poutput->nValue;
        tx.vout.erase(tx.vout.begin() + nOutput);
    }

    // Mark new tx not replaceable, if requested.
    if (!replaceable) {
        for (auto& input : tx.vin) {
            if (input.nSequence < 0xfffffffe) input.nSequence = 0xfffffffe;
        }
    }

    // sign the new tx
    CTransaction txNewConst(tx);
    int nIn = 0;
    for (auto& input : tx.vin) {
        std::map<uint256, CWalletTx>::const_iterator mi = pwalletMain->mapWallet.find(input.prevout.hash);
        assert(mi != pwalletMain->mapWallet.end() && input.prevout.n < mi->second.tx->vout.size());
        const CScript& scriptPubKey = mi->second.tx->vout[input.prevout.n].scriptPubKey;
        const CAmount& amount = mi->second.tx->vout[input.prevout.n].nValue;
        SignatureData sigdata;
        if (!ProduceSignature(TransactionSignatureCreator(pwalletMain, &txNewConst, nIn, amount, SIGHASH_ALL), scriptPubKey, sigdata)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Can't sign transaction.");
        }
        UpdateTransaction(tx, nIn, sigdata);
        nIn++;
    }

    // commit/broadcast the tx
    CReserveKey reservekey(pwalletMain);
    CWalletTx wtxBumped(pwalletMain, MakeTransactionRef(std::move(tx)));
    wtxBumped.mapValue = wtx.mapValue;
    wtxBumped.mapValue["replaces_txid"] = hash.ToString();
    wtxBumped.vOrderForm = wtx.vOrderForm;
    wtxBumped.strFromAccount = wtx.strFromAccount;
    wtxBumped.fTimeReceivedIsTxTime = true;
    wtxBumped.fFromMe = true;
    CValidationState state;
    if (!pwalletMain->CommitTransaction(wtxBumped, reservekey, g_connman.get(), state)) {
        // NOTE: CommitTransaction never returns false, so this should never happen.
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Error: The transaction was rejected! Reason given: %s", state.GetRejectReason()));
    }

    UniValue vErrors(UniValue::VARR);
    if (state.IsInvalid()) {
        // This can happen if the mempool rejected the transaction.  Report
        // what happened in the "errors" response.
        vErrors.push_back(strprintf("Error: The transaction was rejected: %s", FormatStateMessage(state)));
    }

    // mark the original tx as bumped
    if (!pwalletMain->MarkReplaced(wtx.GetHash(), wtxBumped.GetHash())) {
        // TODO: see if JSON-RPC has a standard way of returning a response
        // along with an exception. It would be good to return information about
        // wtxBumped to the caller even if marking the original transaction
        // replaced does not succeed for some reason.
        vErrors.push_back("Error: Created new bumpfee transaction but could not mark the original transaction as replaced.");
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("txid", wtxBumped.GetHash().GetHex()));
    result.push_back(Pair("origfee", ValueFromAmount(nOldFee)));
    result.push_back(Pair("fee", ValueFromAmount(nNewFee)));
    result.push_back(Pair("errors", vErrors));

    return result;
}

extern UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue importprivkey(const JSONRPCRequest& request);
extern UniValue importaddress(const JSONRPCRequest& request);
extern UniValue importpubkey(const JSONRPCRequest& request);
extern UniValue dumpwallet(const JSONRPCRequest& request);
extern UniValue importwallet(const JSONRPCRequest& request);
extern UniValue importprunedfunds(const JSONRPCRequest& request);
extern UniValue removeprunedfunds(const JSONRPCRequest& request);
extern UniValue importmulti(const JSONRPCRequest& request);

static void SendWithOpreturn(const CBitcoinAddress &address, CWalletTx& wtxNew, uint64_t fee, unsigned int nAppID, const vector<unsigned char>& vctAppData)
{
    CAmount curBalance = pwalletMain->GetAddressBalance(address.Get());
    std::string strError;

    CCoinControl coinControl;
    coinControl.nMinimumTotalFee = fee;
    coinControl.fAllowOtherInputs = true;

    if (coinControl.nMinimumTotalFee > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    if (pwalletMain->GetBroadcastTransactions() && !g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    vector<unsigned char> opdata = pwalletMain->CreateOpReturn(nAppID, vctAppData);
    if(opdata.empty()) {
        strError = strprintf("Error: CreateOpReturn");
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    CReserveKey reservekey(pwalletMain);
    CAmount nFeeRequired;
    int nChangePosRet = -1;

    auto fromAddress = address.Get();
    vector<CRecipient> vecSend;
    // wdy begin
    // Parse Bitcoin address
    CScript scriptPubKey = GetScriptForDestination(fromAddress);
    // Create and send the transaction
    CRecipient recipient = {scriptPubKey, curBalance, false};
    vecSend.push_back(recipient);
    // wdy end
    if ( !pwalletMain->CreateTransaction(vecSend, wtxNew, reservekey, nFeeRequired, nChangePosRet, strError, &coinControl, true, &fromAddress, &opdata)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    CValidationState state;
    if (!pwalletMain->CommitTransaction(wtxNew, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
}

string JsonToStruct(CBitcoinAddress& address, CRegisterForgerData& data, const JSONRPCRequest& request)
{
    data.opcode = OP_REGISTE;
    address = CBitcoinAddress(request.params[0].get_str());
    if (!address.IsValid())
        return "Invalid Bitcoin address";

    CKeyID delegate;
    address.GetKeyID(delegate);
    if(Vote::GetInstance().HaveDelegate(request.params[1].get_str(), delegate)) {
        return "Forger name has registe";
    }

    data.name = request.params[1].get_str();
    string ret = CheckStruct(data);
    return ret;
}


UniValue registe(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2 )
        throw runtime_error(
            "register delegateAddress delegateName\n"
            "\nuse lbtc address to register as a delegate.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"delegateAddress\"     (string, required) The lbtc address.\n"
            "2. \"delegateName\"        (string, required) The delegate name.\n"
            "\nResult:\n"
            "\"txid\"                   (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("register", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"delegateName\"")
            + HelpExampleRpc("register", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"delegateName\"")
        );

    CBitcoinAddress address;
    CRegisterForgerData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_REGISTER_FORGER_FEE, AppID::DPOS, opreturn);

    return wtx.GetHash().GetHex();
}

string JsonToStruct(CBitcoinAddress& address, CVoteForgerData& data, const JSONRPCRequest& request)
{
    data.opcode = OP_VOTE;

    address = CBitcoinAddress(request.params[0].get_str());
    CKeyID address_id;
    address.GetKeyID(address_id);
    if (!address.IsValid()) {
        return "Invalid Bitcoin address";
    }

    set<CBitcoinAddress> setAddress;
    for (unsigned int idx = 1; idx < request.params.size(); idx++) {
        auto name = request.params[idx].get_str();
        auto keyID = Vote::GetInstance().GetDelegate(name);

        if(keyID.IsNull())
            return string("delegate name: ") + request.params[idx].get_str() + string(" not register");

        if(Vote::GetInstance().HaveVote(address_id, keyID))
            return string("delegate name: ") + request.params[idx].get_str() + string(" is voted");

        CBitcoinAddress address(keyID);
        if (setAddress.count(address))
            return string("Invalid parameter, duplicated name: ")+ name;

        setAddress.insert(address);
        data.forgers.insert(keyID);
    }

    if((setAddress.size() + Vote::GetInstance().GetVotedDelegates(address_id).size()) > Vote::MaxNumberOfVotes)
        return "delegates number must not more than 51";

    string ret = CheckStruct(data);
    return ret;
}

string JsonToStruct(CBitcoinAddress& address, CCancelVoteForgerData& data, const JSONRPCRequest& request)
{
    data.opcode = OP_REVOKE;

    address = CBitcoinAddress(request.params[0].get_str());
    CKeyID address_id;
    address.GetKeyID(address_id);
    if (!address.IsValid())
        return "Invalid Bitcoin address";

    set<CBitcoinAddress> setAddress;
    for (unsigned int idx = 1; idx < request.params.size(); idx++) {
        auto name = request.params[idx].get_str();
        CKeyID keyID = Vote::GetInstance().GetDelegate(name);

        if(keyID.IsNull())
            return string("delegate name: ") + request.params[idx].get_str() + string(" not register");

        if(Vote::GetInstance().HaveVote(address_id, keyID) == false) {
            return string("delegate name: ") + request.params[idx].get_str() + string(" is not voted");
        }

        CBitcoinAddress address(keyID);
        if (setAddress.count(address))
            return string("Invalid parameter, duplicated name: ")+ request.params[idx].get_str();

        if( setAddress.size() >= Vote::MaxNumberOfVotes)
            return string("delegates number must not more than 51");

        setAddress.insert(address);
        data.forgers.insert(keyID);
    }

    string ret = CheckStruct(data);
    return ret;
}

string JsonToStruct(CBitcoinAddress& address, CRegisterCommitteeData& data, const JSONRPCRequest& request)
{
    string ret;
    data.opcode = 0xc3;
    address = CBitcoinAddress(request.params[0].get_str());
    data.name = request.params[1].get_str();
    data.url = request.params[2].get_str();

    CKeyID id;
    address.GetKeyID(id);
    if(Vote::GetInstance().GetCommittee().GetRegiste(NULL, id)) {
        ret = "The address has registerd";
    } else {
        auto& name = data.name;
        auto f = [&name](const CKeyID& key, const CRegisterCommitteeData& value) -> bool {
            bool ret = false;
            if(value.name == name) {
                ret = true;
            }
            return ret;
        };

        if(Vote::GetInstance().GetCommittee().FindRegiste(f)) {
            ret = "The name has registerd";
        }
    }

    if(ret.empty()) {
        ret = CheckStruct(data);
    }
    return ret;
}

string JsonToStruct(CBitcoinAddress& address, CVoteCommitteeData& data, const JSONRPCRequest& request)
{
    string ret;
    data.opcode = 0xc4;
    address = CBitcoinAddress(request.params[0].get_str());

    string name = request.params[1].get_str();
    CKeyID committee;
    auto f = [&name, &committee](const CKeyID& key, const CRegisterCommitteeData& value) -> bool {
        bool ret = false;
        if(value.name == name) {
            committee = key;
            ret = true;
        }
        return ret;
    };

    if(Vote::GetInstance().GetCommittee().FindRegiste(f) == false) {
        return "The name dosn't registed";
    }

    data.committee = committee;

    CKeyID voterid;
    address.GetKeyID(voterid);

    if(Vote::GetInstance().GetCommittee().FindVoter(voterid)) {
        ret = "The address has voted committee";
    }

    if(ret.empty()) {
        ret = CheckStruct(data);
    }
    return ret;
}

string JsonToStruct(CBitcoinAddress& address, CCancelVoteCommitteeData& data, const JSONRPCRequest& request)
{
    string ret;
    data.opcode = 0xc5;
    address = CBitcoinAddress(request.params[0].get_str());

    string name = request.params[1].get_str();
    CKeyID committee;
    auto f = [&name, &committee](const CKeyID& key, const CRegisterCommitteeData& value) -> bool {
        bool ret = false;
        if(value.name == name) {
            committee = key;
            ret = true;
        }
        return ret;
    };

    if(Vote::GetInstance().GetCommittee().FindRegiste(f) == false) {
        return "The name dosn't registed";
    }
    data.committee = committee;

    CKeyID committeeid;
    CKeyID voterid;
    address.GetKeyID(voterid);
    auto f2 = [&committeeid, &voterid](const CKeyID& key, const std::map<CKeyID, uint64_t>& value) -> bool {
        if(value.find(voterid) != value.end()) {
            committeeid = key;
            return true;
        } else {
            return false;
        }
    };

    if(Vote::GetInstance().GetCommittee().FindVote(f2) == false) {
        ret = "The address don't voted committee";
    }

    if(ret.empty()) {
        ret = CheckStruct(data);
    }
    return ret;
}

string JsonToStruct(CBitcoinAddress& address, CSubmitBillData& data, const JSONRPCRequest& request)
{
    string ret;
    data.opcode = 0xc6;
    address = CBitcoinAddress(request.params[0].get_str());
    data.title = request.params[1].get_str();
    data.detail = request.params[2].get_str();
    data.url = request.params[3].get_str();

    int64_t t = stol(request.params[4].get_str());
    if(t <= 0 || t > 360) {
        return "parameter time invalid";
    }
    data.endtime = t * 3600 * 24 + time(NULL);
    for(uint8_t i = 5; i < request.params.size(); ++i) {
        data.options.push_back(request.params[i].get_str());
    }

    CKeyID id;
    address.GetKeyID(id);
    if (!address.IsValid()) {
        ret = "Invalid Bitcoin address!";
    } else if(Vote::GetInstance().GetCommittee().GetRegiste(NULL, id) == false) {
        ret = "The address don't registered";
    } else if(Vote::GetInstance().GetBill().GetRegiste(NULL, Hash160(data.title.begin(), data.title.end()))) {
        ret = "The bill has submited";
    }

    if(ret.empty()) {
        ret = CheckStruct(data);
    }
    return ret;
}

string JsonToStruct(CBitcoinAddress& address, CVoteBillData& data, const JSONRPCRequest& request)
{
    string ret;
    data.opcode = 0xc7;
    address = CBitcoinAddress(request.params[0].get_str());
    data.id.SetHex(request.params[1].get_str());
    data.index = (uint8_t)stoi(request.params[2].get_str());

    CKeyID id;
    address.GetKeyID(id);
    std::vector<std::map<CKeyID, uint64_t>> voters = Vote::GetInstance().GetBill().GetVote(data.id);
    for(auto& i : voters) {
        if(i.find(id) != i.end()) {
            return "This address has voted the bill";
        }
    }

    CSubmitBillData bill;
    Vote::GetInstance().GetBill().GetRegiste(&bill, data.id);
    if(bill.options.size() == 0) {
        ret = "bill no exited";
    } else if(data.index >= bill.options.size()) {
        ret = "option index Invalid";
    } else if((uint64_t)time(NULL) > bill.endtime) {
        ret = "the bill has completed";
    }

    if(ret.empty()) {
        ret = CheckStruct(data);
    }
    return ret;
}

UniValue registername(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw runtime_error(
            "registername address name"
            "\nregister address name.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "2. \"name\"                (string, required) The address name.\n"
            "\nResult:\n"
            "\"txid:\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("registername", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"testname\"")
            + HelpExampleRpc("registername", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"testname\"")
        );

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        return "Invalid Bitcoin address";

    std::string name = request.params[1].get_str();
    if(CheckStringFormat(name, 2, 16, true) == false)
        return "Invalid name";

    if(DposDB::GetInstance()->GetAddressName(request.params[0].get_str()).empty() == false)
        return "Address has registed";

    if(DposDB::GetInstance()->GetNameAddress(request.params[1].get_str()).empty() == false)
        return "Name has registed";

    CWalletTx wtx;
    LbtcPbMsg::RegisteNameMsg msg;
    msg.set_opid(1);
    msg.set_name(name);

    std::string data;
    msg.SerializeToString(&data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_REGISTER_COMMITTEE_FEE, AppID::DPOS, vector<unsigned char>(data.begin(), data.end()));

    return wtx.GetHash().GetHex();
}

UniValue getaddressname(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getaddressname address"
            "\nget address name.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "\nResult:\n"
            "\"name:\"                  (string) The address name.\n"
            "\nExamples:\n"
            + HelpExampleCli("getaddressname", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("getaddressname", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    CBitcoinAddress address(request.params[0].get_str());
    if(address.IsValid() == false)
        return "Invalid Bitcoin address";

    return DposDB::GetInstance()->GetAddressName(request.params[0].get_str());
}

UniValue getnameaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getnameaddress address"
            "\nget nmae address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"name:\"               (string, required) The address name.\n"
            "\nResult:\n"
            "\"address\"                (string, required) The lbtc address.\n"
            "\nExamples:\n"
            + HelpExampleCli("getnameaddress", "\"testname\"")
            + HelpExampleRpc("getnameaddress", "\"testname\"")
        );

    return DposDB::GetInstance()->GetNameAddress(request.params[0].get_str());
}

UniValue registercommittee(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 3 )
        throw runtime_error(
            "registercommittee address name"
            "\nregister committee.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "2. \"name\"                (string, required) The committee name.\n"
            "3. \"url\"                 (string, required) The url related the committee.\n"
            "\nResult:\n"
            "\"txid:\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("registercommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"testname\" \"www.test.com\"")
            + HelpExampleRpc("registercommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"testname\", \"www.test.com\"")
        );

    CBitcoinAddress address;
    CRegisterCommitteeData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_REGISTER_COMMITTEE_FEE, AppID::DPOS, opreturn);

    return wtx.GetHash().GetHex();
}

UniValue getcommittee(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getcommittee address\n"
            "\nget committee detail infomation.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The address of committee.\n"
            "\nResult:\n"
            "\"{\"\n"
            "\"    name:\"              (string) The committee name.\n"
            "\"    url:\"               (string) The committee related url.\n"
            "\"}\"\n"
            "\nExamples:\n"
            + HelpExampleCli("getcommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("getcommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    CBitcoinAddress address(request.params[0].get_str());

    CKeyID id;
    address.GetKeyID(id);

    CRegisterCommitteeData detail;
    bool ret = Vote::GetInstance().GetCommittee().GetRegiste(&detail, id);

    UniValue results(UniValue::VOBJ);

    if(ret) {
        std::map<CKeyID, uint64_t> voters = Vote::GetInstance().GetCommittee().GetVote(id);
        uint64_t nTotalVote = 0;
        for(auto& i : voters) {
            nTotalVote += Vote::GetInstance().GetBalance(i.first);
        }

        results.push_back(Pair("name",  detail.name));
        results.push_back(Pair("url",  detail.url));
        results.push_back(Pair("votes",  nTotalVote));
    }

    return results;
}

UniValue votecommittee(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() !=2)
        throw runtime_error(
            "votecommittee address committeename\n"
            "\nvote committee.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "2. \"committeename\"       (string, required) The committee name to be voting.\n"
            "\nResult:\n"
            "\"txid:\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("votecommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"testname\"")
            + HelpExampleRpc("votecommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"testname\"")
        );

    CBitcoinAddress address;
    CVoteCommitteeData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_VOTE_COMMITTEE_FEE, AppID::DPOS, opreturn);
    return wtx.GetHash().GetHex();
}

UniValue cancelvotecommittee(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() !=2)
        throw runtime_error(
            "cancelvotecommittee address committeename\n"
            "\ncancel vote committee.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "2. \"committeename\"       (string, required) The committee name to be cancel voting.\n"
            "\nResult:\n"
            "\"txid:\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("cancelvotecommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"testname\"")
            + HelpExampleRpc("cancelvotecommittee", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"testname\"")
        );

    CBitcoinAddress address;
    CCancelVoteCommitteeData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_VOTE_COMMITTEE_FEE, AppID::DPOS, opreturn);
    return wtx.GetHash().GetHex();
}

UniValue listcommitteevoters(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listcommitteevoters committeename\n"
            "\nlist committee received vote.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"committeename\"          (string, required) The committee name.\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    {\"\n"
            "\"        address:\"          (string) The voter address.\n"
            "\"    }\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listcommitteevoters", "\"test-name\"")
            + HelpExampleRpc("listcommitteevoters", "\"test-name\"")
        );

    string name = request.params[0].get_str();
    CKeyID address;
    auto f = [&name, &address](const CKeyID& key, const CRegisterCommitteeData& value) -> bool {
        bool ret = false;
        if(value.name == name) {
            address = key;
            ret = true;
        }
        return ret;
    };

    if(Vote::GetInstance().GetCommittee().FindRegiste(f) == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "committee not register");
    }

    std::map<CKeyID, uint64_t> voters = Vote::GetInstance().GetCommittee().GetVote(address);

    UniValue results(UniValue::VARR);
    for(auto& it : voters) {
        UniValue o(UniValue::VOBJ);
        o.push_back(Pair("address",  CBitcoinAddress(it.first).ToString()));
        results.push_back(o);
    }

    return results;
}

UniValue listcommitteebills(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listcommitteebills committeename\n"
            "\nlist the bills sumbit by the committee.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"committeename\"          (string, required) The committee name.\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    {\"\n"
            "\"        billid:\"          (string) The voter address.\n"
            "\"    }\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listcommitteebills", "\"test-name\"")
            + HelpExampleRpc("listcommitteebills", "\"test-name\"")
        );

    string name = request.params[0].get_str();
    CKeyID address;
    auto f = [&name, &address](const CKeyID& key, const CRegisterCommitteeData& value) -> bool {
        bool ret = false;
        if(value.name == name) {
            address = key;
            ret = true;
        }
        return ret;
    };

    if(Vote::GetInstance().GetCommittee().FindRegiste(f) == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "committee not register");
    }

    vector<uint160> billids;
    {
    auto f = [&address, &billids](const uint160& key, const CSubmitBillData& value) -> bool {
        if(value.committee == address) {
            billids.push_back(key);
        }
        return false;
    };

    Vote::GetInstance().GetBill().FindRegiste(f);
    }

    UniValue results(UniValue::VARR);
    for(auto& i : billids) {
        UniValue o(UniValue::VOBJ);
        o.push_back(Pair("billid",  i.GetHex()));
        results.push_back(o);
    }

    return results;
}

UniValue listcommittees(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
            "listcommittees\n"
            "\nlist all committees.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    {\"\n"
            "\"        address:\"               (string) The LBTC address.\n"
            "\"        name:\"                  (string) The committee name.\n"
            "\"        url:\"                   (string) The url related committee.\n"
            "\"    }\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listcommittees", "")
            + HelpExampleRpc("listcommittees", "")
        );

    vector<tuple<CKeyID, string, string>> committees;
    auto f = [&committees](const CKeyID& key, const CRegisterCommitteeData& value) -> bool {
        committees.push_back(make_tuple(key, value.name, value.url));
        return false;
    };

    Vote::GetInstance().GetCommittee().FindRegiste(f);

    UniValue results(UniValue::VARR);
    for(auto& i : committees){
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("address",  CBitcoinAddress(get<0>(i)).ToString()));
        entry.push_back(Pair("name",  get<1>(i)));
        entry.push_back(Pair("url",  get<2>(i)));
        results.push_back(entry);
    }

    return results;
}

UniValue listvotercommittees(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listvotercommittees address\n"
            "\nlist the voted committees.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"                 (string, required) The address of voter.\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    {\"\n"
            "\"        address:\"           (string) The committee address.\n"
            "\"        name:\"              (string) The committee name.\n"
            "\"    }\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listvotercommittees", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("listvotercommittees", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    CBitcoinAddress address(request.params[0].get_str());
    CKeyID voterid;
    address.GetKeyID(voterid);

    vector<CKeyID> committees;
    auto f = [&voterid, &committees](const CKeyID& key, const std::map<CKeyID, uint64_t>& value) -> bool {
        if(value.find(voterid) != value.end()) {
            committees.push_back(key);
        }
        return false;
    };

    Vote::GetInstance().GetCommittee().FindVote(f);

    UniValue results(UniValue::VARR);
    for(auto& it : committees) {
        CRegisterCommitteeData committee;
        Vote::GetInstance().GetCommittee().GetRegiste(&committee, it);
        UniValue o(UniValue::VOBJ);
        o.push_back(Pair("address", CBitcoinAddress(it).ToString()));
        o.push_back(Pair("name", committee.name));
        results.push_back(o);
    }

    return results;
}

UniValue submitbill(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 7 )
        throw runtime_error(
            "submitbill address title detail url endtime options\n"
            "\nsubmit bill.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "2. \"title\"               (string, required) The bill title. The title not allow empty and the max length of title is 128 bytes.\n"
            "3. \"detail\"              (string, required) The bill detail infomation. The max length of detail is 256 bytes.\n"
            "4. \"url\"                 (string, required) The bill related url. The max length of url  is 256 bytes.\n"
            "5. \"endtime\"             (numeric, required) The bill duration in days.\n"
            "6. \"options\"             (string, required) The bill option1. The max lengh of option is 256 bytes.\n"
            "7. \"options\"             (string, required) The bill option2.\n"
            "8. \"options\"             (string, required) The other options. The max number of option is 16.\n"
            "\nResult:\n"
            "\"{\"\n"
            "    \"txid:\"                   (string) The transaction id.\n"
            "    \"billid:\"                 (string) The bill id.\n"
            "\"}\"\n"
            "\nExamples:\n"
            + HelpExampleCli("submitbill", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"bill1\" \"modify test\" \"http://test.com/bill1\" \"24\" \"yes\" \"no\"")
            + HelpExampleRpc("submitbill", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"bill1\", \"modify test\", \"http://test.com/bill1\", \"24\", \"yes\", \"no\"")
        );

    CBitcoinAddress address;
    CSubmitBillData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    err = CheckStruct(data);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_SUBMIT_BILL_FEE, AppID::DPOS, opreturn);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("txid", wtx.GetHash().GetHex()));
    obj.push_back(Pair("billid", Hash160(data.title.begin(), data.title.end()).GetHex()));
    return obj;
}

UniValue votebill(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() !=3)
        throw runtime_error(
            "votebill address billid billoptionindex\n"
            "\nvote bill.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "2. \"billid\"              (string, required) The bill id voted.\n"
            "3. \"billoptionindex\"     (number, required) The index of this bill option.\n"
            "\nResult:\n"
            "\"txid:\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("votebill", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"c32418e7537b085bbf2cbada63320979c4e72936\" \"1\"")
            + HelpExampleRpc("votebill", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"c32418e7537b085bbf2cbada63320979c4e72936\", \"1\"")
        );

    CBitcoinAddress address;
    CVoteBillData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_VOTE_BILL_FEE, AppID::DPOS, opreturn);
    return wtx.GetHash().GetHex();
}

UniValue listbills(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
            "listbills\n"
            "\nlist all bills.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    {\"\n"
            "\"        id:\"                (string) The bill id.\n"
            "\"        title:\"             (string) The bill title.\n"
            "\"        isfinished:\"        (bool) When the value of isfinished is set true, it means the vote of bill is finished.\n"
            "\"        ispassed:\"          (bool) When the value of ispassed is set true, it means the vote of bill is passed.\n"
            "\"        optoinindex:\"       (bool) The option with this optionindex won the most votes.\n"
            "\"        totalvote:\"         (numeric) The bill total vote amount.\n"
            "\"    }\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listbills", "")
            + HelpExampleRpc("listbills", "")
        );

    vector<pair<uint160, string>> billids;
    auto f = [&billids](const uint160& key, const CSubmitBillData& value) -> bool {
        billids.push_back(make_pair(key, value.title));
        return false;
    };

    Vote::GetInstance().GetBill().FindRegiste(f);

    UniValue results(UniValue::VARR);
    for(auto it : billids){
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("id",  it.first.GetHex()));
        entry.push_back(Pair("title",  it.second));

        auto&& state = Vote::GetInstance().GetBill().GetState(it.first);
        entry.push_back(Pair("isfinished",  state.bFinished));
        entry.push_back(Pair("ispassed",  state.bPassed));
        entry.push_back(Pair("optoinindex",  state.nOptionIndex));
        entry.push_back(Pair("totalvote",  state.nTotalVote));
        results.push_back(entry);
    }

    return results;
}

UniValue listbillvoters(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listbillvoters\n"
            "\nlist the bill voters.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"billid\"                 (string, required) The bill id.\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    \"index:\"              (numeric) The index of bill options.\n"
            "\"    \"voters:\"             (array) The voters info.\n"
            "\"    [\"\n"
            "\"        {\"\n"
            "\"            address:\"      (string) The voter address.\n"
            "\"            votes:\"        (numeric) The number of votes.\n"
            "\"        }\"\n"
            "\"    ]\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listbillvoters", "\"c32418e7537b085bbf2cbada63320979c4e72936\"")
            + HelpExampleRpc("listbillvoters", "\"c32418e7537b085bbf2cbada63320979c4e72936\"")
        );

    uint160 id;
    id.SetHex(request.params[0].get_str());

    auto&& state = Vote::GetInstance().GetBill().GetState(id);
    bool bNeedFindBalance = !state.bFinished;
    std::vector<std::map<CKeyID, uint64_t>> voters = Vote::GetInstance().GetBill().GetVote(id);

    UniValue results(UniValue::VARR);
    for(uint8_t i = 0; i < voters.size(); ++i) {
        UniValue first(UniValue::VOBJ);
        first.push_back(Pair("index",  i));

        UniValue v(UniValue::VARR);
        for(auto& it : voters[i]) {
            UniValue o(UniValue::VOBJ);
            o.push_back(Pair("voters",  CBitcoinAddress(it.first).ToString()));
            if(bNeedFindBalance) {
                o.push_back(Pair("votes",  Vote::GetInstance().GetBalance(it.first)));
            } else {
                o.push_back(Pair("votes",  it.second));
            }
            v.push_back(o);
        }
        first.push_back(Pair("addresses", v));
        results.push_back(first);
    }

    return results;
}

UniValue listvoterbills(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listvoterbills address\n"
            "\nlist the voted bills.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"                 (string, required) The address of voter.\n"
            "\nResult:\n"
            "\"[\"\n"
            "\"    {\"\n"
            "\"        billid:\"                  (string) The voted bill id.\n"
            "\"        optionindex:\"             (string) The bill option id.\n"
            "\"    }\"\n"
            "\"]\"\n"
            "\nExamples:\n"
            + HelpExampleCli("listvoterbills", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("listvoterbills", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    CBitcoinAddress address(request.params[0].get_str());
    CKeyID voterid;
    address.GetKeyID(voterid);

    vector<pair<uint160, uint8_t>> bills;
    auto f = [&voterid, &bills](const uint160& key, const std::vector<std::map<CKeyID, uint64_t>>& value) -> bool {
        for(uint8_t i = 0; i < value.size(); ++i) {
            if(value[i].find(voterid) != value[i].end()) {
                bills.push_back(make_pair(key, i));
            }
        }
        return false;
    };

    Vote::GetInstance().GetBill().FindVote(f);

    UniValue results(UniValue::VARR);
    for(auto& it : bills) {
        UniValue o(UniValue::VOBJ);
        o.push_back(Pair("id", it.first.GetHex()));
        o.push_back(Pair("index", it.second));
        results.push_back(o);
    }

    return results;
}

UniValue getbill(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getbill billid\n"
            "\nget bill detail infomation.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"billid\"                 (string, required) The bill id.\n"
            "\nResult:\n"
            "\"{\"\n"
            "\"    tilte:\"                (string) The bill title.\n"
            "\"    detail:\"               (string) The bill detail.\n"
            "\"    url:\"                  (string) The url related the bill.\n"
            "\"    starttime:\"            (numeric) The bill endtime.\n"
            "\"    endtime:\"              (numeric) The bill endtime.\n"
            "\"    committee:\"            (string) The bill submit by the committee.\n"
            "\"    options:\"              (arrary) The bill options.\n"
            "\"        [\"\n"
            "\"            option:\"       (string) The bill option.\n"
            "\"        ]\"\n"
            "\"    state:\"                (object) The bill state.\n"
            "\"        {\"\n"
            "\"            id:\"           (string) The bill id.\n"
            "\"            title:\"        (string) The bill title.\n"
            "\"            isfinished:\"   (bool) When the value of isfinished is set true, it means the vote of bill is finished.\n"
            "\"            ispassed:\"     (bool) When the value of ispassed is set true, it means the vote of bill is passed.\n"
            "\"            optoinindex:\"  (bool) The option with this optionindex won the most votes.\n"
            "\"            totalvote:\"    (numeric) The bill total vote amount.\n"
            "\"            totalvote:\"    (numeric) The bill total vote amount.\n"
            "\"        }\"\n"
            "\"}\"\n"
            "\nExamples:\n"
            + HelpExampleCli("getbill", "\"c32418e7537b085bbf2cbada63320979c4e72936\"")
            + HelpExampleRpc("getbill", "\"c32418e7537b085bbf2cbada63320979c4e72936\"")
        );

    uint160 id;
    id.SetHex(request.params[0].get_str());

    CSubmitBillData detail;
    bool ret = Vote::GetInstance().GetBill().GetRegiste(&detail, id);

    UniValue results(UniValue::VOBJ);

    if(ret) {
        results.push_back(Pair("title",  detail.title));
        results.push_back(Pair("detail",  detail.detail));
        results.push_back(Pair("url",  detail.url));
        results.push_back(Pair("starttime",  detail.starttime));
        results.push_back(Pair("endtime",  detail.endtime));
        CBitcoinAddress address;
        address.Set(detail.committee);
        results.push_back(Pair("committee",  address.ToString()));
        UniValue v(UniValue::VARR);
        for(auto& it : detail.options) {
            UniValue o(UniValue::VOBJ);
            o.push_back(Pair("option", it));
            v.push_back(o);
        }
        results.push_back(Pair("options",  v));

        UniValue o(UniValue::VOBJ);
        auto&& state = Vote::GetInstance().GetBill().GetState(id);
        o.push_back(Pair("isfinished",  state.bFinished));
        o.push_back(Pair("ispassed",  state.bPassed));
        o.push_back(Pair("optoinindex",  state.nOptionIndex));
        o.push_back(Pair("totalvote",  state.nTotalVote));
        results.push_back(Pair("state",  o));
    }

    return results;
}

UniValue vote(const JSONRPCRequest& request) {
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2)
        throw runtime_error(
            "vote address delegateName1 deleagetNamen\n"
            "\nvote for delegates with this address，each voting will cost 0.01 lbtc."
            "\nA lbtc address can only vote for 51 delegates and can not vote for those already voted with this address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address which used for voting.\n"
            "2. \"delegateName1\"       (string, required) The name of delegate 1.\n"
            "3. \"delegateNamen\"       (string, required) The name of delegate N.\n"
            "\nResult:\n"
            "\"txid\"                   (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("vote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"delegater1\"")
            + HelpExampleCli("vote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"delegater2\" \"delegater3\"")
            + HelpExampleRpc("vote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"delegater1\"")
            + HelpExampleRpc("vote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"delegater2\", \"delegater3\"")
       );

    CBitcoinAddress address;
    CVoteForgerData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_VOTE_FORGER_FEE, AppID::DPOS, opreturn);

    return wtx.GetHash().GetHex();
}

UniValue cancelvote(const JSONRPCRequest& request) {
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2)
        throw runtime_error(
            "cancelvote address delegateName1 ... deleagetNamen\n"
            "\ncancelvote delegates which voted by this address.This address can only cancelvote"
            "\nthose delegates which voted by this address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The cancelvote on this address.\n"
            "2. \"delegateName1\"       (string, required) The delegate cancelvoted.\n"
            "3. \"delegateNamen\"       (string, required) The delegate cancelvoted.\n"
            "\nResult:\n"
            "\"txid\"                   (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("cancelvote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"delegater1\"")
            + HelpExampleCli("cancelvote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"delegater2\" \"delegater3\"")
            + HelpExampleRpc("cancelvote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"delegater1\"")
            + HelpExampleRpc("cancelvote", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"delegater2\", \"delegater3\"")
       );

    CBitcoinAddress address;
    CCancelVoteForgerData data;
    string err = JsonToStruct(address, data, request);
    if(err.empty() == false) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, err);
    }

    CWalletTx wtx;
    vector<unsigned char>&& opreturn = StructToData(data);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();

    SendWithOpreturn(address, wtx, OP_CANCEL_VOTE_FORGER_FEE, AppID::DPOS, opreturn);

    return wtx.GetHash().GetHex();
}

UniValue getaddressbalance(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getaddressbalance address\n"
            "\nget available balance lbtc(Satoshi) on address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"          (string, required) The lbtc address.\n"
            "\nResult:\n"
            "amount                  (numeric) The total amount lbtc.\n"
            "\nExamples:\n"
            + HelpExampleCli("getaddressbalance", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("getaddressbalance", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    uint64_t balance = 0;
    CTxDestination key = address.Get();
    if(address.IsScript()) {
        balance = Vote::GetInstance().GetAddressBalance(CMyAddress(boost::get<CScriptID>(key), CChainParams::SCRIPT_ADDRESS));
    } else {
        balance = Vote::GetInstance().GetAddressBalance(CMyAddress(boost::get<CKeyID>(key), CChainParams::PUBKEY_ADDRESS));
    }

    return UniValue(balance);
}

UniValue getcoinrank(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
            "getcoinrank number\n"
            "\nget lbtc coin rank.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"number\"           (string, optional) The number of address in top rank. Default 100.\n"
            "\nResult:\n"
            "[\n"
            "    {\n"
            "         \"address\": \"mkTLFbzw1YuLoRDSTXeDZbSbRaXMbFThCJ\", (string) The lbtc address\n"
            "         \"balance\": n,                                      (numeric) The balance of address\n"
            "    }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getcoinrank", "")
            + HelpExampleCli("getcoinrank", "\"100\"")
            + HelpExampleRpc("getcoinrank", "")
            + HelpExampleRpc("getcoinrank", "\"100\"")
        );

    int number = 100;
    if(request.params.size() == 1) {
        number = atoi(request.params[0].get_str().c_str());
        if(number < 0) {
            number = 100;
        }
    }

    std::multimap<uint64_t, CMyAddress> result = Vote::GetInstance().GetCoinRank(number);

    UniValue jsonResult(UniValue::VARR);
    for(auto it = result.rbegin(); it != result.rend(); ++it) {
        UniValue obj(UniValue::VOBJ);

        CBitcoinAddress a;
        if(it->second.second == CChainParams::PUBKEY_ADDRESS) {
            CKeyID id(it->second.first);
            a.Set(id);
        } else if(it->second.second == CChainParams::SCRIPT_ADDRESS) {
            CScriptID id(it->second.first);
            a.Set(id);
        }

        obj.push_back(Pair("address", a.ToString().c_str()));
        obj.push_back(Pair("balance", it->first));
        jsonResult.push_back(obj);
    }

    return jsonResult;
}

UniValue getcoindistribution(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1)
        throw runtime_error(
            "getcoindistribution threshold\n"
            "\nget lbtc coin distribution.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"threshold\"           (string, required) The Segmental threshold. At least one threshold\n"
            "\nResult:\n"
            "[\n"
            "    {\n"
            "         \"threshold\": n,    (numeric) Segmental threshold.\n"
            "         \"addresses\": n,    (numeric) The number of address.\n"
            "         \"coins\": n,        (numeric) The total amount of lbtc coin.\n"
            "    }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getcoindistribution", "\"10000\", \"1000000\"")
            + HelpExampleRpc("getcoindistribution", "\"100\", \"1000000\"")
        );

    std::set<uint64_t> distribution;
    for(uint8_t i=0; i < request.params.size(); ++i) {
        int64_t d = atol(request.params[i].get_str().c_str());
        if(d <= 0) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("para: ") + request.params[0].get_str() + string(" is negative"));
        }

        distribution.insert(d);
    }

    std::map<uint64_t, std::pair<uint64_t, uint64_t>> result =  Vote::GetInstance().GetCoinDistribution(distribution);

    UniValue jsonResult(UniValue::VARR);
    for(auto& it : result) {
        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("threshold", it.first));
        obj.push_back(Pair("addresses", it.second.first));
        obj.push_back(Pair("coins", it.second.second));
        jsonResult.push_back(obj);
    }

    return jsonResult;
}

UniValue getdelegatevotes(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getdelegatevotes delegateName\n"
            "\nget the number of votes the delegate received.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"delegateName\"      (string, required) The delegate name.\n"
            "\nResult:\n"
            "\"number\"               (numeric) The number of votes the delegate received.\n"
            "\nExamples:\n"
            + HelpExampleCli("getdelegatevotes", "\"delegateName\"")
            + HelpExampleRpc("getdelegatevotes", "\"delegateName\"")
        );

    Vote &vote = Vote::GetInstance();

    if(!vote.HaveDelegate(request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("delegate name: ") + request.params[0].get_str() + string(" not registe"));
    }

    uint64_t nShare{0};
    CKeyID key = vote.GetDelegate(request.params[0].get_str());
    nShare = Vote::GetInstance().GetDelegateVotes(key);

    UniValue entry(nShare);
    return entry;
}

UniValue getirreversibleblock(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getirreversibleblock\n"
            "\nget irreversible block height and hash.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "\nResult:\n"
            "{\n"
            "   \"height\"            (numeric) The block height.\n"
            "   \"hash\"              (string) The block hash.\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getirreversibleblock", "")
            + HelpExampleRpc("getirreversibleblock", "")
        );

    UniValue result(UniValue::VOBJ);
    auto&& info = DPoS::GetInstance().GetIrreversibleBlock();
    if(info.first > 0) {
        result.push_back(Pair("height", info.first));
        result.push_back(Pair("hash", info.second.ToString()));
    }
    return result;
}

UniValue getdelegatefunds(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getdelegatefunds delegateName\n"
            "\nget delegate the number of funds.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"delegateName\"      (string, required) The delegate name.\n"
            "\nResult:\n"
            "\"number\"               (numeric) The number of funds.\n"
            "\nExamples:\n"
            + HelpExampleCli("getdelegatefunds", "\"delegateName\"")
            + HelpExampleRpc("getdelegatefunds", "\"delegateName\"")
        );

    Vote &vote = Vote::GetInstance();

    if(!vote.HaveDelegate(request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("delegate name: ") + request.params[0].get_str() + string(" not registe"));
    }

    uint64_t nShare{0};
    CKeyID key = vote.GetDelegate(request.params[0].get_str());
    nShare = Vote::GetInstance().GetDelegateFunds(std::make_pair(key, CChainParams::PUBKEY_ADDRESS));

    UniValue entry(nShare);
    return entry;
}

UniValue listvoteddelegates(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listvoteddelegates address\n"
            "\nlist all the delegates voted by this address.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"address\"             (string, required) The lbtc address.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "     \"name\"              (string) The voted delegate name.\n"
            "     \"delegate\"          (string) The voted delegate address.\n"
            "  }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listvoteddelegates", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("listvoteddelegates", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    UniValue results(UniValue::VARR);
    CKeyID keyID;
    address.GetKeyID(keyID);

    auto result = Vote::GetInstance().GetVotedDelegates(keyID);
    for(auto& i : result)
    {
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("name", Vote::GetInstance().GetDelegate(i)));
        entry.push_back(Pair("delegate", CBitcoinAddress(i).ToString() ));
        results.push_back(entry);
    }

    return results;
}

UniValue listdelegates(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "listdelegates\n"
            "\nlist all delegates.\n"
            + HelpRequiringPassphrase() +
            "\nResult:\n"
            "[\n"
            "  {\n"
            "      \"name\"           (string) The delegate name.\n"
            "      \"address\"        (string) The delegate address.\n"
            "  }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listdelegates", "")
            + HelpExampleRpc("listdelegates", "")
        );
    UniValue results(UniValue::VARR);

    auto result = Vote::GetInstance().ListDelegates();
    for(auto w : result){
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("name", std::string(w.first) ));
        entry.push_back(Pair("address", CBitcoinAddress(w.second).ToString() ));
        results.push_back(entry);
    }

    return results;
}

UniValue listreceivedvotes(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "listreceivedvotes delegateName\n"
            "\nlist the all the addresses which vote the delegate.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"delegateName\"      (string, required) The delegate name.\n"
            "\nResult:\n"
            "[\n"
            "   \"address\"           (string) The addresses which vote the delegate.\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listreceivedvotes", "\"test-delegate-name\"")
            + HelpExampleRpc("listreceivedvotes", "\"test-delegate-name\"")
        );
    Vote& vote = Vote::GetInstance();
    CKeyID keyID = vote.GetDelegate(request.params[0].get_str());
    if(keyID.IsNull()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("delegate name: ") + request.params[0].get_str() + string(" not registe"));
    }

    CBitcoinAddress address(keyID);
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");

    std::set<CKeyID> voters = vote.GetDelegateVoters(keyID);
    UniValue results(UniValue::VARR);
    for (auto& v:voters)
    {
        results.push_back(CBitcoinAddress(v).ToString());
    }
    return results;
}

static uint32_t coins[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

UniValue createtoken(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 6)
        throw runtime_error(
            "createtoken tokenSymbol tokenName ownerAddress tokenAddress totalSupply decimal\n"
            "\ncreate a new token.\n"
            + HelpRequiringPassphrase() +
            "\nArguments:\n"
            "{\n"
            "1. \"tokenSymbol\"       (string, required) The token symbol.\n"
            "2. \"tokenName\"         (string, required) The token name.\n"
            "3. \"ownerAddress\"      (string, required) Creater's address.\n"
            "4. \"tokenAddress\"      (string, required) Token contract address.\n"
            "5. \"totalSupply\"       (numeric, required) Total amount of the token.\n"
            "6. \"decimal\"           (numeric, required) The token fund amount decimal.\n"
            "}\n"
            "\nResult:\n"
            "\"result\"               (string) The result description.\n"
            "\nExamples:\n"
            + HelpExampleCli("createtoken", "\"tokenSymbol\" \"tokenName\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\" \"100000000\" \"2\"")
            + HelpExampleRpc("createtoken", "\"tokenSymbol\", \"tokenName\", \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", \"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\", \"100000000\", \"2\"")
        );

    int32_t digits = atoll(request.params[5].get_str().c_str());
    int64_t totalamount = atoll(request.params[4].get_str().c_str());
    if(totalamount < 0 || totalamount > 100000000000)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid totalamount");
    if(digits < 0 || digits > 8)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid digits");

    LbtcPbMsg::CreateTokenMsg pbTokenMsg;
    pbTokenMsg.set_opid(CREATETOKEN);
    pbTokenMsg.set_symbol(request.params[0].get_str());
    pbTokenMsg.set_name(request.params[1].get_str());
    pbTokenMsg.set_tokenaddress(request.params[3].get_str());
    pbTokenMsg.set_totalamount((uint64_t)coins[digits] * totalamount);
    pbTokenMsg.set_digits((uint32_t)digits);

    std::string strErr = IsValid(pbTokenMsg);
    if(strErr.empty() == false)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strErr);

    std::string strMsg;
    pbTokenMsg.SerializeToString(&strMsg);

    if(DposDB::GetInstance()->GetAddressName(request.params[2].get_str()).empty())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Address has not registe name");

    CBitcoinAddress fromAddress(request.params[2].get_str());
    if (!fromAddress.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid owner address");
    if( TokenDB::GetInstance()->GetToken(pbTokenMsg.tokenaddress()))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Token address have been binded another token");

    std::map<int64_t, TokenInfo> mapTokenInfo = TokenDB::GetInstance()->GetTokens();
    for (auto& item : mapTokenInfo) {
        if(item.second.symbol == pbTokenMsg.symbol() && item.second.fromAddress == request.params[2].get_str()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Token has registerd by the address");
        }
    }

    CWalletTx wtx;
    vector<unsigned char> opreturn(strMsg.begin(), strMsg.end());
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    SendWithOpreturn(fromAddress, wtx, OP_CREATE_TOKEN_FEE, TOKEN, opreturn);
    return wtx.GetHash().GetHex();
}

UniValue sendtoken(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 4)
        throw runtime_error(
            "sendtoken tokenAddress fromAddress toAddress amount changeAddress commnet\n"
            "\nSent an amount from an address to another address."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
            "1. \"tokenAddress\"      (string, required) The token contract address.\n"
            "2. \"fromAddress\"       (string, required) The address to send funds from.\n"
            "3. \"toAddress\"         (string, required) The address to send funds to.\n"
            "4. \"amount\"        (numeric or string, required) The amount to send (transaction fee is added on top).\n"
            "5. \"changeAddress\"     (string, optional) The change address.\n"
            "6. \"comment\"           (string, optional) A comment used to store what the transaction is for. \n"
            "                                     This is not part of the transaction, just kept in your wallet.\n"
            "\nResult:\n"
            "\"txid\"                 (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("sendtoken", "\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\" 0.01 \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"Comment\"")
        );

    TokenDB* ptokendb = TokenDB::GetInstance();
    TokenInfo* tokenInfo = NULL;
    if(!(tokenInfo = ptokendb->GetToken(request.params[0].get_str())))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "token address not registed");

    uint64_t amount = 0;
    if(!ParseFixedPointUnsign(request.params[3].get_str(), tokenInfo->digits, &amount)
        || amount <= 0
        || amount > tokenInfo->totalamount) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "invalid amount");
    }

    LbtcPbMsg::TransferTokenMsg pbTokenMsg;
    std::string strPbMsg;
    pbTokenMsg.set_opid(TRANSFRERTOKEN);
    pbTokenMsg.set_dstaddress(request.params[2].get_str());
    pbTokenMsg.set_tokenid(tokenInfo->id);
    pbTokenMsg.set_amount(amount);
    if(request.params.size() == 6) {
        pbTokenMsg.set_comment(request.params[5].get_str());
    }

    int64_t fromAddressId =  ptokendb->GetAddressId(request.params[1].get_str());
    if(ptokendb->GetBalance(pbTokenMsg.tokenid(), fromAddressId) < amount) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "token balance insufficient");
    }

    std::string strErr = IsValid(pbTokenMsg);
    if(strErr.empty() == false)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strErr);

    CBitcoinAddress fromAddress(request.params[1].get_str());
    if (!fromAddress.IsValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid from address");
    }

    CBitcoinAddress changeAddress;

    if (request.params.size() > 4) {
        pbTokenMsg.set_comment(request.params[4].get_str());
    }
    pbTokenMsg.SerializeToString(&strPbMsg);

    CWalletTx wtx;
    vector<unsigned char> opreturn(strPbMsg.begin(), strPbMsg.end());
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    SendWithOpreturn(fromAddress, wtx, OP_SEND_TOKEN_FEE, TOKEN, opreturn);
    return wtx.GetHash().GetHex();
}

UniValue locktoken(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 5)
        throw runtime_error(
            "locktoken tokenAddress fromAddress toAddress amount changeAddress commnet\n"
            "\nlock an amount from an address to another address."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
            "1. \"tokenAddress\"      (string, required) The token contract address.\n"
            "2. \"fromAddress\"       (string, required) The address to lock funds from.\n"
            "3. \"toAddress\"         (string, required) The address to lock funds to.\n"
            "4. \"amount\"            (string, required) The amount to lock (transaction fee is added on top).\n"
            "5. \"heights\"           (string, required) Lock heights.\n"
            "6. \"changeAddress\"     (string, optional) The change address.\n"
            "7. \"comment\"           (string, optional) A comment used to store what the transaction is for. \n"
            "                                     This is not part of the transaction, just kept in your wallet.\n"
            "\nResult:\n"
            "\"txid\"                 (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("locktoken", "\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\" 0.01  100 \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"Comment\"")
        );

    TokenDB* ptokendb = TokenDB::GetInstance();
    TokenInfo* tokenInfo = NULL;
    if(!(tokenInfo = ptokendb->GetToken(request.params[0].get_str())))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "token address not registed");

    uint64_t amount = 0;
    if(!ParseFixedPointUnsign(request.params[3].get_str(), tokenInfo->digits, &amount)
        || amount <= 0
        || amount > tokenInfo->totalamount) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "invalid amount");
    }

    if(atoi(request.params[4].get_str().c_str()) <= 0) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "invalid lock blockheight");
    }

    uint64_t blockheight = 0;
    {
        LOCK(cs_main);
        blockheight = chainActive.Height();
    }

    LbtcPbMsg::LockTokenMsg pbTokenMsg;
    std::string strPbMsg;
    pbTokenMsg.set_opid(LOCKTOKEN);
    pbTokenMsg.set_dstaddress(request.params[2].get_str());
    pbTokenMsg.set_tokenid(tokenInfo->id);
    pbTokenMsg.set_amount(amount);
    pbTokenMsg.set_expiryheight(atoi(request.params[4].get_str().c_str()) + blockheight);
    if(request.params.size() == 7) {
        pbTokenMsg.set_comment(request.params[6].get_str());
    }

    int64_t fromAddressId =  ptokendb->GetAddressId(request.params[1].get_str());
    if(ptokendb->GetBalance(pbTokenMsg.tokenid(), fromAddressId) < amount) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "token balance insufficient");
    }

    std::string strErr = IsValid(pbTokenMsg);
    if(strErr.empty() == false)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strErr);

    CBitcoinAddress fromAddress(request.params[1].get_str());
    if (!fromAddress.IsValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid from address");
    }

    CBitcoinAddress changeAddress;

    if (request.params.size() > 5) {
        pbTokenMsg.set_comment(request.params[5].get_str());
    }
    pbTokenMsg.SerializeToString(&strPbMsg);

    CWalletTx wtx;

    vector<unsigned char> opreturn(strPbMsg.begin(), strPbMsg.end());
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    SendWithOpreturn(fromAddress, wtx, OP_LOCK_TOKEN_FEE, TOKEN, opreturn);
    return wtx.GetHash().GetHex();
}

UniValue gettokeninfo(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
            "gettokeninfo \"tokenAddress\" )\n"
            "\nGet token info."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
            "1. \"tokenAddress\"      (string, optional) The token address.\n"
            "\nResult:\n"
            "[                        (json array) Token info array.\n"
            "  {\n"
            "    \"tokenSymbol\"      (string) The token symbol.\n"
            "    \"tokenName\"        (string) The token name.\n"
            "    \"ownerAddress\"     (string) Creater's address.\n"
            "    \"tokenAddress\"     (string) Token address.\n"
            "    \"decimal\"          (numeric) The token fund amount decimal.\n"
            "    \"totalSupply\"      (numeric) Total amount of the token.\n"
            "  }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("gettokeninfo", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    std::string tokenAddress;
    if(request.params.size() == 1) {
        if(CBitcoinAddress(request.params[0].get_str()).IsValid() == false)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
        tokenAddress = request.params[0].get_str();
    }

    TokenDB* ptokendb = TokenDB::GetInstance();
    std::map<int64_t, TokenInfo> mapTokenInfo = ptokendb->GetTokens();
    UniValue results(UniValue::VARR);

    for (std::map<int64_t, TokenInfo>::iterator it = mapTokenInfo.begin(); it != mapTokenInfo.end(); ++it) {
        if(tokenAddress.empty() || tokenAddress == it->second.tokenAddress) {
            UniValue cTokenPiece(UniValue::VOBJ);
            cTokenPiece.push_back(Pair("tokenSymbol", it->second.symbol.c_str()));
            cTokenPiece.push_back(Pair("tokenName", it->second.name.c_str()));
            cTokenPiece.push_back(Pair("ownerAddress", it->second.fromAddress.c_str()));
            cTokenPiece.push_back(Pair("ownerName", DposDB::GetInstance()->GetAddressName(it->second.fromAddress.c_str()).c_str()));
            cTokenPiece.push_back(Pair("tokenAddress", it->second.tokenAddress.c_str()));
            cTokenPiece.push_back(Pair("decimal", it->second.digits));
            cTokenPiece.push_back(Pair("totalSupply", it->second.totalamount / coins[it->second.digits]));
            results.push_back(cTokenPiece);
        }
    }
    return results;
}

UniValue gettokenbalance(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "gettokenbalance \"tokenAddress\" \"useraddress\" )\n"
            "gettokenbalance \"useraddress\" )\n"
            "\nGet token balance."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments:\n"
            "1. \"userAddress\"       (string, required) The target address.\n"
            "2. \"tokenAddress\"      (string, optional) The token contract address.\n"
            "\nResult:\n"
            "[                        (json array) Token balance array.\n"
            "  {\n"
            "    \"tokenSymbol\"      (string) The token symbol.\n"
            "    \"availableBalance\" (numeric) Available balance.\n"
            "    \"lockBalance:\"     (json array) Lock balance array.\n"
            "    [\n"
            "      {\n"
            "        \"expiryHeight\" (numeric) Expiry height.\n"
            "        \"lockAmount\"   (numeric) Lock token amount.\n"
            "      }\n"
            "    ]\n"
            "  }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("gettokenbalance", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleCli("gettokenbalance", "\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
        );

    std::string strTokenAddress;
    std::string strUserAddress;
    strUserAddress = request.params[0].get_str();
    if(request.params.size() == 2) {
        strTokenAddress = request.params[1].get_str();
    }

    if((strTokenAddress.empty() == false && CBitcoinAddress(strTokenAddress).IsValid() == false)
        || CBitcoinAddress(strUserAddress).IsValid() == false)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");

    TokenDB* ptokendb = TokenDB::GetInstance();
    UniValue results(UniValue::VARR);

    std::map<int64_t, std::string> setTokenId;
    std::map<int64_t, TokenInfo> mapTokenInfo = ptokendb->GetTokens();
    for(auto& it : mapTokenInfo) {
        if(strTokenAddress.empty() == false) {
            if(strTokenAddress == it.second.tokenAddress) {
                setTokenId[it.first] = it.second.tokenAddress;
            }
        } else {
            setTokenId[it.first] = it.second.tokenAddress;
        }
    }

    uint64_t nAddressId = ptokendb->GetAddressId(strUserAddress);
    if(nAddressId == 0)
        return results;

    uint64_t balance = 0;
    for(auto& it : setTokenId) {
        balance = ptokendb->GetBalance(it.first, nAddressId);
        UniValue cTokenPiece(UniValue::VOBJ);
        cTokenPiece.push_back(Pair("tokenaddress", it.second));
        cTokenPiece.push_back(Pair("availablebalance", balance));

        UniValue cLockArray(UniValue::VARR);
        std::map<uint64_t, uint64_t>* mapLockBalance = ptokendb->GetLockBalance(it.first, nAddressId);
        if (mapLockBalance && mapLockBalance->empty() == false) {
            for(auto& item : *mapLockBalance) {
                UniValue cLockPiece(UniValue::VOBJ);
                cLockPiece.push_back(Pair("expiryheight", item.first));
                cLockPiece.push_back(Pair("amount", item.second));
                cLockArray.push_back(cLockPiece);
            }
            cTokenPiece.push_back(Pair("lockbalance", cLockArray));
        }

        results.push_back(cTokenPiece);
    }

    return results;
}


UniValue generateHolyBlocks(const JSONRPCRequest& request);

static const CRPCCommand commands[] =
{ //  category              name                        actor (function)           okSafeMode
    //  --------------------- ------------------------    -----------------------    ----------
    { "rawtransactions",    "fundrawtransaction",       &fundrawtransaction,       false,  {"hexstring","options"} },
    { "hidden",             "resendwallettransactions", &resendwallettransactions, true,   {} },
    { "wallet",             "abandontransaction",       &abandontransaction,       false,  {"txid"} },
    { "wallet",             "addmultisigaddress",       &addmultisigaddress,       true,   {"nrequired","keys","account"} },
    { "wallet",             "addwitnessaddress",        &addwitnessaddress,        true,   {"address"} },
    { "wallet",             "backupwallet",             &backupwallet,             true,   {"destination"} },
    { "wallet",             "bumpfee",                  &bumpfee,                  true,   {"txid", "options"} },
    { "wallet",             "dumpprivkey",              &dumpprivkey,              true,   {"address"}  },
    { "wallet",             "dumpwallet",               &dumpwallet,               true,   {"filename"} },
    { "wallet",             "encryptwallet",            &encryptwallet,            true,   {"passphrase"} },
    { "wallet",             "getaccountaddress",        &getaccountaddress,        true,   {"account"} },
    { "wallet",             "getaccount",               &getaccount,               true,   {"address"} },
    { "wallet",             "getaddressesbyaccount",    &getaddressesbyaccount,    true,   {"account"} },
    { "wallet",             "getbalance",               &getbalance,               false,  {"account","minconf","include_watchonly"} },
    { "wallet",             "getnewaddress",            &getnewaddress,            true,   {"account"} },
    { "wallet",             "getrawchangeaddress",      &getrawchangeaddress,      true,   {} },
    { "wallet",             "getreceivedbyaccount",     &getreceivedbyaccount,     false,  {"account","minconf"} },
    { "wallet",             "getreceivedbyaddress",     &getreceivedbyaddress,     false,  {"address","minconf"} },
    { "wallet",             "gettransaction",           &gettransaction,           false,  {"txid","include_watchonly"} },
    { "wallet",             "getunconfirmedbalance",    &getunconfirmedbalance,    false,  {} },
    { "wallet",             "getwalletinfo",            &getwalletinfo,            false,  {} },
    { "wallet",             "importmulti",              &importmulti,              true,   {"requests","options"} },
    { "wallet",             "importprivkey",            &importprivkey,            true,   {"privkey","label","rescan"} },
    { "wallet",             "importwallet",             &importwallet,             true,   {"filename"} },
    { "wallet",             "importaddress",            &importaddress,            true,   {"address","label","rescan","p2sh"} },
    { "wallet",             "importprunedfunds",        &importprunedfunds,        true,   {"rawtransaction","txoutproof"} },
    { "wallet",             "importpubkey",             &importpubkey,             true,   {"pubkey","label","rescan"} },
    { "wallet",             "keypoolrefill",            &keypoolrefill,            true,   {"newsize"} },
    { "wallet",             "listaccounts",             &listaccounts,             false,  {"minconf","include_watchonly"} },
    { "wallet",             "listaddressgroupings",     &listaddressgroupings,     false,  {} },
    { "wallet",             "listlockunspent",          &listlockunspent,          false,  {} },
    { "wallet",             "listreceivedbyaccount",    &listreceivedbyaccount,    false,  {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listreceivedbyaddress",    &listreceivedbyaddress,    false,  {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listsinceblock",           &listsinceblock,           false,  {"blockhash","target_confirmations","include_watchonly"} },
    { "wallet",             "listtransactions",         &listtransactions,         false,  {"account","count","skip","include_watchonly"} },
    { "wallet",             "listunspent",              &listunspent,              false,  {"minconf","maxconf","addresses","include_unsafe"} },
    { "wallet",             "lockunspent",              &lockunspent,              true,   {"unlock","transactions"} },
    { "wallet",             "move",                     &movecmd,                  false,  {"fromaccount","toaccount","amount","minconf","comment"} },
//    { "wallet",             "sendfrom",                 &sendfrom,                 false,  {"fromaccount","toaddress","amount","minconf","comment","comment_to"} },
    { "wallet",             "sendmany",                 &sendmany,                 false,  {"fromaccount","amounts", "fromaddress", "changeaddress", "minconf","comment","subtractfeefrom"} },
    { "wallet",             "sendtoaddress",            &sendtoaddress,         false,  {"address","amount","comment","comment_to","subtractfeefromamount"} },
    { "wallet",             "sendfromaddress",          &sendfromaddress,       false,  {"address","amount","comment","comment_to","subtractfeefromamount"} },
    { "wallet",             "setaccount",               &setaccount,               true,   {"address","account"} },
    { "wallet",             "settxfee",                 &settxfee,                 true,   {"amount"} },
    { "wallet",             "signmessage",              &signmessage,              true,   {"address","message"} },
    { "wallet",             "walletlock",               &walletlock,               true,   {} },
    { "wallet",             "walletpassphrasechange",   &walletpassphrasechange,   true,   {"oldpassphrase","newpassphrase"} },
    { "wallet",             "walletpassphrase",         &walletpassphrase,         true,   {"passphrase","timeout"} },
    { "wallet",             "removeprunedfunds",        &removeprunedfunds,        true,   {"txid"} },
    { "wallet",             "getaddressbalance",        &getaddressbalance,        true,   {"getaddressbalance", "address"} },
    { "wallet",             "getcoinrank",              &getcoinrank,              true,   {"getcoinrank"} },
    { "wallet",             "getcoindistribution",      &getcoindistribution,      true,   {"getcoindistribution", "threshold"} },
    { "dpos",               "register",                 &registe,                  true,   {"register", "address"} },
    { "dpos",               "vote",                     &vote,                     true,   {"vote", "fromaddress", "addresses"} },
    { "dpos",               "cancelvote",               &cancelvote,               true,   {"cancelvote", "fromaddress", "delegatename"} },
    { "dpos",               "listdelegates",            &listdelegates,            true,   {"listdelegates"} },
    { "dpos",               "getdelegatevotes",         &getdelegatevotes,         true,   {"getdelegatevotes", "delegatename"} },
    { "dpos",               "getdelegatefunds",         &getdelegatefunds,         true,   {"getdelegatefunds", "delegatename"} },
    { "dpos",               "listvoteddelegates",       &listvoteddelegates,       true,   {"listvoteddelegates", "address"} },
    { "dpos",               "listreceivedvotes",        &listreceivedvotes,        true,   {"listreceivedvotes", "delegatename"} },
    { "dpos",               "getirreversibleblock",     &getirreversibleblock,     true,   {"getirreversibleblock"} },
    { "dpos",               "registername",             &registername,             true,   {"registername", "name"} },
    { "dpos",               "createtoken",              &createtoken,              true,   {"createtoken", "tokenname"} },
    { "dpos",               "sendtoken",                &sendtoken,                true,   {"sendtoken", "tokenname"} },
    { "dpos",               "locktoken",                &locktoken,                true,   {"locktoken", "tokenname"} },
    { "dpos",               "gettokeninfo",             &gettokeninfo,             true,   {"gettokeninfo", "tokenname"} },
    { "dpos",               "gettokenbalance",          &gettokenbalance,          true,   {"gettokenbalance", "tokenname"} },
    { "dpos",               "getaddressname",           &getaddressname,           true,   {"getaddressname", "address"} },
    { "dpos",               "getnameaddress",           &getnameaddress,           true,   {"getnameaddress", "name"} },
    { "govern",             "submitbill",               &submitbill,               true,   {"submitbill"} },
    { "govern",             "votebill",                 &votebill,                 true,   {"votebill"} },
    { "govern",             "listbills",                &listbills,                true,   {"listbills"} },
    { "govern",             "getbill",                  &getbill,                  true,   {"getbill"} },
    { "govern",             "listbillvoters",           &listbillvoters,           true,   {"listbillvoters"} },
    { "govern",             "listvoterbills",           &listvoterbills,           true,   {"listvoterbills"} },
    { "govern",             "registercommittee",        &registercommittee,        true,   {"registercommittee"} },
    { "govern",             "votecommittee",            &votecommittee,            true,   {"votecommittee"} },
    { "govern",             "cancelvotecommittee",      &cancelvotecommittee,      true,   {"cancelvotecommittee"} },
    { "govern",             "listcommittees",           &listcommittees,           true,   {"listcommittees"} },
    { "govern",             "getcommittee",             &getcommittee,             true,   {"getcommittee"} },
    { "govern",             "listcommitteevoters",      &listcommitteevoters,      true,   {"listcommitteevoters"} },
    { "govern",             "listcommitteebills",       &listcommitteebills,       true,   {"listcommitteebills"} },
    { "govern",             "listvotercommittees",      &listvotercommittees,      true,   {"listvotercommittees"} },
};

void RegisterWalletRPCCommands(CRPCTable &t)
{
    if (GetBoolArg("-disablewallet", false))
        return;

    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
