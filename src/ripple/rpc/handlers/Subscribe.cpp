//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/net/RPCSub.h>
#include <ripple/rpc/impl/ParseAccountIds.h>
#include <ripple/server/Role.h>

namespace ripple {

Json::Value doSubscribe (RPC::Context& context)
{
    auto lock = getApp().masterLock();

    // FIXME: This needs to release the master lock immediately
    // Subscriptions need to be protected by their own lock

    InfoSub::pointer ispSub;
    Json::Value jvResult (Json::objectValue);
    std::uint32_t uLedgerIndex = context.params.isMember (jss::ledger_index)
            && context.params[jss::ledger_index].isNumeric ()
            ? context.params[jss::ledger_index].asUInt ()
            : 0;

    if (!context.infoSub && !context.params.isMember ("url"))
    {
        // Must be a JSON-RPC call.
        WriteLog (lsINFO, RPCHandler)
            << "doSubscribe: RPC subscribe requires a url";

        return rpcError (rpcINVALID_PARAMS);
    }

    if (context.params.isMember ("url"))
    {
        if (context.role != Role::ADMIN)
            return rpcError (rpcNO_PERMISSION);

        std::string strUrl      = context.params["url"].asString ();
        std::string strUsername = context.params.isMember ("url_username") ?
                context.params["url_username"].asString () : "";
        std::string strPassword = context.params.isMember ("url_password") ?
                context.params["url_password"].asString () : "";

        // DEPRECATED
        if (context.params.isMember ("username"))
            strUsername = context.params["username"].asString ();

        // DEPRECATED
        if (context.params.isMember ("password"))
            strPassword = context.params["password"].asString ();

        ispSub  = context.netOps.findRpcSub (strUrl);

        if (!ispSub)
        {
            WriteLog (lsDEBUG, RPCHandler)
                << "doSubscribe: building: " << strUrl;

            RPCSub::pointer rspSub = RPCSub::New (getApp ().getOPs (),
                getApp ().getIOService (), getApp ().getJobQueue (),
                    strUrl, strUsername, strPassword);
            ispSub  = context.netOps.addRpcSub (
                strUrl, std::dynamic_pointer_cast<InfoSub> (rspSub));
        }
        else
        {
            WriteLog (lsTRACE, RPCHandler)
                << "doSubscribe: reusing: " << strUrl;

            if (context.params.isMember ("username"))
                dynamic_cast<RPCSub*> (&*ispSub)->setUsername (strUsername);

            if (context.params.isMember ("password"))
                dynamic_cast<RPCSub*> (&*ispSub)->setPassword (strPassword);
        }
    }
    else
    {
        ispSub  = context.infoSub;
    }

    if (!context.params.isMember ("streams"))
    {
    }
    else if (!context.params["streams"].isArray ())
    {
        WriteLog (lsINFO, RPCHandler)
            << "doSubscribe: streams requires an array.";

        return rpcError (rpcINVALID_PARAMS);
    }
    else
    {
        for (auto& it: context.params["streams"])
        {
            if (it.isString ())
            {
                std::string streamName = it.asString ();

                if (streamName == "server")
                {
                    context.netOps.subServer (ispSub, jvResult,
                        context.role == Role::ADMIN);
                }
                else if (streamName == "ledger")
                {
                    context.netOps.subLedger (ispSub, jvResult);
                }
                else if (streamName == "transactions")
                {
                    context.netOps.subTransactions (ispSub);
                }
                else if (streamName == "transactions_proposed"
                         || streamName == "rt_transactions") // DEPRECATED
                {
                    context.netOps.subRTTransactions (ispSub);
                }
                else
                {
                    jvResult[jss::error]   = "unknownStream";
                }
            }
            else
            {
                jvResult[jss::error]   = "malformedStream";
            }
        }
    }

    std::string strAccountsProposed =
               context.params.isMember ("accounts_proposed")
               ? "accounts_proposed" : "rt_accounts";  // DEPRECATED

    if (!context.params.isMember (strAccountsProposed))
    {
    }
    else if (!context.params[strAccountsProposed].isArray ())
    {
        return rpcError (rpcINVALID_PARAMS);
    }
    else
    {
        auto ids  = RPC::parseAccountIds (context.params[strAccountsProposed]);

        if (ids.empty ())
            jvResult[jss::error] = "malformedAccount";
        else
            context.netOps.subAccount (ispSub, ids, uLedgerIndex, true);
    }

    if (!context.params.isMember ("accounts"))
    {
    }
    else if (!context.params["accounts"].isArray ())
    {
        return rpcError (rpcINVALID_PARAMS);
    }
    else
    {
        auto ids  = RPC::parseAccountIds (context.params["accounts"]);

        if (ids.empty ())
        {
            jvResult[jss::error]   = "malformedAccount";
        }
        else
        {
            context.netOps.subAccount (ispSub, ids, uLedgerIndex, false);
            WriteLog (lsDEBUG, RPCHandler)
                << "doSubscribe: accounts: " << ids.size ();
        }
    }

    bool bHaveMasterLock = true;
    if (!context.params.isMember ("books"))
    {
    }
    else if (!context.params["books"].isArray ())
    {
        return rpcError (rpcINVALID_PARAMS);
    }
    else
    {
        for (auto& j: context.params["books"])
        {
            if (!j.isObject ()
                    || !j.isMember (jss::taker_pays)
                    || !j.isMember (jss::taker_gets)
                    || !j[jss::taker_pays].isObject ()
                    || !j[jss::taker_gets].isObject ())
                return rpcError (rpcINVALID_PARAMS);

            Book book;
            bool bBoth =
                    (j.isMember ("both") && j["both"].asBool ()) ||
                    (j.isMember ("both_sides") && j["both_sides"].asBool ());
            bool bSnapshot =
                    (j.isMember ("snapshot") && j["snapshot"].asBool ()) ||
                    (j.isMember ("state_now") && j["state_now"].asBool ());
            // TODO(tom): both_sides and state_now are apparently deprecated...
            // where is this documented?

            Json::Value taker_pays = j[jss::taker_pays];
            Json::Value taker_gets = j[jss::taker_gets];

            // Parse mandatory currency.
            if (!taker_pays.isMember (jss::currency)
                    || !to_currency (book.in.currency,
                                     taker_pays[jss::currency].asString ()))
            {
                WriteLog (lsINFO, RPCHandler) << "Bad taker_pays currency.";

                return rpcError (rpcSRC_CUR_MALFORMED);
            }
            // Parse optional issuer.
            else if (((taker_pays.isMember (jss::issuer))
                      && (!taker_pays[jss::issuer].isString ()
                          || !to_issuer (book.in.account,
                                         taker_pays[jss::issuer].asString ())))
                     // Don't allow illegal issuers.
                     || (!book.in.currency != !book.in.account)
                     || noAccount() == book.in.account)
            {
                WriteLog (lsINFO, RPCHandler) << "Bad taker_pays issuer.";

                return rpcError (rpcSRC_ISR_MALFORMED);
            }

            // Parse mandatory currency.
            if (!taker_gets.isMember (jss::currency)
                    || !to_currency (book.out.currency,
                                     taker_gets[jss::currency].asString ()))
            {
                WriteLog (lsINFO, RPCHandler) << "Bad taker_pays currency.";

                return rpcError (rpcSRC_CUR_MALFORMED);
            }
            // Parse optional issuer.
            else if (((taker_gets.isMember (jss::issuer))
                      && (!taker_gets[jss::issuer].isString ()
                          || !to_issuer (book.out.account,
                                         taker_gets[jss::issuer].asString ())))
                     // Don't allow illegal issuers.
                     || (!book.out.currency != !book.out.account)
                     || noAccount() == book.out.account)
            {
                WriteLog (lsINFO, RPCHandler) << "Bad taker_gets issuer.";

                return rpcError (rpcDST_ISR_MALFORMED);
            }

            if (book.in.currency == book.out.currency
                    && book.in.account == book.out.account)
            {
                WriteLog (lsINFO, RPCHandler)
                    << "taker_gets same as taker_pays.";
                return rpcError (rpcBAD_MARKET);
            }

            RippleAddress   raTakerID;

            if (!j.isMember ("taker"))
                raTakerID.setAccountID (noAccount());
            else if (!raTakerID.setAccountID (j["taker"].asString ()))
                return rpcError (rpcBAD_ISSUER);

            if (!isConsistent (book))
            {
                WriteLog (lsWARNING, RPCHandler) << "Bad market: " << book;
                return rpcError (rpcBAD_MARKET);
            }

            context.netOps.subBook (ispSub, book);

            if (bBoth)
                context.netOps.subBook (ispSub, book);

            if (bSnapshot)
            {
                if (bHaveMasterLock)
                {
                    lock->unlock ();
                    bHaveMasterLock = false;
                }

                context.loadType = Resource::feeMediumBurdenRPC;
                auto lpLedger = getApp().getLedgerMaster ().
                        getPublishedLedger ();
                if (lpLedger)
                {
                    const Json::Value jvMarker = Json::Value (Json::nullValue);
                    Json::Value jvOffers (Json::objectValue);

                    auto add = [&](Json::StaticString field)
                    {
                        context.netOps.getBookPage (context.role == Role::ADMIN,
                            lpLedger, field == jss::asks ? reversed (book) : book,
                            raTakerID.getAccountID(), false, 0, jvMarker,
                            jvOffers);

                        if (jvResult.isMember (field))
                        {
                            Json::Value& results (jvResult[field]);
                            for (auto const& e : jvOffers[jss::offers])
                                results.append (e);
                        }
                        else
                        {
                            jvResult[field] = jvOffers[jss::offers];
                        }
                    };

                    if (bBoth)
                    {
                        add (jss::bids);
                        add (jss::asks);
                    }
                    else
                    {
                        add (jss::offers);
                    }
                }
            }
        }
    }

    return jvResult;
}

} // ripple
